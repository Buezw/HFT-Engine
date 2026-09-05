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
#include <stdint.h>
#include "orderbook_engine.h"

static int books_equal(const L3OrderBook *a, const L3OrderBook *b) {
    return memcmp(a, b, sizeof(L3OrderBook)) == 0;
}

static int accounts_equal(const EngineAccount *a, const EngineAccount *b) {
    return a->my_cash == b->my_cash &&
           a->my_inventory == b->my_inventory &&
           a->total_fill_volume == b->total_fill_volume;
}

// Used by the cancel-replace tests below to verify a modified order
// actually landed where it should have, not just that some return value
// looked right.
static int order_live_at(const L3OrderBook *ob, int is_bid, int level, int id, int expected_qty) {
    const L3PriceLevel *lvl = is_bid ? &ob->bids[level] : &ob->asks[level];
    for (int i = 0; i < lvl->order_count; i++) {
        if (lvl->queue[i].order_id == id && lvl->queue[i].state != 2) {
            return lvl->queue[i].qty == expected_qty;
        }
    }
    return 0;
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

// ============================================================================
// Test 3: resting limit-order lifecycle (place / cancel), deterministic.
//
// Exercises the completed is_mine lifecycle described in orderbook_engine.h:
// reservation on placement, refund on cancel, rejection of invalid
// placements, and idempotency of cancel (can't cancel twice, can't cancel
// something that never existed). Run in lockstep against baseline and opt
// so any divergence between the two implementations shows up immediately.
// ============================================================================
static int test_limit_order_lifecycle(void) {
    L3OrderBook ob_base, ob_opt;
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(&ob_base, &acc_base, 100);
    ob_init_opt(&ob_opt, &acc_opt, 100);

    int fails = 0;
    long cash_before_base = acc_base.my_cash, cash_before_opt = acc_opt.my_cash;

    int buy_id_base = ob_place_limit_buy_baseline(&ob_base, &acc_base, 0, 20);
    int buy_id_opt  = ob_place_limit_buy_opt(&ob_opt, &acc_opt, 0, 20);
    if (buy_id_base < 0 || buy_id_opt < 0 || buy_id_base != buy_id_opt) {
        printf("FAIL: limit buy placement ids diverged or failed (base=%d opt=%d)\n",
               buy_id_base, buy_id_opt);
        fails++;
    }
    long expect_reserved = (long)20 * ob_base.bids[0].price;
    if (acc_base.my_cash != cash_before_base - expect_reserved ||
        acc_opt.my_cash  != cash_before_opt  - expect_reserved) {
        printf("FAIL: limit buy did not reserve cash correctly (base=%ld opt=%ld, expected -%ld)\n",
               acc_base.my_cash, acc_opt.my_cash, expect_reserved);
        fails++;
    }

    // Invalid placements must be rejected, and reject with -1, on both.
    if (ob_place_limit_buy_baseline(&ob_base, &acc_base, MAX_PRICE_LEVELS, 10) != -1 ||
        ob_place_limit_buy_opt(&ob_opt, &acc_opt, MAX_PRICE_LEVELS, 10) != -1) {
        printf("FAIL: out-of-range level was accepted\n");
        fails++;
    }
    if (ob_place_limit_buy_baseline(&ob_base, &acc_base, 0, 0) != -1 ||
        ob_place_limit_buy_opt(&ob_opt, &acc_opt, 0, 0) != -1) {
        printf("FAIL: zero-qty placement was accepted\n");
        fails++;
    }
    // No inventory yet — selling should be rejected, not allowed to go negative.
    if (ob_place_limit_sell_baseline(&ob_base, &acc_base, 0, 5) != -1 ||
        ob_place_limit_sell_opt(&ob_opt, &acc_opt, 0, 5) != -1) {
        printf("FAIL: sell placement with insufficient inventory was accepted\n");
        fails++;
    }

    // Acquire inventory via a market buy (is_player=1), then place a limit sell.
    ob_market_buy_baseline(&ob_base, &acc_base, 30, 1);
    ob_market_buy_opt(&ob_opt, &acc_opt, 30, 1);
    if (acc_base.my_inventory != acc_opt.my_inventory || acc_base.my_inventory <= 0) {
        printf("FAIL: setup market buy diverged (base=%d opt=%d)\n",
               acc_base.my_inventory, acc_opt.my_inventory);
        fails++;
    }
    int inv_before = acc_base.my_inventory;
    int sell_id_base = ob_place_limit_sell_baseline(&ob_base, &acc_base, 1, inv_before);
    int sell_id_opt  = ob_place_limit_sell_opt(&ob_opt, &acc_opt, 1, inv_before);
    if (sell_id_base < 0 || sell_id_opt < 0 || sell_id_base != sell_id_opt ||
        acc_base.my_inventory != 0 || acc_opt.my_inventory != 0) {
        printf("FAIL: limit sell placement/inventory reservation diverged\n");
        fails++;
    }

    // Cancel the buy: cash must come back by exactly the reserved amount.
    // (Not back to cash_before_base/opt — the market buy used to acquire
    // inventory above was a real trade in between, not a reservation, so
    // it permanently changed cash; only the limit-buy's reservation is
    // refundable here.)
    long cash_before_cancel_base = acc_base.my_cash, cash_before_cancel_opt = acc_opt.my_cash;
    if (!ob_cancel_order_baseline(&ob_base, &acc_base, buy_id_base) ||
        !ob_cancel_order_opt(&ob_opt, &acc_opt, buy_id_opt)) {
        printf("FAIL: cancelling a live resting order was rejected\n");
        fails++;
    }
    if (acc_base.my_cash != cash_before_cancel_base + expect_reserved ||
        acc_opt.my_cash  != cash_before_cancel_opt  + expect_reserved) {
        printf("FAIL: cancel did not refund cash exactly (base=%ld opt=%ld, expected %ld/%ld)\n",
               acc_base.my_cash, acc_opt.my_cash,
               cash_before_cancel_base + expect_reserved, cash_before_cancel_opt + expect_reserved);
        fails++;
    }

    // Cancelling again must fail — already ghosted, no double refund.
    if (ob_cancel_order_baseline(&ob_base, &acc_base, buy_id_base) ||
        ob_cancel_order_opt(&ob_opt, &acc_opt, buy_id_opt)) {
        printf("FAIL: double-cancel was accepted (would double-refund)\n");
        fails++;
    }
    // An id that never existed must also just fail, not crash.
    if (ob_cancel_order_baseline(&ob_base, &acc_base, 999999) ||
        ob_cancel_order_opt(&ob_opt, &acc_opt, 999999)) {
        printf("FAIL: cancelling a nonexistent order id was accepted\n");
        fails++;
    }

    // Sync baseline's total_qty (opt is already incrementally correct) and
    // diff full state — this is the same-shape check test_tick_replay does.
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        ob_clean_ghosts_baseline(&ob_base.bids[i]);
        ob_clean_ghosts_opt(&ob_opt.bids[i]);
        ob_clean_ghosts_baseline(&ob_base.asks[i]);
        ob_clean_ghosts_opt(&ob_opt.asks[i]);
    }
    ob_update_total_qty_baseline(&ob_base);
    if (!accounts_equal(&acc_base, &acc_opt) || !books_equal(&ob_base, &ob_opt)) {
        printf("FAIL: baseline/opt diverged after limit order lifecycle sequence\n");
        fails++;
    }

    if (fails == 0) {
        printf("PASS: limit order lifecycle (place/reserve, invalid rejection, "
               "cancel/refund, double-cancel rejection) identical on baseline and opt\n");
    }
    return fails;
}

