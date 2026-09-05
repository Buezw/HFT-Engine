// ============================================================================
// book_trace.c
//
// The desktop-testable engine (src/orderbook_engine.c) has no visualization
// at all — the only rendering that ever existed for this project is
// board/main.c's VGA framebuffer code, which only runs on the physical
// DE1-SoC and can't be seen without the board. Debugging or demoing anything
// built on top of the extracted engine (market-making logic included) means
// staring at printf("PASS")/nanosecond numbers and reasoning about book
// state in your head.
//
// This runs a fixed, reproducible scenario against the *_opt engine +
// indexed order lifecycle, and prints one JSON object per tick to stdout:
// full book state (every price level, every order, is_mine/state), account
// state, and a short description of what happened that tick. `make
// visualize` pipes this into tools/render_trace.py, which embeds it into a
// self-contained, scrubbable HTML replay (tools/visualizer_template.html).
//
// Deliberately reuses the engine's public API only (ob_market_*_opt,
// ob_place_limit_*_opt_indexed, ob_cancel_order_opt_indexed,
// ob_drift_price_opt, ob_clean_ghosts_opt) — this is a viewer, not a
// second implementation of anything, and it's the same harness that will
// show whatever market-making/quoting logic gets layered on top later,
// unchanged.
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "orderbook_engine.h"

// Same fixed-seed xorshift32 used in tests/test_correctness.c, so this
// scenario reproduces identically on every run/platform.
static uint32_t rng_state = 0xC0FFEEu;
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

static void print_level(const L3PriceLevel *lvl) {
    printf("{\"price\":%d,\"total_qty\":%d,\"order_count\":%d,\"orders\":[",
           lvl->price, lvl->total_qty, lvl->order_count);
    for (int i = 0; i < lvl->order_count; i++) {
        const L3Order *o = &lvl->queue[i];
        if (i) printf(",");
        printf("{\"id\":%d,\"qty\":%d,\"is_mine\":%d,\"state\":%d}",
               o->order_id, o->qty, o->is_mine, o->state);
    }
    printf("]}");
}

static void print_tick(int tick, const L3OrderBook *ob, const EngineAccount *acc,
                        const char *event) {
    printf("{\"tick\":%d,\"event\":\"%s\",\"bids\":[", tick, event);
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        if (i) printf(",");
        print_level(&ob->bids[i]);
    }
    printf("],\"asks\":[");
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        if (i) printf(",");
        print_level(&ob->asks[i]);
    }
    // current_total_assets is never updated past ob_init_* in this engine
    // (only board/main.c's own loop marks it to market) — compute PnL here
    // from cash + inventory marked at the best bid instead of trusting it.
    long pnl = acc->my_cash + (long)acc->my_inventory * ob->bids[0].price - INITIAL_CAPITAL;
    printf("],\"account\":{\"cash\":%ld,\"inventory\":%d,\"pnl\":%ld,\"fill_volume\":%ld}}\n",
           acc->my_cash, acc->my_inventory, pnl, acc->total_fill_volume);
}

#define MAX_OPEN_ORDERS 64
static int open_ids[MAX_OPEN_ORDERS];
static int open_count = 0;

static void track_open(int id) {
    if (id < 0 || open_count >= MAX_OPEN_ORDERS) return;
    open_ids[open_count++] = id;
}
static int pop_random_open(void) {
    if (open_count == 0) return -1;
    int i = rand_range(0, open_count - 1);
    int id = open_ids[i];
    open_ids[i] = open_ids[--open_count];
    return id;
}

int main(int argc, char **argv) {
    int ticks = (argc > 1) ? atoi(argv[1]) : 300;

    L3OrderBook ob;
    EngineAccount acc;
    OrderIndex idx;
    ob_init_opt(&ob, &acc, 100);
    ob_index_init(&idx);

    for (int t = 0; t < ticks; t++) {
        char event[128];
        snprintf(event, sizeof(event), "tick %d: quiet", t);

        // Market-maker liquidity injection, same cadence/shape as the
        // replay in tests/test_correctness.c.
        if (t % 3 == 0) {
            int i = t % MAX_PRICE_LEVELS;
            int qb = (t * 13 + i * 7) % 40 + 10;
            int qa = (t * 17 + i * 11) % 40 + 10;
            ob_add_order_opt(&ob.bids[i], (L3Order){acc.global_order_id++, qb, 0, 0, 0});
            ob_add_order_opt(&ob.asks[i], (L3Order){acc.global_order_id++, qa, 0, 0, 0});
            snprintf(event, sizeof(event),
                     "tick %d: market noise added liquidity at level %d", t, i);
        }

        // Noise market orders (is_player=0) chewing into the book.
        if (t % 5 == 0) {
            int vol = rand_range(10, 30);
            if (rand_range(0, 1)) {
                ob_market_sell_opt(&ob, &acc, vol, 0);
                snprintf(event, sizeof(event), "tick %d: market noise sold %d", t, vol);
            } else {
                ob_market_buy_opt(&ob, &acc, vol, 0);
                snprintf(event, sizeof(event), "tick %d: market noise bought %d", t, vol);
            }
        }

        // Player places a resting limit order (indexed lifecycle).
        if (t % 9 == 0) {
            int lvl = rand_range(0, MAX_PRICE_LEVELS - 1);
            int qty = rand_range(5, 25);
            int id = rand_range(0, 1)
                ? ob_place_limit_buy_opt_indexed(&ob, &acc, &idx, lvl, qty)
                : ob_place_limit_sell_opt_indexed(&ob, &acc, &idx, lvl, qty);
            if (id >= 0) {
                track_open(id);
                snprintf(event, sizeof(event),
                         "tick %d: player placed limit order id %d (level %d, qty %d)",
                         t, id, lvl, qty);
            }
        }

        // Player cancels one of its own resting orders.
        if (t % 11 == 0) {
            int id = pop_random_open();
            if (id >= 0) {
                int ok = ob_cancel_order_opt_indexed(&ob, &acc, &idx, id);
                snprintf(event, sizeof(event), "tick %d: player cancelled order id %d (%s)",
                         t, id, ok ? "refunded" : "already gone");
            }
        }

        // Player risk-checked market order (pre-trade position limit).
        if (t % 13 == 0) {
            int qty = rand_range(5, 20);
            int submitted = rand_range(0, 1)
                ? ob_market_buy_risk_checked_opt(&ob, &acc, qty)
                : ob_market_sell_risk_checked_opt(&ob, &acc, qty);
            snprintf(event, sizeof(event),
                     "tick %d: player risk-checked order requested %d, submitted %d "
                     "(inventory %d)", t, qty, submitted, acc.my_inventory);
        }

        // Price drift — the market moving on its own, not in response to
        // anything the player or the noise flow did. Small step, every
        // few ticks, so the ladder visibly walks instead of sitting
        // frozen at its ob_init_opt value for the whole replay.
        if (t % 4 == 0) {
            int delta = rand_range(0, 1) ? 1 : -1;
            if (ob_drift_price_opt(&ob, delta)) {
                snprintf(event, sizeof(event), "tick %d: price drifted %s%d (mid now %d)",
                         t, delta > 0 ? "+" : "", delta, (ob.bids[0].price + ob.asks[0].price) / 2);
            }
        }

        for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
            ob_clean_ghosts_opt(&ob.bids[i]);
            ob_clean_ghosts_opt(&ob.asks[i]);
        }

        print_tick(t, &ob, &acc, event);
    }

    return 0;
}
