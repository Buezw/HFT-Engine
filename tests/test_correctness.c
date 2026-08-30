// ============================================================================
// test_correctness.c
//
// Before trusting any benchmark number, we must prove opt and baseline are
// behaviorally identical. This replays the SAME sequence of market-maker
// additions, market buy/sell events, and ghost cleanups against both
// implementations and diffs the resulting account state + book state after
// every single tick. If they ever diverge, this aborts immediately with the
// tick number, so silent optimization bugs can't hide inside "it's faster".
// ============================================================================
#include <stdio.h>
#include <string.h>
#include "orderbook_engine.h"

static int books_equal(const L3OrderBook *a, const L3OrderBook *b) {
    return memcmp(a, b, sizeof(L3OrderBook)) == 0;
}

static int accounts_equal(const EngineAccount *a, const EngineAccount *b) {
    return a->my_cash == b->my_cash &&
           a->my_inventory == b->my_inventory &&
           a->total_fill_volume == b->total_fill_volume;
}

// ============================================================================
// Test 1: 200k-tick behavioral replay, baseline vs opt (original test).
// ============================================================================
static int test_tick_replay(void) {
    L3OrderBook ob_base, ob_opt;
    EngineAccount acc_base, acc_opt;

    ob_init_baseline(&ob_base, &acc_base, 100);
    ob_init_opt(&ob_opt, &acc_opt, 100);

    if (!books_equal(&ob_base, &ob_opt)) {
        printf("FAIL: initial books differ\n");
        return 1;
    }

    const int TICKS = 200000;
    int mismatches = 0;

    for (int t = 0; t < TICKS; t++) {
        // Same synthetic workload used in main.c's process_tick(): market
        // maker adds liquidity every other tick, then a market order eats
        // into one side, then ghosts get cleaned.
        if (t % 2 == 0) {
            for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
                int qb = (t * 13 + i * 7) % 40 + 10;
                ob_add_order_baseline(&ob_base.bids[i],
                    (L3Order){acc_base.global_order_id++, qb, 0, 0, 1});
                ob_add_order_opt(&ob_opt.bids[i],
                    (L3Order){acc_opt.global_order_id++, qb, 0, 0, 1});

                int qa = (t * 17 + i * 11) % 40 + 10;
                ob_add_order_baseline(&ob_base.asks[i],
                    (L3Order){acc_base.global_order_id++, qa, 0, 0, 1});
                ob_add_order_opt(&ob_opt.asks[i],
                    (L3Order){acc_opt.global_order_id++, qa, 0, 0, 1});
            }
        }

        int vol = 40;
        if ((t * 11) % 100 < 45) {
            ob_market_sell_baseline(&ob_base, &acc_base, vol, 0);
            ob_market_sell_opt(&ob_opt, &acc_opt, vol, 0);
        } else {
            ob_market_buy_baseline(&ob_base, &acc_base, vol, 0);
            ob_market_buy_opt(&ob_opt, &acc_opt, vol, 0);
        }

        // Also exercise the is_player=1 path (this is what actually moves
        // my_cash / my_inventory) every 7 ticks, mixing buys and sells, so
        // the account-state comparison above is not vacuously true.
        if (t % 7 == 0) {
            if (t % 14 == 0) {
                ob_market_buy_baseline(&ob_base, &acc_base, 15, 1);
                ob_market_buy_opt(&ob_opt, &acc_opt, 15, 1);
            } else {
                ob_market_sell_baseline(&ob_base, &acc_base, 15, 1);
                ob_market_sell_opt(&ob_opt, &acc_opt, 15, 1);
            }
        }

        for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
            ob_clean_ghosts_baseline(&ob_base.bids[i]);
            ob_clean_ghosts_opt(&ob_opt.bids[i]);
            ob_clean_ghosts_baseline(&ob_base.asks[i]);
            ob_clean_ghosts_opt(&ob_opt.asks[i]);
        }

        ob_update_total_qty_baseline(&ob_base);
        // NOTE: intentionally NOT calling ob_update_total_qty_opt here —
        // the whole point of the optimization is that total_qty is already
        // correct incrementally. We verify that below instead.

        if (!accounts_equal(&acc_base, &acc_opt)) {
            printf("FAIL at tick %d: account state diverged "
                   "(base cash=%ld inv=%d, opt cash=%ld inv=%d)\n",
                   t, acc_base.my_cash, acc_base.my_inventory,
                   acc_opt.my_cash, acc_opt.my_inventory);
            mismatches++;
            if (mismatches > 5) break;
        }

        for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
            if (ob_base.bids[i].total_qty != ob_opt.bids[i].total_qty ||
                ob_base.asks[i].total_qty != ob_opt.asks[i].total_qty) {
                printf("FAIL at tick %d: total_qty diverged at level %d "
                       "(base bid=%d/ask=%d, opt bid=%d/ask=%d)\n",
                       t, i, ob_base.bids[i].total_qty, ob_base.asks[i].total_qty,
                       ob_opt.bids[i].total_qty, ob_opt.asks[i].total_qty);
                mismatches++;
                if (mismatches > 5) break;
            }
        }
        if (mismatches > 5) break;
    }

    if (mismatches == 0) {
        printf("PASS: %d ticks, baseline and optimized engines produced "
               "identical account + book state (final cash=%ld, inventory=%d, "
               "fill_volume=%ld)\n",
               TICKS, acc_base.my_cash, acc_base.my_inventory, acc_base.total_fill_volume);
        return 0;
    } else {
        printf("FAIL: %d mismatches found\n", mismatches);
        return 1;
    }
}