// ============================================================================
// Test 4: randomized stress test, baseline vs opt, diffed every step.
//
// The fixed synthetic workloads above are good regression tests but only
// exercise the sequences someone thought to write. This drives both engines
// with the same pseudo-random sequence of market orders, limit placements,
// and cancels (including cancelling ids that were never issued, or were
// already filled/cancelled) and diffs full state after every single
// operation, not just periodically — so a divergence introduced by any
// future change to either implementation is caught at the exact operation
// that caused it, not somewhere downstream.
//
// Uses a small xorshift32 PRNG with a fixed seed instead of rand(), so the
// exact same sequence reproduces on every platform/libc, not just this one.
// ============================================================================
static uint32_t rng_state;
static uint32_t xorshift32(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}
static int rand_range(int lo, int hi) { // inclusive
    return lo + (int)(xorshift32() % (uint32_t)(hi - lo + 1));
}

static int test_random_stress(void) {
    L3OrderBook ob_base, ob_opt;
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(&ob_base, &acc_base, 100);
    ob_init_opt(&ob_opt, &acc_opt, 100);

    rng_state = 0xC0FFEEu; // fixed seed: reproducible failures, not flaky ones

    #define MAX_ISSUED 4096
    int issued_ids[MAX_ISSUED];
    int issued_count = 0;

    const int ITERATIONS = 100000;
    int fails = 0;

    for (int i = 0; i < ITERATIONS && fails == 0; i++) {
        int action = rand_range(0, 99);
        int level = rand_range(0, MAX_PRICE_LEVELS - 1);
        int qty = rand_range(1, 25);

        if (action < 35) {
            // Market order, mix of player and non-player, buy and sell.
            int is_player = rand_range(0, 3) == 0; // 25% player fills
            if (rand_range(0, 1)) {
                ob_market_buy_baseline(&ob_base, &acc_base, qty, is_player);
                ob_market_buy_opt(&ob_opt, &acc_opt, qty, is_player);
            } else {
                ob_market_sell_baseline(&ob_base, &acc_base, qty, is_player);
                ob_market_sell_opt(&ob_opt, &acc_opt, qty, is_player);
            }
        } else if (action < 70) {
            // Limit placement, buy or sell.
            int id_base, id_opt;
            if (rand_range(0, 1)) {
                id_base = ob_place_limit_buy_baseline(&ob_base, &acc_base, level, qty);
                id_opt  = ob_place_limit_buy_opt(&ob_opt, &acc_opt, level, qty);
            } else {
                id_base = ob_place_limit_sell_baseline(&ob_base, &acc_base, level, qty);
                id_opt  = ob_place_limit_sell_opt(&ob_opt, &acc_opt, level, qty);
            }
            if (id_base != id_opt) {
                printf("FAIL at iter %d: placement id diverged (base=%d opt=%d)\n",
                       i, id_base, id_opt);
                fails++;
            }
            if (id_base >= 0 && issued_count < MAX_ISSUED) {
                issued_ids[issued_count++] = id_base;
            }
        } else if (action < 90 && issued_count > 0) {
            // Cancel a previously-issued id (may already be filled/cancelled —
            // that's an intentional, exercised path, not skipped).
            int id = issued_ids[rand_range(0, issued_count - 1)];
            int r_base = ob_cancel_order_baseline(&ob_base, &acc_base, id);
            int r_opt  = ob_cancel_order_opt(&ob_opt, &acc_opt, id);
            if (r_base != r_opt) {
                printf("FAIL at iter %d: cancel(%d) result diverged (base=%d opt=%d)\n",
                       i, id, r_base, r_opt);
                fails++;
            }
        } else {
            // Cancel an id that (almost certainly) was never issued.
            int bogus_id = rand_range(2000000, 3000000);
            int r_base = ob_cancel_order_baseline(&ob_base, &acc_base, bogus_id);
            int r_opt  = ob_cancel_order_opt(&ob_opt, &acc_opt, bogus_id);
            if (r_base || r_opt) {
                printf("FAIL at iter %d: cancel of a bogus id was accepted "
                       "(base=%d opt=%d)\n", i, r_base, r_opt);
                fails++;
            }
        }

        for (int lvl = 0; lvl < MAX_PRICE_LEVELS; lvl++) {
            ob_clean_ghosts_baseline(&ob_base.bids[lvl]);
            ob_clean_ghosts_opt(&ob_opt.bids[lvl]);
            ob_clean_ghosts_baseline(&ob_base.asks[lvl]);
            ob_clean_ghosts_opt(&ob_opt.asks[lvl]);
        }
        ob_update_total_qty_baseline(&ob_base);

        if (!accounts_equal(&acc_base, &acc_opt)) {
            printf("FAIL at iter %d: account state diverged (base cash=%ld inv=%d, "
                   "opt cash=%ld inv=%d)\n", i, acc_base.my_cash, acc_base.my_inventory,
                   acc_opt.my_cash, acc_opt.my_inventory);
            fails++;
        }
        if (!books_equal(&ob_base, &ob_opt)) {
            printf("FAIL at iter %d: book state diverged\n", i);
            fails++;
        }
    }
    #undef MAX_ISSUED

    if (fails == 0) {
        printf("PASS: %d randomized operations (market/limit/cancel, mixed valid "
               "and invalid), baseline and opt stayed identical after every single "
               "operation (final cash=%ld, inventory=%d, %d order ids issued)\n",
               ITERATIONS, acc_base.my_cash, acc_base.my_inventory, issued_count);
    }
    return fails;
}

// ============================================================================
// Test 5: pre-trade position risk limit.
//
// main.c's player_market_sell has no position floor at all — test_random_stress
// above (which calls the RAW, unwrapped ob_market_sell_*) drove my_inventory to
// -168,805 over 100k operations with nothing stopping it. ob_market_buy/sell_
// risk_checked_* wrap the same matching functions with a pre-trade clip to
// MAX_POSITION instead of modifying them (see header comment for why: baseline
// has to stay a faithful port for the whole correctness methodology to mean
// anything).
//
// Two tests: the clipping arithmetic in isolation (drive my_inventory to a
// known distance from the boundary directly, so this doesn't depend on the
// book having enough liquidity to fill any particular amount), and an
// end-to-end randomized run asserting the invariant |my_inventory| <=
// MAX_POSITION actually holds through real matching, not just in the
// wrapper's arithmetic.
// ============================================================================
static int test_risk_limit_clipping(void) {
    int fails = 0;
    L3OrderBook ob; EngineAccount acc;

    // Near the long limit: only 5 units of room should get through, no
    // matter how much was requested or how much the book could fill.
    ob_init_opt(&ob, &acc, 100);
    acc.my_inventory = MAX_POSITION - 5;
    int sub = ob_market_buy_risk_checked_opt(&ob, &acc, 9999);
    if (sub != 5) {
        printf("FAIL: risk-checked buy 5 units from the long limit submitted %d, expected 5\n", sub);
        fails++;
    }

    // Exactly at the limit: zero room. 0 submitted is success, not an
    // error — the check did exactly its job.
    ob_init_opt(&ob, &acc, 100);
    acc.my_inventory = MAX_POSITION;
    sub = ob_market_buy_risk_checked_opt(&ob, &acc, 10);
    if (sub != 0 || acc.my_inventory != MAX_POSITION) {
        printf("FAIL: risk-checked buy exactly at the limit submitted %d (inventory now %d), "
               "expected 0 submitted and inventory unchanged\n", sub, acc.my_inventory);
        fails++;
    }

    // Symmetric on the short side.
    ob_init_opt(&ob, &acc, 100);
    acc.my_inventory = -(MAX_POSITION - 7);
    sub = ob_market_sell_risk_checked_opt(&ob, &acc, 9999);
    if (sub != 7) {
        printf("FAIL: risk-checked sell 7 units from the short limit submitted %d, expected 7\n", sub);
        fails++;
    }

    // Well within room: this is a limiter, not something that always
    // clips — a small request must pass through unchanged.
    ob_init_opt(&ob, &acc, 100);
    acc.my_inventory = 0;
    sub = ob_market_buy_risk_checked_opt(&ob, &acc, 3);
    if (sub != 3) {
        printf("FAIL: risk-checked buy well within room clipped a 3-unit request to %d\n", sub);
        fails++;
    }

    // Baseline must clip identically to opt.
    ob_init_baseline(&ob, &acc, 100);
    acc.my_inventory = MAX_POSITION - 5;
    sub = ob_market_buy_risk_checked_baseline(&ob, &acc, 9999);
    if (sub != 5) {
        printf("FAIL: baseline risk-checked buy 5 units from the long limit submitted %d, expected 5\n", sub);
        fails++;
    }
    ob_init_baseline(&ob, &acc, 100);
    acc.my_inventory = -(MAX_POSITION - 7);
    sub = ob_market_sell_risk_checked_baseline(&ob, &acc, 9999);
    if (sub != 7) {
        printf("FAIL: baseline risk-checked sell 7 units from the short limit submitted %d, expected 7\n", sub);
        fails++;
    }

    if (fails == 0) {
        printf("PASS: risk-limit clipping arithmetic (near-limit, exactly-at-limit, "
               "within-room, both sides, baseline and opt) all correct\n");
    }
    return fails;
}

