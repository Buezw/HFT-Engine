// ============================================================================
// book_trace.c
//
// Runs a fixed, reproducible scenario against the opt engine and prints
// one JSON object per tick to stdout: full book state (every price
// level, every order resting there, is_mine), account state, and a short
// description of what happened that tick. `make visualize` pipes this
// into tools/render_trace.py, which embeds it into a self-contained,
// scrubbable HTML replay (tools/visualizer_template.html).
//
// Deliberately reuses only the engine's public API (ob_market_*_opt,
// ob_place_limit_*_opt, ob_cancel_order_opt, the read-only accessors) --
// this is a viewer, not a second implementation of anything.
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

static void print_side(const L3OrderBook *ob, int is_bid) {
    int n = ob_num_levels(ob, is_bid);
    for (int i = 0; i < n; i++) {
        if (i) printf(",");
        int price = ob_level_price(ob, is_bid, i);
        long qty = ob_level_qty(ob, is_bid, i);
        int count = ob_level_order_count(ob, is_bid, i);
        printf("{\"price\":%d,\"total_qty\":%ld,\"order_count\":%d,\"orders\":[",
               price, qty, count);
        for (int s = 0; s < count; s++) {
            int id, oqty, mine;
            ob_level_order_at(ob, is_bid, i, s, &id, &oqty, &mine);
            if (s) printf(",");
            printf("{\"id\":%d,\"qty\":%d,\"is_mine\":%d}", id, oqty, mine);
        }
        printf("]}");
    }
}

static void print_tick(int tick, const L3OrderBook *ob, const EngineAccount *acc,
                        const char *event) {
    printf("{\"tick\":%d,\"event\":\"%s\",\"bids\":[", tick, event);
    print_side(ob, 1);
    printf("],\"asks\":[");
    print_side(ob, 0);
    // current_total_assets is never updated past ob_init_* in this engine
    // (only board/main.c's own loop marks it to market) -- compute PnL
    // here from cash + inventory marked at the best bid instead of
    // trusting it. An empty bid side (possible now that levels can
    // actually run dry) has no price to mark against; skip marking then.
    int best_bid = ob_level_price(ob, 1, 0);
    long pnl = acc->my_cash + (best_bid > 0 ? (long)acc->my_inventory * best_bid : 0) - INITIAL_CAPITAL;
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

    L3OrderBook *ob = ob_create();
    EngineAccount acc;
    ob_init_opt(ob, &acc, 100);

    for (int t = 0; t < ticks; t++) {
        char event[128];
        snprintf(event, sizeof(event), "tick %d: quiet", t);

        // Market-maker liquidity injection at a small spread of real
        // prices around the mid, same cadence/shape as before.
        if (t % 3 == 0) {
            int i = t % 3;
            int qb = (t * 13 + i * 7) % 40 + 10;
            int qa = (t * 17 + i * 11) % 40 + 10;
            ob_add_order_opt(ob, /*is_bid=*/1, 99 - i, acc.global_order_id++, qb, 0);
            ob_add_order_opt(ob, /*is_bid=*/0, 101 + i, acc.global_order_id++, qa, 0);
            snprintf(event, sizeof(event),
                     "tick %d: market noise added liquidity near %d/%d", t, 99 - i, 101 + i);
        }

        // Noise market orders (is_player=0) chewing into the book.
        if (t % 5 == 0) {
            int vol = rand_range(10, 30);
            if (rand_range(0, 1)) {
                ob_market_sell_opt(ob, &acc, vol, 0);
                snprintf(event, sizeof(event), "tick %d: market noise sold %d", t, vol);
            } else {
                ob_market_buy_opt(ob, &acc, vol, 0);
                snprintf(event, sizeof(event), "tick %d: market noise bought %d", t, vol);
            }
        }

        // Player places a resting limit order at a real price near the mid.
        if (t % 9 == 0) {
            int price = 95 + rand_range(0, 10);
            int qty = rand_range(5, 25);
            int id = rand_range(0, 1)
                ? ob_place_limit_buy_opt(ob, &acc, price, qty)
                : ob_place_limit_sell_opt(ob, &acc, price, qty);
            if (id >= 0) {
                track_open(id);
                snprintf(event, sizeof(event),
                         "tick %d: player placed limit order id %d (price %d, qty %d)",
                         t, id, price, qty);
            }
        }

        // Player cancels one of its own resting orders.
        if (t % 11 == 0) {
            int id = pop_random_open();
            if (id >= 0) {
                int ok = ob_cancel_order_opt(ob, &acc, id);
                snprintf(event, sizeof(event), "tick %d: player cancelled order id %d (%s)",
                         t, id, ok ? "refunded" : "already gone");
            }
        }

        // Player risk-checked market order (pre-trade position limit).
        if (t % 13 == 0) {
            int qty = rand_range(5, 20);
            int submitted = rand_range(0, 1)
                ? ob_market_buy_risk_checked_opt(ob, &acc, qty)
                : ob_market_sell_risk_checked_opt(ob, &acc, qty);
            snprintf(event, sizeof(event),
                     "tick %d: player risk-checked order requested %d, submitted %d "
                     "(inventory %d)", t, qty, submitted, acc.my_inventory);
        }

        print_tick(t, ob, &acc, event);
    }

    ob_destroy(ob);
    return 0;
}