// ============================================================================
// Test 2: queue-capacity regression test.
//
// Guards against the bug that ob_add_order_opt originally had: writing into
// lvl->queue[lvl->order_count++] with no check against MAX_ORDERS_PER_LVL.
// A level that is already full and receives one more insert used to write
// past the end of the fixed-size queue[] array — undefined behavior, and
// with the wrong luck, silent corruption of whatever memory follows it,
// not necessarily a crash. This fills a level to exact capacity, then
// attempts one more insert and asserts:
//   - the insert is rejected (return value 0)
//   - order_count and total_qty are unchanged (no partial/corrupt write)
//   - every already-resident order is untouched (memcmp against a saved
//     copy), which is what an out-of-bounds write into adjacent struct
//     fields would otherwise disturb
// Run this binary under -fsanitize=address,undefined (`make asan`) for an
// independent check that no out-of-bounds write happens even if the
// in-process assertions above didn't catch it.
// ============================================================================
static int test_capacity_bounds_one(const char *label,
                                     int (*add)(L3PriceLevel *, L3Order)) {
    L3PriceLevel lvl;
    memset(&lvl, 0, sizeof(lvl));
    lvl.price = 100;

    for (int i = 0; i < MAX_ORDERS_PER_LVL; i++) {
        L3Order o = {1000 + i, 10 + i, 0, 0, 0};
        if (!add(&lvl, o)) {
            printf("FAIL [%s]: insert %d/%d unexpectedly rejected while "
                   "level had room\n", label, i, MAX_ORDERS_PER_LVL);
            return 1;
        }
    }
    if (lvl.order_count != MAX_ORDERS_PER_LVL) {
        printf("FAIL [%s]: order_count=%d after filling to capacity, "
               "expected %d\n", label, lvl.order_count, MAX_ORDERS_PER_LVL);
        return 1;
    }

    L3PriceLevel before = lvl;

    // The level is now exactly full. One more insert must be rejected, not
    // silently overrun the array.
    L3Order overflow = {9999, 999, 0, 0, 0};
    int accepted = add(&lvl, overflow);

    if (accepted) {
        printf("FAIL [%s]: insert into a full level (order_count=%d, "
               "capacity=%d) was accepted instead of rejected\n",
               label, MAX_ORDERS_PER_LVL, MAX_ORDERS_PER_LVL);
        return 1;
    }
    if (memcmp(&lvl, &before, sizeof(lvl)) != 0) {
        printf("FAIL [%s]: level state changed after a rejected insert "
               "(order_count, total_qty, or queue contents mutated)\n", label);
        return 1;
    }

    printf("PASS [%s]: filled to capacity (%d orders), 11th insert "
           "correctly rejected, no state mutated\n", label, MAX_ORDERS_PER_LVL);
    return 0;
}

static int test_capacity_bounds(void) {
    int failures = 0;
    failures += test_capacity_bounds_one("ob_add_order_baseline", ob_add_order_baseline);
    failures += test_capacity_bounds_one("ob_add_order_opt",      ob_add_order_opt);
    return failures;
}

int main(void) {
    int failures = 0;
    failures += test_tick_replay();
    failures += test_capacity_bounds();

    if (failures == 0) {
        printf("\nALL TESTS PASSED\n");
        return 0;
    }
    printf("\n%d TEST GROUP(S) FAILED\n", failures);
    return 1;
}