static int test_risk_limit_end_to_end(void) {
    L3OrderBook ob_base, ob_opt;
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(&ob_base, &acc_base, 100);
    ob_init_opt(&ob_opt, &acc_opt, 100);

    rng_state = 0xBADA55u; // different fixed seed from test_random_stress, still reproducible
    int fails = 0;
    const int ITERATIONS = 20000;

    for (int i = 0; i < ITERATIONS && fails == 0; i++) {
        // Keep the book stocked so risk-checked orders have real liquidity
        // to walk into, same market-maker pattern used elsewhere.
        for (int lvl = 0; lvl < MAX_PRICE_LEVELS; lvl++) {
            int qb = rand_range(10, 40);
            ob_add_order_baseline(&ob_base.bids[lvl], (L3Order){acc_base.global_order_id++, qb, 0, 0, 1});
            ob_add_order_opt(&ob_opt.bids[lvl], (L3Order){acc_opt.global_order_id++, qb, 0, 0, 1});
            int qa = rand_range(10, 40);
            ob_add_order_baseline(&ob_base.asks[lvl], (L3Order){acc_base.global_order_id++, qa, 0, 0, 1});
            ob_add_order_opt(&ob_opt.asks[lvl], (L3Order){acc_opt.global_order_id++, qa, 0, 0, 1});
        }

        int qty = rand_range(1, 60); // deliberately can exceed MAX_POSITION in a single call
        int sub_base, sub_opt;
        if (rand_range(0, 1)) {
            sub_base = ob_market_buy_risk_checked_baseline(&ob_base, &acc_base, qty);
            sub_opt  = ob_market_buy_risk_checked_opt(&ob_opt, &acc_opt, qty);
        } else {
            sub_base = ob_market_sell_risk_checked_baseline(&ob_base, &acc_base, qty);
            sub_opt  = ob_market_sell_risk_checked_opt(&ob_opt, &acc_opt, qty);
        }
        if (sub_base != sub_opt) {
            printf("FAIL at iter %d: risk-checked submitted qty diverged (base=%d opt=%d)\n",
                   i, sub_base, sub_opt);
            fails++;
        }
        if (acc_base.my_inventory > MAX_POSITION || acc_base.my_inventory < -MAX_POSITION ||
            acc_opt.my_inventory  > MAX_POSITION || acc_opt.my_inventory  < -MAX_POSITION) {
            printf("FAIL at iter %d: position limit breached (base inv=%d, opt inv=%d, limit=%d)\n",
                   i, acc_base.my_inventory, acc_opt.my_inventory, MAX_POSITION);
            fails++;
        }

        for (int lvl = 0; lvl < MAX_PRICE_LEVELS; lvl++) {
            ob_clean_ghosts_baseline(&ob_base.bids[lvl]);
            ob_clean_ghosts_opt(&ob_opt.bids[lvl]);
            ob_clean_ghosts_baseline(&ob_base.asks[lvl]);
            ob_clean_ghosts_opt(&ob_opt.asks[lvl]);
        }
        ob_update_total_qty_baseline(&ob_base);

        if (!accounts_equal(&acc_base, &acc_opt) || !books_equal(&ob_base, &ob_opt)) {
            printf("FAIL at iter %d: baseline/opt diverged under risk-checked trading\n", i);
            fails++;
        }
    }

    if (fails == 0) {
        printf("PASS: %d risk-checked market orders, |inventory| never exceeded "
               "MAX_POSITION=%d on either engine, baseline and opt stayed identical "
               "(final inventory base=%d opt=%d)\n",
               ITERATIONS, MAX_POSITION, acc_base.my_inventory, acc_opt.my_inventory);
    }
    return fails;
}

// ============================================================================
// Test 6: O(1) cancel-by-id index (ob_cancel_order_opt_indexed).
//
// The index is purely a performance layer: for any given sequence of
// placements/cancels, it must produce EXACTLY the same account and book
// mutations as the existing, already-proven ob_cancel_order_opt. Three
// things are checked, in order:
//   6a. the O(1) fast path itself (cancel immediately after placement,
//       cached slot still valid, no compaction has run)
//   6b. the bounded single-level fallback (force a compaction that shifts
//       the target order to a new slot within its level *before*
//       cancelling it, so the cached slot is stale and the fallback scan
//       has to actually run and still find it)
//   6c. behavioral equivalence with ob_cancel_order_opt under the same
//       randomized mixed workload used in test_random_stress, diffing
//       full book+account state after every operation
// ============================================================================
static int test_indexed_cancel_fast_path(void) {
    L3OrderBook ob; EngineAccount acc;
    static OrderIndex idx; // 128KB (ORDER_INDEX_CAPACITY buckets) — static, not on the stack
    ob_init_opt(&ob, &acc, 100);
    ob_index_init(&idx);
    int fails = 0;

    long cash_before = acc.my_cash;
    int id = ob_place_limit_buy_opt_indexed(&ob, &acc, &idx, 0, 20);
    if (id < 0) {
        printf("FAIL: indexed limit buy placement rejected unexpectedly\n");
        return 1;
    }
    long expect_reserved = (long)20 * ob.bids[0].price;
    if (acc.my_cash != cash_before - expect_reserved) {
        printf("FAIL: indexed placement did not reserve cash correctly\n");
        fails++;
    }

    // No clean_ghosts call in between: the cached (level, slot) from
    // placement must still be exactly right, so this hits the O(1) path,
    // not the fallback scan.
    if (!ob_cancel_order_opt_indexed(&ob, &acc, &idx, id)) {
        printf("FAIL: indexed cancel of a freshly-placed order was rejected\n");
        fails++;
    }
    if (acc.my_cash != cash_before) {
        printf("FAIL: indexed cancel did not refund cash exactly (got %ld, expected %ld)\n",
               acc.my_cash, cash_before);
        fails++;
    }

    // Double-cancel and bogus-id must both fail cleanly, same contract as
    // ob_cancel_order_opt.
    if (ob_cancel_order_opt_indexed(&ob, &acc, &idx, id)) {
        printf("FAIL: indexed double-cancel was accepted\n");
        fails++;
    }
    if (ob_cancel_order_opt_indexed(&ob, &acc, &idx, 999999)) {
        printf("FAIL: indexed cancel of a nonexistent id was accepted\n");
        fails++;
    }

    if (fails == 0) {
        printf("PASS: indexed cancel O(1) fast path (place, immediate cancel, "
               "refund, double-cancel rejection, bogus-id rejection) all correct\n");
    }
    return fails;
}

