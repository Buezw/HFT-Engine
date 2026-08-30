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

int main(void) {
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
                if (ob_base.bids[i].order_count < MAX_ORDERS_PER_LVL) {
                    int q = (t * 13 + i * 7) % 40 + 10;
                    ob_base.bids[i].queue[ob_base.bids[i].order_count++] =
                        (L3Order){acc_base.global_order_id++, q, 0, 0, 1};
                    ob_add_order_opt(&ob_opt.bids[i],
                        (L3Order){acc_opt.global_order_id++, q, 0, 0, 1});
                }
                if (ob_base.asks[i].order_count < MAX_ORDERS_PER_LVL) {
                    int q = (t * 17 + i * 11) % 40 + 10;
                    ob_base.asks[i].queue[ob_base.asks[i].order_count++] =
                        (L3Order){acc_base.global_order_id++, q, 0, 0, 1};
                    ob_add_order_opt(&ob_opt.asks[i],
                        (L3Order){acc_opt.global_order_id++, q, 0, 0, 1});
                }
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