static int test_indexed_cancel_stale_fallback(void) {
    L3OrderBook ob; EngineAccount acc;
    static OrderIndex idx;
    ob_init_opt(&ob, &acc, 100);
    ob_index_init(&idx);
    int fails = 0;

    // ob_init_opt already seeded bids[0] with 3 market-maker orders (slots
    // 0-2). Place the order under test after them, so it lands at slot 3
    // — the index caches that slot at placement time.
    long cash_before = acc.my_cash;
    int id = ob_place_limit_buy_opt_indexed(&ob, &acc, &idx, 0, 20);
    if (id < 0) {
        printf("FAIL: setup placement rejected unexpectedly\n");
        return 1;
    }
    int cached_slot_at_placement = ob.bids[0].order_count - 1;

    // Ghost the FIRST of the pre-seeded orders (slot 0, ahead of the order
    // under test) and compact — this shifts every surviving order down by
    // one slot, including `id`'s, exactly the staleness scenario the
    // fallback exists for.
    ob.bids[0].queue[0].qty = 0;
    ob.bids[0].queue[0].state = 2;
    ob_clean_ghosts_opt(&ob.bids[0]);

    int actual_slot_now = -1;
    for (int q = 0; q < ob.bids[0].order_count; q++) {
        if (ob.bids[0].queue[q].order_id == id) { actual_slot_now = q; break; }
    }
    if (actual_slot_now < 0) {
        printf("FAIL: test setup lost track of the target order after compaction\n");
        return 1;
    }
    if (actual_slot_now == cached_slot_at_placement) {
        printf("FAIL: test setup didn't actually shift the target order's slot "
               "(still at %d) — fallback wouldn't be exercised\n", actual_slot_now);
        return 1;
    }

    // idx still thinks `id` is at slot 2 — stale. This must still succeed,
    // via the bounded single-level fallback scan.
    if (!ob_cancel_order_opt_indexed(&ob, &acc, &idx, id)) {
        printf("FAIL: indexed cancel failed to find an order after its cached "
               "slot went stale (fallback scan did not work)\n");
        fails++;
    }
    if (acc.my_cash != cash_before) {
        printf("FAIL: indexed cancel via fallback did not refund cash exactly "
               "(got %ld, expected %ld)\n", acc.my_cash, cash_before);
        fails++;
    }

    if (fails == 0) {
        printf("PASS: indexed cancel correctly falls back to a single-level scan "
               "when a compaction has staled the cached slot\n");
    }
    return fails;
}

static int test_indexed_cancel_equivalence(void) {
    L3OrderBook ob_a, ob_b;   // ob_a: existing ob_cancel_order_opt; ob_b: indexed
    EngineAccount acc_a, acc_b;
    static OrderIndex idx;
    ob_init_opt(&ob_a, &acc_a, 100);
    ob_init_opt(&ob_b, &acc_b, 100);
    ob_index_init(&idx);

    rng_state = 0x1DEA5u; // fixed seed, independent of the other stress tests' streams
    int fails = 0;

    #define MAX_ISSUED 4096
    int issued_ids[MAX_ISSUED];
    int issued_count = 0;

    const int ITERATIONS = 50000;
    for (int i = 0; i < ITERATIONS && fails == 0; i++) {
        int action = rand_range(0, 99);
        int level = rand_range(0, MAX_PRICE_LEVELS - 1);
        int qty = rand_range(1, 25);

        if (action < 35) {
            int is_player = rand_range(0, 3) == 0;
            if (rand_range(0, 1)) {
                ob_market_buy_opt(&ob_a, &acc_a, qty, is_player);
                ob_market_buy_opt(&ob_b, &acc_b, qty, is_player);
            } else {
                ob_market_sell_opt(&ob_a, &acc_a, qty, is_player);
                ob_market_sell_opt(&ob_b, &acc_b, qty, is_player);
            }
        } else if (action < 70) {
            int id_a, id_b;
            if (rand_range(0, 1)) {
                id_a = ob_place_limit_buy_opt(&ob_a, &acc_a, level, qty);
                id_b = ob_place_limit_buy_opt_indexed(&ob_b, &acc_b, &idx, level, qty);
            } else {
                id_a = ob_place_limit_sell_opt(&ob_a, &acc_a, level, qty);
                id_b = ob_place_limit_sell_opt_indexed(&ob_b, &acc_b, &idx, level, qty);
            }
            if (id_a != id_b) {
                printf("FAIL at iter %d: placement id diverged (plain=%d indexed=%d)\n",
                       i, id_a, id_b);
                fails++;
            }
            if (id_a >= 0 && issued_count < MAX_ISSUED) {
                issued_ids[issued_count++] = id_a;
            }
        } else if (action < 90 && issued_count > 0) {
            int id = issued_ids[rand_range(0, issued_count - 1)];
            int r_a = ob_cancel_order_opt(&ob_a, &acc_a, id);
            int r_b = ob_cancel_order_opt_indexed(&ob_b, &acc_b, &idx, id);
            if (r_a != r_b) {
                printf("FAIL at iter %d: cancel(%d) result diverged (plain=%d indexed=%d)\n",
                       i, id, r_a, r_b);
                fails++;
            }
        } else {
            int bogus_id = rand_range(2000000, 3000000);
            int r_a = ob_cancel_order_opt(&ob_a, &acc_a, bogus_id);
            int r_b = ob_cancel_order_opt_indexed(&ob_b, &acc_b, &idx, bogus_id);
            if (r_a || r_b) {
                printf("FAIL at iter %d: cancel of a bogus id was accepted "
                       "(plain=%d indexed=%d)\n", i, r_a, r_b);
                fails++;
            }
        }

        for (int lvl = 0; lvl < MAX_PRICE_LEVELS; lvl++) {
            ob_clean_ghosts_opt(&ob_a.bids[lvl]);
            ob_clean_ghosts_opt(&ob_b.bids[lvl]);
            ob_clean_ghosts_opt(&ob_a.asks[lvl]);
            ob_clean_ghosts_opt(&ob_b.asks[lvl]);
        }

        if (!accounts_equal(&acc_a, &acc_b)) {
            printf("FAIL at iter %d: account state diverged between plain and "
                   "indexed cancel (plain cash=%ld inv=%d, indexed cash=%ld inv=%d)\n",
                   i, acc_a.my_cash, acc_a.my_inventory, acc_b.my_cash, acc_b.my_inventory);
            fails++;
        }
        if (!books_equal(&ob_a, &ob_b)) {
            printf("FAIL at iter %d: book state diverged between plain and indexed cancel\n", i);
            fails++;
        }
    }
    #undef MAX_ISSUED

    if (fails == 0) {
        printf("PASS: %d randomized operations, ob_cancel_order_opt_indexed produced "
               "identical account+book state to ob_cancel_order_opt at every step "
               "(%d order ids issued)\n", ITERATIONS, issued_count);
    }
    return fails;
}

static int test_indexed_cancel(void) {
    int failures = 0;
    failures += test_indexed_cancel_fast_path();
    failures += test_indexed_cancel_stale_fallback();
    failures += test_indexed_cancel_equivalence();
    return failures;
}

// ============================================================================
// Test 8: cancel-replace (ob_modify_qty_*, ob_modify_price_*).
//
// Same split the header documents: a qty-only modify keeps the order's id
// and queue slot (no priority lost); a price/level modify is cancel-old +
// place-new under the hood (new id, back of the new level's queue) and
// must leave the old order completely untouched if the new placement is
// rejected.
// ============================================================================
static int test_modify_order_lifecycle(void) {
    L3OrderBook ob_base, ob_opt;
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(&ob_base, &acc_base, 100);
    ob_init_opt(&ob_opt, &acc_opt, 100);

    int fails = 0;

    int id_base = ob_place_limit_buy_baseline(&ob_base, &acc_base, 0, 20);
    int id_opt  = ob_place_limit_buy_opt(&ob_opt, &acc_opt, 0, 20);
    if (id_base < 0 || id_base != id_opt) {
        printf("FAIL: setup placement diverged or failed (base=%d opt=%d)\n", id_base, id_opt);
        fails++;
    }

    // --- modify_qty: increase, same id, same slot, extra cash reserved ---
    long cash_before_inc_base = acc_base.my_cash, cash_before_inc_opt = acc_opt.my_cash;
    int r_base = ob_modify_qty_baseline(&ob_base, &acc_base, id_base, 35);
    int r_opt  = ob_modify_qty_opt(&ob_opt, &acc_opt, id_opt, 35);
    long expect_extra = (long)(35 - 20) * ob_base.bids[0].price;
    if (!r_base || !r_opt) {
        printf("FAIL: modify_qty increase was rejected (base=%d opt=%d)\n", r_base, r_opt);
        fails++;
    }
    if (acc_base.my_cash != cash_before_inc_base - expect_extra ||
        acc_opt.my_cash  != cash_before_inc_opt  - expect_extra) {
        printf("FAIL: modify_qty increase reserved the wrong amount of cash\n");
        fails++;
    }
    if (!order_live_at(&ob_base, 1, 0, id_base, 35) || !order_live_at(&ob_opt, 1, 0, id_opt, 35)) {
        printf("FAIL: modify_qty increase did not update the resting order in place\n");
        fails++;
    }

    // --- modify_qty: decrease, refund the difference ---
    long cash_before_dec_base = acc_base.my_cash, cash_before_dec_opt = acc_opt.my_cash;
    r_base = ob_modify_qty_baseline(&ob_base, &acc_base, id_base, 5);
    r_opt  = ob_modify_qty_opt(&ob_opt, &acc_opt, id_opt, 5);
    long expect_refund = (long)(35 - 5) * ob_base.bids[0].price;
    if (!r_base || !r_opt ||
        acc_base.my_cash != cash_before_dec_base + expect_refund ||
        acc_opt.my_cash  != cash_before_dec_opt  + expect_refund) {
        printf("FAIL: modify_qty decrease did not refund the exact difference\n");
        fails++;
    }
    if (!order_live_at(&ob_base, 1, 0, id_base, 5) || !order_live_at(&ob_opt, 1, 0, id_opt, 5)) {
        printf("FAIL: modify_qty decrease did not update the resting order in place\n");
        fails++;
    }

    // --- modify_qty: increase far beyond available cash must be rejected, untouched ---
    long cash_before_reject_base = acc_base.my_cash, cash_before_reject_opt = acc_opt.my_cash;
    r_base = ob_modify_qty_baseline(&ob_base, &acc_base, id_base, 10000000);
    r_opt  = ob_modify_qty_opt(&ob_opt, &acc_opt, id_opt, 10000000);
    if (r_base || r_opt ||
        acc_base.my_cash != cash_before_reject_base || acc_opt.my_cash != cash_before_reject_opt ||
        !order_live_at(&ob_base, 1, 0, id_base, 5) || !order_live_at(&ob_opt, 1, 0, id_opt, 5)) {
        printf("FAIL: modify_qty increase beyond available cash was accepted or mutated state\n");
        fails++;
    }

    // --- modify_qty: invalid new_qty and bogus id must both just fail ---
    if (ob_modify_qty_baseline(&ob_base, &acc_base, id_base, 0) ||
        ob_modify_qty_opt(&ob_opt, &acc_opt, id_opt, 0) ||
        ob_modify_qty_baseline(&ob_base, &acc_base, 999999, 10) ||
        ob_modify_qty_opt(&ob_opt, &acc_opt, 999999, 10)) {
        printf("FAIL: modify_qty accepted a non-positive qty or a bogus order id\n");
        fails++;
    }

    // --- modify_price: move to a different level, new id, old order gone ---
    long cash_before_move_base = acc_base.my_cash, cash_before_move_opt = acc_opt.my_cash;
    int new_id_base = ob_modify_price_baseline(&ob_base, &acc_base, id_base, 2, 12);
    int new_id_opt  = ob_modify_price_opt(&ob_opt, &acc_opt, id_opt, 2, 12);
    if (new_id_base < 0 || new_id_opt < 0 || new_id_base != new_id_opt || new_id_base == id_base) {
        printf("FAIL: modify_price did not return a fresh id (old=%d base=%d opt=%d)\n",
               id_base, new_id_base, new_id_opt);
        fails++;
    }
    if (order_live_at(&ob_base, 1, 0, id_base, 5) || order_live_at(&ob_opt, 1, 0, id_opt, 5)) {
        printf("FAIL: modify_price left the old order still resting at its old level\n");
        fails++;
    }
    if (!order_live_at(&ob_base, 1, 2, new_id_base, 12) || !order_live_at(&ob_opt, 1, 2, new_id_opt, 12)) {
        printf("FAIL: modify_price did not place the new order at the new level\n");
        fails++;
    }
    long expect_move_cash_delta = (long)5 * ob_base.bids[0].price - (long)12 * ob_base.bids[2].price;
    if (acc_base.my_cash != cash_before_move_base + expect_move_cash_delta ||
        acc_opt.my_cash  != cash_before_move_opt  + expect_move_cash_delta) {
        printf("FAIL: modify_price's net cash effect (refund old, reserve new) was wrong\n");
        fails++;
    }

    // --- modify_price rejected (invalid level): old order must be untouched ---
    long cash_before_bad_move_base = acc_base.my_cash, cash_before_bad_move_opt = acc_opt.my_cash;
    int bad_base = ob_modify_price_baseline(&ob_base, &acc_base, new_id_base, MAX_PRICE_LEVELS, 12);
    int bad_opt  = ob_modify_price_opt(&ob_opt, &acc_opt, new_id_opt, MAX_PRICE_LEVELS, 12);
    if (bad_base != -1 || bad_opt != -1 ||
        acc_base.my_cash != cash_before_bad_move_base || acc_opt.my_cash != cash_before_bad_move_opt ||
        !order_live_at(&ob_base, 1, 2, new_id_base, 12) || !order_live_at(&ob_opt, 1, 2, new_id_opt, 12)) {
        printf("FAIL: rejected modify_price mutated state or lost the original order\n");
        fails++;
    }

    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        ob_clean_ghosts_baseline(&ob_base.bids[i]);
        ob_clean_ghosts_opt(&ob_opt.bids[i]);
        ob_clean_ghosts_baseline(&ob_base.asks[i]);
        ob_clean_ghosts_opt(&ob_opt.asks[i]);
    }
    ob_update_total_qty_baseline(&ob_base);
    if (!accounts_equal(&acc_base, &acc_opt) || !books_equal(&ob_base, &ob_opt)) {
        printf("FAIL: baseline/opt diverged after the cancel-replace sequence\n");
        fails++;
    }

    if (fails == 0) {
        printf("PASS: cancel-replace lifecycle (qty increase/decrease/rejected-increase, "
               "price move with fresh id, rejected move leaving the original untouched) "
               "identical on baseline and opt\n");
    }
    return fails;
}

// Randomized run mixing modify_qty/modify_price into the same style of
// workload as test_random_stress, diffing full state after every op.
static int test_modify_random_stress(void) {
    L3OrderBook ob_base, ob_opt;
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(&ob_base, &acc_base, 100);
    ob_init_opt(&ob_opt, &acc_opt, 100);

    rng_state = 0x5CA1ABu; // fixed seed, independent stream from the other stress tests
    int fails = 0;

    #define MAX_ISSUED 4096
    int issued_ids[MAX_ISSUED];
    int issued_count = 0;

    const int ITERATIONS = 50000;
    for (int i = 0; i < ITERATIONS && fails == 0; i++) {
        int action = rand_range(0, 99);
        int level = rand_range(0, MAX_PRICE_LEVELS - 1);
        int qty = rand_range(1, 25);

        if (action < 25) {
            int is_player = rand_range(0, 3) == 0;
            if (rand_range(0, 1)) {
                ob_market_buy_baseline(&ob_base, &acc_base, qty, is_player);
                ob_market_buy_opt(&ob_opt, &acc_opt, qty, is_player);
            } else {
                ob_market_sell_baseline(&ob_base, &acc_base, qty, is_player);
                ob_market_sell_opt(&ob_opt, &acc_opt, qty, is_player);
            }
        } else if (action < 55) {
            int id_base, id_opt;
            if (rand_range(0, 1)) {
                id_base = ob_place_limit_buy_baseline(&ob_base, &acc_base, level, qty);
                id_opt  = ob_place_limit_buy_opt(&ob_opt, &acc_opt, level, qty);
            } else {
                id_base = ob_place_limit_sell_baseline(&ob_base, &acc_base, level, qty);
                id_opt  = ob_place_limit_sell_opt(&ob_opt, &acc_opt, level, qty);
            }
            if (id_base != id_opt) {
                printf("FAIL at iter %d: placement id diverged (base=%d opt=%d)\n", i, id_base, id_opt);
                fails++;
            }
            if (id_base >= 0 && issued_count < MAX_ISSUED) issued_ids[issued_count++] = id_base;
        } else if (action < 70 && issued_count > 0) {
            int id = issued_ids[rand_range(0, issued_count - 1)];
            int r_base = ob_cancel_order_baseline(&ob_base, &acc_base, id);
            int r_opt  = ob_cancel_order_opt(&ob_opt, &acc_opt, id);
            if (r_base != r_opt) {
                printf("FAIL at iter %d: cancel(%d) result diverged (base=%d opt=%d)\n", i, id, r_base, r_opt);
                fails++;
            }
        } else if (action < 85 && issued_count > 0) {
            // modify_qty on a previously-issued id — may already be gone,
            // that path (rejection) is exercised on purpose, not skipped.
            int id = issued_ids[rand_range(0, issued_count - 1)];
            int new_qty = rand_range(1, 40);
            int r_base = ob_modify_qty_baseline(&ob_base, &acc_base, id, new_qty);
            int r_opt  = ob_modify_qty_opt(&ob_opt, &acc_opt, id, new_qty);
            if (r_base != r_opt) {
                printf("FAIL at iter %d: modify_qty(%d,%d) result diverged (base=%d opt=%d)\n",
                       i, id, new_qty, r_base, r_opt);
                fails++;
            }
        } else if (issued_count > 0) {
            // modify_price on a previously-issued id — new id (if accepted)
            // gets tracked too, so it can itself be modified/cancelled later.
            int id = issued_ids[rand_range(0, issued_count - 1)];
            int new_level = rand_range(0, MAX_PRICE_LEVELS - 1);
            int new_qty = rand_range(1, 25);
            int new_id_base = ob_modify_price_baseline(&ob_base, &acc_base, id, new_level, new_qty);
            int new_id_opt  = ob_modify_price_opt(&ob_opt, &acc_opt, id, new_level, new_qty);
            if (new_id_base != new_id_opt) {
                printf("FAIL at iter %d: modify_price(%d) result diverged (base=%d opt=%d)\n",
                       i, id, new_id_base, new_id_opt);
                fails++;
            }
            if (new_id_base >= 0 && issued_count < MAX_ISSUED) issued_ids[issued_count++] = new_id_base;
        } else {
            int bogus_id = rand_range(2000000, 3000000);
            int r_base = ob_modify_qty_baseline(&ob_base, &acc_base, bogus_id, qty);
            int r_opt  = ob_modify_qty_opt(&ob_opt, &acc_opt, bogus_id, qty);
            if (r_base || r_opt) {
                printf("FAIL at iter %d: modify_qty of a bogus id was accepted (base=%d opt=%d)\n",
                       i, r_base, r_opt);
                fails++;
            }
        }

        for (int lvl = 0; lvl < MAX_PRICE_LEVELS; lvl++) {
            ob_clean_ghosts_baseline(&ob_base.bids[lvl]);
            ob_clean_ghosts_opt(&ob_opt.bids[lvl]);
            ob_clean_ghosts_baseline(&ob_base.asks[lvl]);
            ob_clean_ghosts_opt(&ob_opt.asks[lvl]);
        }
        ob_update_total_qty_baseline(&ob_base);

        if (!accounts_equal(&acc_base, &acc_opt)) {
            printf("FAIL at iter %d: account state diverged (base cash=%ld inv=%d, "
                   "opt cash=%ld inv=%d)\n", i, acc_base.my_cash, acc_base.my_inventory,
                   acc_opt.my_cash, acc_opt.my_inventory);
            fails++;
        }
        if (!books_equal(&ob_base, &ob_opt)) {
            printf("FAIL at iter %d: book state diverged\n", i);
            fails++;
        }
    }
    #undef MAX_ISSUED

    if (fails == 0) {
        printf("PASS: %d randomized operations (market/limit/cancel/modify_qty/modify_price, "
               "mixed valid and invalid), baseline and opt stayed identical after every single "
               "operation (%d order ids issued)\n", ITERATIONS, issued_count);
    }
    return fails;
}

// Indexed modify functions vs the plain (whole-book-scan) ones — same
// equivalence-under-randomization shape as test_indexed_cancel_equivalence.
static int test_indexed_modify_equivalence(void) {
    L3OrderBook ob_a, ob_b; // ob_a: plain ob_modify_*_opt; ob_b: indexed
    EngineAccount acc_a, acc_b;
    static OrderIndex idx;
    ob_init_opt(&ob_a, &acc_a, 100);
    ob_init_opt(&ob_b, &acc_b, 100);
    ob_index_init(&idx);

    rng_state = 0xBADA55u;
    int fails = 0;

    #define MAX_ISSUED 4096
    int issued_ids[MAX_ISSUED];
    int issued_count = 0;

    const int ITERATIONS = 50000;
    for (int i = 0; i < ITERATIONS && fails == 0; i++) {
        int action = rand_range(0, 99);
        int level = rand_range(0, MAX_PRICE_LEVELS - 1);
        int qty = rand_range(1, 25);

        if (action < 25) {
            int is_player = rand_range(0, 3) == 0;
            if (rand_range(0, 1)) {
                ob_market_buy_opt(&ob_a, &acc_a, qty, is_player);
                ob_market_buy_opt(&ob_b, &acc_b, qty, is_player);
            } else {
                ob_market_sell_opt(&ob_a, &acc_a, qty, is_player);
                ob_market_sell_opt(&ob_b, &acc_b, qty, is_player);
            }
        } else if (action < 55) {
            int id_a, id_b;
            if (rand_range(0, 1)) {
                id_a = ob_place_limit_buy_opt(&ob_a, &acc_a, level, qty);
                id_b = ob_place_limit_buy_opt_indexed(&ob_b, &acc_b, &idx, level, qty);
            } else {
                id_a = ob_place_limit_sell_opt(&ob_a, &acc_a, level, qty);
                id_b = ob_place_limit_sell_opt_indexed(&ob_b, &acc_b, &idx, level, qty);
            }
            if (id_a != id_b) {
                printf("FAIL at iter %d: placement id diverged (plain=%d indexed=%d)\n", i, id_a, id_b);
                fails++;
            }
            if (id_a >= 0 && issued_count < MAX_ISSUED) issued_ids[issued_count++] = id_a;
        } else if (action < 70 && issued_count > 0) {
            int id = issued_ids[rand_range(0, issued_count - 1)];
            int r_a = ob_cancel_order_opt(&ob_a, &acc_a, id);
            int r_b = ob_cancel_order_opt_indexed(&ob_b, &acc_b, &idx, id);
            if (r_a != r_b) {
                printf("FAIL at iter %d: cancel(%d) result diverged (plain=%d indexed=%d)\n", i, id, r_a, r_b);
                fails++;
            }
        } else if (action < 85 && issued_count > 0) {
            int id = issued_ids[rand_range(0, issued_count - 1)];
            int new_qty = rand_range(1, 40);
            int r_a = ob_modify_qty_opt(&ob_a, &acc_a, id, new_qty);
            int r_b = ob_modify_qty_opt_indexed(&ob_b, &acc_b, &idx, id, new_qty);
            if (r_a != r_b) {
                printf("FAIL at iter %d: modify_qty(%d,%d) result diverged (plain=%d indexed=%d)\n",
                       i, id, new_qty, r_a, r_b);
                fails++;
            }
        } else if (issued_count > 0) {
            int id = issued_ids[rand_range(0, issued_count - 1)];
            int new_level = rand_range(0, MAX_PRICE_LEVELS - 1);
            int new_qty = rand_range(1, 25);
            int new_id_a = ob_modify_price_opt(&ob_a, &acc_a, id, new_level, new_qty);
            int new_id_b = ob_modify_price_opt_indexed(&ob_b, &acc_b, &idx, id, new_level, new_qty);
            if (new_id_a != new_id_b) {
                printf("FAIL at iter %d: modify_price(%d) result diverged (plain=%d indexed=%d)\n",
                       i, id, new_id_a, new_id_b);
                fails++;
            }
            if (new_id_a >= 0 && issued_count < MAX_ISSUED) issued_ids[issued_count++] = new_id_a;
        } else {
            int bogus_id = rand_range(2000000, 3000000);
            int r_a = ob_modify_qty_opt(&ob_a, &acc_a, bogus_id, qty);
            int r_b = ob_modify_qty_opt_indexed(&ob_b, &acc_b, &idx, bogus_id, qty);
            if (r_a || r_b) {
                printf("FAIL at iter %d: modify_qty of a bogus id was accepted (plain=%d indexed=%d)\n",
                       i, r_a, r_b);
                fails++;
            }
        }

        for (int lvl = 0; lvl < MAX_PRICE_LEVELS; lvl++) {
            ob_clean_ghosts_opt(&ob_a.bids[lvl]);
            ob_clean_ghosts_opt(&ob_b.bids[lvl]);
            ob_clean_ghosts_opt(&ob_a.asks[lvl]);
            ob_clean_ghosts_opt(&ob_b.asks[lvl]);
        }

        if (!accounts_equal(&acc_a, &acc_b)) {
            printf("FAIL at iter %d: account state diverged between plain and indexed modify\n", i);
            fails++;
        }
        if (!books_equal(&ob_a, &ob_b)) {
            printf("FAIL at iter %d: book state diverged between plain and indexed modify\n", i);
            fails++;
        }
    }
    #undef MAX_ISSUED

    if (fails == 0) {
        printf("PASS: %d randomized operations, indexed modify_qty/modify_price produced "
               "identical account+book state to the plain (whole-book-scan) versions at "
               "every step (%d order ids issued)\n", ITERATIONS, issued_count);
    }
    return fails;
}

static int test_modify_order(void) {
    int failures = 0;
    failures += test_modify_order_lifecycle();
    failures += test_modify_random_stress();
    failures += test_indexed_modify_equivalence();
    return failures;
}

// ============================================================================
// Test 9: price drift (ob_drift_price_*).
//
// Every price level was frozen forever until now (see header comment on
// ob_drift_price_*). Deterministic case: shift up, shift down, and the
// floor rejection at MIN_PRICE — checked on both engines. Then a
// randomized run mixing drift into the full existing workload (market/
// limit/cancel/modify), diffing full state after every single operation —
// this is the one that actually proves drift composes safely with
// everything else already in this file, not just that it works in
// isolation.
// ============================================================================
static int test_price_drift_deterministic(void) {
    L3OrderBook ob_base, ob_opt;
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(&ob_base, &acc_base, 100);
    ob_init_opt(&ob_opt, &acc_opt, 100);

    int fails = 0;

    int r_base = ob_drift_price_baseline(&ob_base, 3);
    int r_opt  = ob_drift_price_opt(&ob_opt, 3);
    if (!r_base || !r_opt) {
        printf("FAIL: an ordinary upward drift was rejected (base=%d opt=%d)\n", r_base, r_opt);
        fails++;
    }
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        if (ob_base.bids[i].price != 100 - 1 - i + 3 || ob_opt.bids[i].price != 100 - 1 - i + 3 ||
            ob_base.asks[i].price != 100 + 1 + i + 3 || ob_opt.asks[i].price != 100 + 1 + i + 3) {
            printf("FAIL: drift did not shift every level by exactly delta (level %d)\n", i);
            fails++;
        }
    }

    r_base = ob_drift_price_baseline(&ob_base, -5);
    r_opt  = ob_drift_price_opt(&ob_opt, -5);
    if (!r_base || !r_opt) {
        printf("FAIL: an ordinary downward drift was rejected (base=%d opt=%d)\n", r_base, r_opt);
        fails++;
    }
    // Net drift so far: +3-5 = -2 -> lowest bid (index MAX_PRICE_LEVELS-1) is
    // 100 - 1 - (MAX_PRICE_LEVELS-1) - 2.
    int expect_lowest_bid = 100 - 1 - (MAX_PRICE_LEVELS - 1) - 2;
    if (ob_base.bids[MAX_PRICE_LEVELS - 1].price != expect_lowest_bid ||
        ob_opt.bids[MAX_PRICE_LEVELS - 1].price != expect_lowest_bid) {
        printf("FAIL: cumulative drift produced the wrong lowest bid price (expected %d)\n", expect_lowest_bid);
        fails++;
    }

    // Drive a big downward drift that must be rejected outright (would
    // push a bid below MIN_PRICE) — nothing should change.
    L3OrderBook ob_base_before = ob_base, ob_opt_before = ob_opt;
    r_base = ob_drift_price_baseline(&ob_base, -1000000);
    r_opt  = ob_drift_price_opt(&ob_opt, -1000000);
    if (r_base || r_opt || !books_equal(&ob_base, &ob_base_before) || !books_equal(&ob_opt, &ob_opt_before)) {
        printf("FAIL: a drift that would push a bid below MIN_PRICE was accepted or mutated state\n");
        fails++;
    }

    if (!accounts_equal(&acc_base, &acc_opt) || !books_equal(&ob_base, &ob_opt)) {
        printf("FAIL: baseline/opt diverged after the price-drift sequence\n");
        fails++;
    }

    if (fails == 0) {
        printf("PASS: price drift (upward, downward, cumulative, floor rejection leaving state "
               "untouched) identical on baseline and opt\n");
    }
    return fails;
}

static int test_price_drift_random_stress(void) {
    L3OrderBook ob_base, ob_opt;
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(&ob_base, &acc_base, 100);
    ob_init_opt(&ob_opt, &acc_opt, 100);

    rng_state = 0xD121F7u; // fixed seed, independent stream
    int fails = 0;

    #define MAX_ISSUED 4096
    int issued_ids[MAX_ISSUED];
    int issued_count = 0;

    const int ITERATIONS = 50000;
    for (int i = 0; i < ITERATIONS && fails == 0; i++) {
        int action = rand_range(0, 99);
        int level = rand_range(0, MAX_PRICE_LEVELS - 1);
        int qty = rand_range(1, 25);

        if (action < 20) {
            int is_player = rand_range(0, 3) == 0;
            if (rand_range(0, 1)) {
                ob_market_buy_baseline(&ob_base, &acc_base, qty, is_player);
                ob_market_buy_opt(&ob_opt, &acc_opt, qty, is_player);
            } else {
                ob_market_sell_baseline(&ob_base, &acc_base, qty, is_player);
                ob_market_sell_opt(&ob_opt, &acc_opt, qty, is_player);
            }
        } else if (action < 45) {
            int id_base, id_opt;
            if (rand_range(0, 1)) {
                id_base = ob_place_limit_buy_baseline(&ob_base, &acc_base, level, qty);
                id_opt  = ob_place_limit_buy_opt(&ob_opt, &acc_opt, level, qty);
            } else {
                id_base = ob_place_limit_sell_baseline(&ob_base, &acc_base, level, qty);
                id_opt  = ob_place_limit_sell_opt(&ob_opt, &acc_opt, level, qty);
            }
            if (id_base != id_opt) {
                printf("FAIL at iter %d: placement id diverged (base=%d opt=%d)\n", i, id_base, id_opt);
                fails++;
            }
            if (id_base >= 0 && issued_count < MAX_ISSUED) issued_ids[issued_count++] = id_base;
        } else if (action < 60 && issued_count > 0) {
            int id = issued_ids[rand_range(0, issued_count - 1)];
            int r_base = ob_cancel_order_baseline(&ob_base, &acc_base, id);
            int r_opt  = ob_cancel_order_opt(&ob_opt, &acc_opt, id);
            if (r_base != r_opt) {
                printf("FAIL at iter %d: cancel(%d) result diverged (base=%d opt=%d)\n", i, id, r_base, r_opt);
                fails++;
            }
        } else if (action < 75 && issued_count > 0) {
            int id = issued_ids[rand_range(0, issued_count - 1)];
            int new_qty = rand_range(1, 40);
            int r_base = ob_modify_qty_baseline(&ob_base, &acc_base, id, new_qty);
            int r_opt  = ob_modify_qty_opt(&ob_opt, &acc_opt, id, new_qty);
            if (r_base != r_opt) {
                printf("FAIL at iter %d: modify_qty(%d,%d) result diverged (base=%d opt=%d)\n",
                       i, id, new_qty, r_base, r_opt);
                fails++;
            }
        } else if (action < 90 && issued_count > 0) {
            int id = issued_ids[rand_range(0, issued_count - 1)];
            int new_level = rand_range(0, MAX_PRICE_LEVELS - 1);
            int new_qty = rand_range(1, 25);
            int new_id_base = ob_modify_price_baseline(&ob_base, &acc_base, id, new_level, new_qty);
            int new_id_opt  = ob_modify_price_opt(&ob_opt, &acc_opt, id, new_level, new_qty);
            if (new_id_base != new_id_opt) {
                printf("FAIL at iter %d: modify_price(%d) result diverged (base=%d opt=%d)\n",
                       i, id, new_id_base, new_id_opt);
                fails++;
            }
            if (new_id_base >= 0 && issued_count < MAX_ISSUED) issued_ids[issued_count++] = new_id_base;
        } else {
            // Drift, including deltas large enough to sometimes hit the
            // MIN_PRICE floor rejection path on purpose.
            int delta = rand_range(-20, 20);
            int r_base = ob_drift_price_baseline(&ob_base, delta);
            int r_opt  = ob_drift_price_opt(&ob_opt, delta);
            if (r_base != r_opt) {
                printf("FAIL at iter %d: drift(%d) result diverged (base=%d opt=%d)\n", i, delta, r_base, r_opt);
                fails++;
            }
        }

        for (int lvl = 0; lvl < MAX_PRICE_LEVELS; lvl++) {
            ob_clean_ghosts_baseline(&ob_base.bids[lvl]);
            ob_clean_ghosts_opt(&ob_opt.bids[lvl]);
            ob_clean_ghosts_baseline(&ob_base.asks[lvl]);
            ob_clean_ghosts_opt(&ob_opt.asks[lvl]);
        }
        ob_update_total_qty_baseline(&ob_base);

        if (!accounts_equal(&acc_base, &acc_opt)) {
            printf("FAIL at iter %d: account state diverged (base cash=%ld inv=%d, "
                   "opt cash=%ld inv=%d)\n", i, acc_base.my_cash, acc_base.my_inventory,
                   acc_opt.my_cash, acc_opt.my_inventory);
            fails++;
        }
        if (!books_equal(&ob_base, &ob_opt)) {
            printf("FAIL at iter %d: book state diverged\n", i);
            fails++;
        }
    }
    #undef MAX_ISSUED

    if (fails == 0) {
        printf("PASS: %d randomized operations (market/limit/cancel/modify_qty/modify_price/drift, "
               "mixed valid and invalid), baseline and opt stayed identical after every single "
               "operation (final lowest bid=%d, %d order ids issued)\n",
               ITERATIONS, ob_base.bids[MAX_PRICE_LEVELS - 1].price, issued_count);
    }
    return fails;
}

static int test_price_drift(void) {
    int failures = 0;
    failures += test_price_drift_deterministic();
    failures += test_price_drift_random_stress();
    return failures;
}

int main(void) {
    int failures = 0;
    failures += test_tick_replay();
    failures += test_capacity_bounds();
    failures += test_limit_order_lifecycle();
    failures += test_random_stress();
    failures += test_risk_limit_clipping();
    failures += test_risk_limit_end_to_end();
    failures += test_indexed_cancel();
    failures += test_modify_order();
    failures += test_price_drift();

    if (failures == 0) {
        printf("\nALL TESTS PASSED\n");
        return 0;
    }
    printf("\n%d TEST GROUP(S) FAILED\n", failures);
    return 1;
}
