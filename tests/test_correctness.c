// ============================================================================
// test_correctness.c
//
// Same idea as before the C++ rewrite: replay the same sequence of
// market-maker additions, market buy/sell events, and lifecycle
// operations against both baseline and opt, diff the result after every
// single step. If they ever diverge, abort immediately with the step
// number, so a silent bug can't hide behind "it's faster".
//
// books_equal used to be a raw memcmp(L3OrderBook) -- that only worked
// because both implementations shared one fixed-array struct layout.
// Now that L3OrderBook is opaque and backed by std::map/std::deque
// internally, equality means the same thing it always should have:
// same price levels, in the same order, same qty, same orders resting
// at each one -- checked through the public accessors, not raw memory.
// ============================================================================
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "orderbook_engine.h"

// ob_books_equal (native, single linear pass inside the engine) -- do
// NOT reimplement this by looping ob_level_price/ob_level_qty/etc. here;
// each of those walks from the best price on every call, so a loop over
// every level turns one comparison into O(levels^2). Learned that the
// hard way: the 200k-tick replay below went from a few seconds to
// "didn't finish in two minutes" the first time this was written that way.
static int books_equal(const L3OrderBook *a, const L3OrderBook *b) {
    return ob_books_equal(a, b);
}

static int accounts_equal(const EngineAccount *a, const EngineAccount *b) {
    return a->my_cash == b->my_cash &&
           a->my_inventory == b->my_inventory &&
           a->total_fill_volume == b->total_fill_volume;
}

// Used by the modify tests to verify a placed/modified order actually
// landed where it should have, not just that some return value looked
// right. Scans every level on the given side for order_id.
static int order_live_at(const L3OrderBook *ob, int is_bid, int price, int id, int expected_qty) {
    int n = ob_num_levels(ob, is_bid);
    for (int i = 0; i < n; i++) {
        if (ob_level_price(ob, is_bid, i) != price) continue;
        int c = ob_level_order_count(ob, is_bid, i);
        for (int s = 0; s < c; s++) {
            int oid, qty, mine;
            ob_level_order_at(ob, is_bid, i, s, &oid, &qty, &mine);
            if (oid == id) return qty == expected_qty;
        }
        return 0;
    }
    return 0;
}

// Fixed-seed xorshift32 -- deterministic across platforms/runs, same PRNG
// tools/book_trace.c uses for its own reproducible scenario.
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

// ============================================================================
// Test 1: N-tick behavioral replay, baseline vs opt.
// ============================================================================
#define SEED_SPREAD 3 // how many price levels each side gets seeded with -- a starting shape, not a limit

static int test_tick_replay(void) {
    L3OrderBook *ob_base = ob_create(), *ob_opt = ob_create();
    EngineAccount acc_base, acc_opt;

    ob_init_baseline(ob_base, &acc_base, 100);
    ob_init_opt(ob_opt, &acc_opt, 100);

    if (!books_equal(ob_base, ob_opt)) {
        printf("FAIL: initial books differ\n");
        ob_destroy(ob_base); ob_destroy(ob_opt);
        return 1;
    }

    const int TICKS = 200000;
    int mismatches = 0;

    for (int t = 0; t < TICKS; t++) {
        // Market maker adds liquidity every other tick at a handful of
        // real prices around the mid; then a market order eats into one
        // side. Same synthetic pattern as before the rewrite, just naming
        // real prices instead of fixed level indices.
        if (t % 2 == 0) {
            for (int i = 0; i < SEED_SPREAD; i++) {
                int qb = (t * 13 + i * 7) % 40 + 10;
                ob_add_order_baseline(ob_base, /*is_bid=*/1, 99 - i, acc_base.global_order_id++, qb, 0);
                ob_add_order_opt     (ob_opt,  /*is_bid=*/1, 99 - i, acc_opt.global_order_id++,  qb, 0);

                int qa = (t * 17 + i * 11) % 40 + 10;
                ob_add_order_baseline(ob_base, /*is_bid=*/0, 101 + i, acc_base.global_order_id++, qa, 0);
                ob_add_order_opt     (ob_opt,  /*is_bid=*/0, 101 + i, acc_opt.global_order_id++,  qa, 0);
            }
        }

        // 40 was fine on the old engine because MAX_ORDERS_PER_LVL=10
        // silently capped injection -- excess inserts just got rejected,
        // so the book stayed small no matter the imbalance. No cap
        // anymore, so 40 (way under the ~30/level/side injected every
        // other tick across 3 levels) meant the book just grew forever
        // over 200k ticks and every op got slower and slower. 100 keeps
        // it roughly steady-state.
        int vol = 100;
        if ((t * 11) % 100 < 45) {
            ob_market_sell_baseline(ob_base, &acc_base, vol, 0);
            ob_market_sell_opt(ob_opt, &acc_opt, vol, 0);
        } else {
            ob_market_buy_baseline(ob_base, &acc_base, vol, 0);
            ob_market_buy_opt(ob_opt, &acc_opt, vol, 0);
        }

        // Also exercise the is_player=1 path (this is what actually moves
        // my_cash/my_inventory) every 7 ticks, mixing buys and sells, so
        // the account-state comparison below isn't vacuously true.
        if (t % 7 == 0) {
            if (t % 14 == 0) {
                ob_market_buy_baseline(ob_base, &acc_base, 15, 1);
                ob_market_buy_opt(ob_opt, &acc_opt, 15, 1);
            } else {
                ob_market_sell_baseline(ob_base, &acc_base, 15, 1);
                ob_market_sell_opt(ob_opt, &acc_opt, 15, 1);
            }
        }

        if (!accounts_equal(&acc_base, &acc_opt)) {
            printf("FAIL at tick %d: account state diverged "
                   "(base cash=%ld inv=%d, opt cash=%ld inv=%d)\n",
                   t, acc_base.my_cash, acc_base.my_inventory,
                   acc_opt.my_cash, acc_opt.my_inventory);
            mismatches++;
            if (mismatches > 5) break;
        }
        if (!books_equal(ob_base, ob_opt)) {
            printf("FAIL at tick %d: book state diverged\n", t);
            mismatches++;
            if (mismatches > 5) break;
        }
    }

    int result = mismatches == 0 ? 0 : 1;
    if (result == 0) {
        printf("PASS: %d ticks, baseline and optimized engines produced "
               "identical account + book state (final cash=%ld, inventory=%d, "
               "fill_volume=%ld)\n",
               TICKS, acc_base.my_cash, acc_base.my_inventory, acc_base.total_fill_volume);
    } else {
        printf("FAIL: %d mismatches found\n", mismatches);
    }
    ob_destroy(ob_base); ob_destroy(ob_opt);
    return result;
}

// ============================================================================
// Test 2: no fixed depth -- this is the whole reason for the C++ rewrite.
// The old engine had MAX_PRICE_LEVELS price slots and MAX_ORDERS_PER_LVL
// orders per slot, both fixed at compile time; anything past either cap
// got silently dropped (or, before a fix, corrupted memory). Confirms
// neither limit exists anymore: many orders at one level, and orders at
// prices far outside any "reasonable" window, both just work.
// ============================================================================
static int test_no_capacity_bounds_one(const char *label,
                                        int (*add)(L3OrderBook *, int, int, int, int, int)) {
    L3OrderBook *ob = ob_create();
    int fails = 0;

    // What used to be a hard cap (MAX_ORDERS_PER_LVL was 10 on the
    // board's real build) -- 500 orders at the exact same price, all
    // must be accepted and all must still be there.
    const int N = 500;
    for (int i = 0; i < N; i++) {
        if (!add(ob, /*is_bid=*/1, /*price=*/100, 1000 + i, 10 + i, 0)) {
            printf("FAIL [%s]: insert %d/%d rejected -- there's no capacity to run out of now\n",
                   label, i, N);
            fails++;
            break;
        }
    }
    if (ob_level_order_count(ob, 1, 0) != N) {
        printf("FAIL [%s]: expected %d orders resting at the one level, found %d\n",
               label, N, ob_level_order_count(ob, 1, 0));
        fails++;
    }

    // A price far outside what the old fixed 3-slot window could ever
    // represent -- this used to be silently dropped as "out of range".
    if (!add(ob, /*is_bid=*/1, /*price=*/1, 99999, 7, 0)) {
        printf("FAIL [%s]: a deep, far-from-best price was rejected\n", label);
        fails++;
    }
    if (ob_num_levels(ob, 1) < 2) {
        printf("FAIL [%s]: the deep price didn't create its own level\n", label);
        fails++;
    }

    if (fails == 0) {
        printf("PASS [%s]: %d orders at one level and an arbitrarily deep price "
               "both just worked, no fixed-depth rejection\n", label, N);
    }
    ob_destroy(ob);
    return fails;
}

static int test_no_capacity_bounds(void) {
    int failures = 0;
    failures += test_no_capacity_bounds_one("ob_add_order_baseline", ob_add_order_baseline);
    failures += test_no_capacity_bounds_one("ob_add_order_opt",      ob_add_order_opt);
    return failures;
}

// ============================================================================
// Test 3: resting limit-order lifecycle (place / cancel), deterministic.
// ============================================================================
static int test_limit_order_lifecycle(void) {
    L3OrderBook *ob_base = ob_create(), *ob_opt = ob_create();
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(ob_base, &acc_base, 100);
    ob_init_opt(ob_opt, &acc_opt, 100);

    int fails = 0;
    long cash_before_base = acc_base.my_cash, cash_before_opt = acc_opt.my_cash;

    int buy_id_base = ob_place_limit_buy_baseline(ob_base, &acc_base, 99, 20);
    int buy_id_opt  = ob_place_limit_buy_opt(ob_opt, &acc_opt, 99, 20);
    if (buy_id_base < 0 || buy_id_opt < 0 || buy_id_base != buy_id_opt) {
        printf("FAIL: limit buy placement ids diverged or failed (base=%d opt=%d)\n",
               buy_id_base, buy_id_opt);
        fails++;
    }
    long expect_reserved = (long)20 * 99;
    if (acc_base.my_cash != cash_before_base - expect_reserved ||
        acc_opt.my_cash  != cash_before_opt  - expect_reserved) {
        printf("FAIL: limit buy did not reserve cash correctly (base=%ld opt=%ld, expected -%ld)\n",
               acc_base.my_cash, acc_opt.my_cash, expect_reserved);
        fails++;
    }

    // Invalid placements must be rejected, and reject with -1, on both.
    if (ob_place_limit_buy_baseline(ob_base, &acc_base, -5, 10) != -1 ||
        ob_place_limit_buy_opt(ob_opt, &acc_opt, -5, 10) != -1) {
        printf("FAIL: a non-positive price was accepted\n");
        fails++;
    }
    if (ob_place_limit_buy_baseline(ob_base, &acc_base, 99, 0) != -1 ||
        ob_place_limit_buy_opt(ob_opt, &acc_opt, 99, 0) != -1) {
        printf("FAIL: zero-qty placement was accepted\n");
        fails++;
    }
    // No inventory yet -- selling should be rejected, not allowed to go negative.
    if (ob_place_limit_sell_baseline(ob_base, &acc_base, 101, 5) != -1 ||
        ob_place_limit_sell_opt(ob_opt, &acc_opt, 101, 5) != -1) {
        printf("FAIL: sell placement with insufficient inventory was accepted\n");
        fails++;
    }

    // Cancel refunds exactly what was reserved.
    long cash_before_cancel_base = acc_base.my_cash, cash_before_cancel_opt = acc_opt.my_cash;
    int c_base = ob_cancel_order_baseline(ob_base, &acc_base, buy_id_base);
    int c_opt  = ob_cancel_order_opt(ob_opt, &acc_opt, buy_id_opt);
    if (!c_base || !c_opt ||
        acc_base.my_cash != cash_before_cancel_base + expect_reserved ||
        acc_opt.my_cash  != cash_before_cancel_opt  + expect_reserved) {
        printf("FAIL: cancel did not refund exactly what was reserved\n");
        fails++;
    }

    // Cancelling twice, or an id that never existed, must both just fail.
    if (ob_cancel_order_baseline(ob_base, &acc_base, buy_id_base) ||
        ob_cancel_order_opt(ob_opt, &acc_opt, buy_id_opt) ||
        ob_cancel_order_baseline(ob_base, &acc_base, 999999) ||
        ob_cancel_order_opt(ob_opt, &acc_opt, 999999)) {
        printf("FAIL: cancel accepted a double-cancel or a bogus id\n");
        fails++;
    }

    if (!accounts_equal(&acc_base, &acc_opt) || !books_equal(ob_base, ob_opt)) {
        printf("FAIL: baseline/opt diverged after the limit order lifecycle\n");
        fails++;
    }

    if (fails == 0) {
        printf("PASS: limit order lifecycle (place/reserve, invalid rejection, "
               "cancel/refund, double-cancel rejection) identical on baseline and opt\n");
    }
    ob_destroy(ob_base); ob_destroy(ob_opt);
    return fails;
}

// ============================================================================
// Test 4: randomized stress mixing market orders, placements, and cancels.
// ============================================================================
static int test_random_stress(void) {
    L3OrderBook *ob_base = ob_create(), *ob_opt = ob_create();
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(ob_base, &acc_base, 100);
    ob_init_opt(ob_opt, &acc_opt, 100);

    int fails = 0;
    const int ITERATIONS = 100000;
    int open_ids[4096], open_count = 0, issued_count = 0;

    for (int t = 0; t < ITERATIONS && fails < 5; t++) {
        int op = rand_range(0, 4);
        int price = 90 + rand_range(0, 20); // spans both sides of the initial mid, and beyond it
        int qty = rand_range(1, 50);

        switch (op) {
        case 0: ob_market_buy_baseline(ob_base, &acc_base, qty, 0);
                ob_market_buy_opt(ob_opt, &acc_opt, qty, 0); break;
        case 1: ob_market_sell_baseline(ob_base, &acc_base, qty, 0);
                ob_market_sell_opt(ob_opt, &acc_opt, qty, 0); break;
        case 2: {
            int idb = ob_place_limit_buy_baseline(ob_base, &acc_base, price, qty);
            int ido = ob_place_limit_buy_opt(ob_opt, &acc_opt, price, qty);
            if (idb != ido) { printf("FAIL at op %d: place_limit_buy id diverged\n", t); fails++; }
            if (idb >= 0 && open_count < 4096) { open_ids[open_count++] = idb; issued_count++; }
            break;
        }
        case 3: {
            int idb = ob_place_limit_sell_baseline(ob_base, &acc_base, price, qty);
            int ido = ob_place_limit_sell_opt(ob_opt, &acc_opt, price, qty);
            if (idb != ido) { printf("FAIL at op %d: place_limit_sell id diverged\n", t); fails++; }
            if (idb >= 0 && open_count < 4096) { open_ids[open_count++] = idb; issued_count++; }
            break;
        }
        case 4:
            if (open_count > 0) {
                int i = rand_range(0, open_count - 1);
                int id = open_ids[i];
                open_ids[i] = open_ids[--open_count];
                ob_cancel_order_baseline(ob_base, &acc_base, id);
                ob_cancel_order_opt(ob_opt, &acc_opt, id);
            }
            break;
        }

        if (!accounts_equal(&acc_base, &acc_opt) || !books_equal(ob_base, ob_opt)) {
            printf("FAIL at op %d (kind %d): baseline/opt diverged\n", t, op);
            fails++;
        }
    }

    if (fails == 0) {
        printf("PASS: %d randomized operations (market/limit/cancel, mixed valid and invalid), "
               "baseline and opt stayed identical after every single operation "
               "(final cash=%ld, inventory=%d, %d order ids issued)\n",
               ITERATIONS, acc_base.my_cash, acc_base.my_inventory, issued_count);
    }
    ob_destroy(ob_base); ob_destroy(ob_opt);
    return fails;
}

// ============================================================================
// Test 5: pre-trade risk limit.
// ============================================================================
static int test_risk_limit_clipping(void) {
    L3OrderBook *ob = ob_create();
    EngineAccount acc;
    ob_init_opt(ob, &acc, 100);
    int fails = 0;

    acc.my_inventory = MAX_POSITION - 10;
    int clipped = ob_market_buy_risk_checked_opt(ob, &acc, 100);
    if (clipped != 10) { printf("FAIL: near-limit buy clip: got %d, want 10\n", clipped); fails++; }

    acc.my_inventory = MAX_POSITION;
    clipped = ob_market_buy_risk_checked_opt(ob, &acc, 5);
    if (clipped != 0) { printf("FAIL: exactly-at-limit buy clip: got %d, want 0\n", clipped); fails++; }

    acc.my_inventory = MAX_POSITION - 100;
    clipped = ob_market_buy_risk_checked_opt(ob, &acc, 30);
    if (clipped != 30) { printf("FAIL: within-room buy clip: got %d, want 30\n", clipped); fails++; }

    acc.my_inventory = -(MAX_POSITION - 10);
    clipped = ob_market_sell_risk_checked_opt(ob, &acc, 100);
    if (clipped != 10) { printf("FAIL: near-limit sell clip: got %d, want 10\n", clipped); fails++; }

    if (fails == 0) {
        printf("PASS: risk-limit clipping arithmetic (near-limit, exactly-at-limit, "
               "within-room, both sides) all correct\n");
    }
    ob_destroy(ob);
    return fails;
}

static int test_risk_limit_end_to_end(void) {
    L3OrderBook *ob_base = ob_create(), *ob_opt = ob_create();
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(ob_base, &acc_base, 100);
    ob_init_opt(ob_opt, &acc_opt, 100);

    int fails = 0;
    const int ITERATIONS = 20000;
    for (int t = 0; t < ITERATIONS && fails < 5; t++) {
        int qty = rand_range(1, 80);
        if (rand_range(0, 1)) {
            ob_market_buy_risk_checked_baseline(ob_base, &acc_base, qty);
            ob_market_buy_risk_checked_opt(ob_opt, &acc_opt, qty);
        } else {
            ob_market_sell_risk_checked_baseline(ob_base, &acc_base, qty);
            ob_market_sell_risk_checked_opt(ob_opt, &acc_opt, qty);
        }
        // Replenish liquidity periodically so the risk limit is what's
        // actually constraining fills, not the book running dry.
        if (t % 20 == 0) {
            ob_add_order_baseline(ob_base, 1, 99, acc_base.global_order_id++, 200, 0);
            ob_add_order_opt(ob_opt, 1, 99, acc_opt.global_order_id++, 200, 0);
            ob_add_order_baseline(ob_base, 0, 101, acc_base.global_order_id++, 200, 0);
            ob_add_order_opt(ob_opt, 0, 101, acc_opt.global_order_id++, 200, 0);
        }
        if (acc_base.my_inventory > MAX_POSITION || acc_base.my_inventory < -MAX_POSITION ||
            acc_opt.my_inventory > MAX_POSITION || acc_opt.my_inventory < -MAX_POSITION) {
            printf("FAIL at op %d: |inventory| exceeded MAX_POSITION (base=%d opt=%d)\n",
                   t, acc_base.my_inventory, acc_opt.my_inventory);
            fails++;
        }
        if (!accounts_equal(&acc_base, &acc_opt)) {
            printf("FAIL at op %d: account state diverged\n", t);
            fails++;
        }
    }

    if (fails == 0) {
        printf("PASS: %d risk-checked market orders, |inventory| never exceeded "
               "MAX_POSITION=%d on either engine, baseline and opt stayed identical "
               "(final inventory base=%d opt=%d)\n",
               ITERATIONS, MAX_POSITION, acc_base.my_inventory, acc_opt.my_inventory);
    }
    ob_destroy(ob_base); ob_destroy(ob_opt);
    return fails;
}

// ============================================================================
// Test 6: cancel-replace (order modification) -- deterministic lifecycle,
// then folded into a randomized stress run alongside market/limit/cancel.
// ============================================================================
static int test_modify_order_lifecycle(void) {
    L3OrderBook *ob_base = ob_create(), *ob_opt = ob_create();
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(ob_base, &acc_base, 100);
    ob_init_opt(ob_opt, &acc_opt, 100);

    int fails = 0;

    int id_base = ob_place_limit_buy_baseline(ob_base, &acc_base, 99, 20);
    int id_opt  = ob_place_limit_buy_opt(ob_opt, &acc_opt, 99, 20);
    if (id_base < 0 || id_base != id_opt) {
        printf("FAIL: setup placement diverged or failed (base=%d opt=%d)\n", id_base, id_opt);
        fails++;
    }

    // --- modify_qty: increase, same id, same price, extra cash reserved ---
    long cash_before_inc_base = acc_base.my_cash, cash_before_inc_opt = acc_opt.my_cash;
    int r_base = ob_modify_qty_baseline(ob_base, &acc_base, id_base, 35);
    int r_opt  = ob_modify_qty_opt(ob_opt, &acc_opt, id_opt, 35);
    long expect_extra = (long)(35 - 20) * 99;
    if (!r_base || !r_opt) {
        printf("FAIL: modify_qty increase was rejected (base=%d opt=%d)\n", r_base, r_opt);
        fails++;
    }
    if (acc_base.my_cash != cash_before_inc_base - expect_extra ||
        acc_opt.my_cash  != cash_before_inc_opt  - expect_extra) {
        printf("FAIL: modify_qty increase reserved the wrong amount of cash\n");
        fails++;
    }
    if (!order_live_at(ob_base, 1, 99, id_base, 35) || !order_live_at(ob_opt, 1, 99, id_opt, 35)) {
        printf("FAIL: modify_qty increase did not update the resting order in place\n");
        fails++;
    }

    // --- modify_qty: decrease, refund the difference ---
    long cash_before_dec_base = acc_base.my_cash, cash_before_dec_opt = acc_opt.my_cash;
    r_base = ob_modify_qty_baseline(ob_base, &acc_base, id_base, 5);
    r_opt  = ob_modify_qty_opt(ob_opt, &acc_opt, id_opt, 5);
    long expect_refund = (long)(35 - 5) * 99;
    if (!r_base || !r_opt ||
        acc_base.my_cash != cash_before_dec_base + expect_refund ||
        acc_opt.my_cash  != cash_before_dec_opt  + expect_refund) {
        printf("FAIL: modify_qty decrease did not refund the exact difference\n");
        fails++;
    }

    // --- modify_qty: increase far beyond available cash must be rejected, untouched ---
    long cash_before_reject_base = acc_base.my_cash, cash_before_reject_opt = acc_opt.my_cash;
    r_base = ob_modify_qty_baseline(ob_base, &acc_base, id_base, 10000000);
    r_opt  = ob_modify_qty_opt(ob_opt, &acc_opt, id_opt, 10000000);
    if (r_base || r_opt ||
        acc_base.my_cash != cash_before_reject_base || acc_opt.my_cash != cash_before_reject_opt ||
        !order_live_at(ob_base, 1, 99, id_base, 5) || !order_live_at(ob_opt, 1, 99, id_opt, 5)) {
        printf("FAIL: modify_qty increase beyond available cash was accepted or mutated state\n");
        fails++;
    }

    // --- modify_qty: invalid new_qty and bogus id must both just fail ---
    if (ob_modify_qty_baseline(ob_base, &acc_base, id_base, 0) ||
        ob_modify_qty_opt(ob_opt, &acc_opt, id_opt, 0) ||
        ob_modify_qty_baseline(ob_base, &acc_base, 999999, 10) ||
        ob_modify_qty_opt(ob_opt, &acc_opt, 999999, 10)) {
        printf("FAIL: modify_qty accepted a non-positive qty or a bogus order id\n");
        fails++;
    }

    // --- modify_price: move to a different price, new id, old order gone ---
    long cash_before_move_base = acc_base.my_cash, cash_before_move_opt = acc_opt.my_cash;
    int new_id_base = ob_modify_price_baseline(ob_base, &acc_base, id_base, 97, 12);
    int new_id_opt  = ob_modify_price_opt(ob_opt, &acc_opt, id_opt, 97, 12);
    if (new_id_base < 0 || new_id_opt < 0 || new_id_base != new_id_opt || new_id_base == id_base) {
        printf("FAIL: modify_price did not return a fresh id (old=%d base=%d opt=%d)\n",
               id_base, new_id_base, new_id_opt);
        fails++;
    }
    if (order_live_at(ob_base, 1, 99, id_base, 5) || order_live_at(ob_opt, 1, 99, id_opt, 5)) {
        printf("FAIL: modify_price left the old order still resting at its old price\n");
        fails++;
    }
    if (!order_live_at(ob_base, 1, 97, new_id_base, 12) || !order_live_at(ob_opt, 1, 97, new_id_opt, 12)) {
        printf("FAIL: modify_price did not place the new order at the new price\n");
        fails++;
    }
    long expect_move_cash_delta = (long)5 * 99 - (long)12 * 97;
    if (acc_base.my_cash != cash_before_move_base + expect_move_cash_delta ||
        acc_opt.my_cash  != cash_before_move_opt  + expect_move_cash_delta) {
        printf("FAIL: modify_price's net cash effect (refund old, reserve new) was wrong\n");
        fails++;
    }

    // --- modify_price rejected (invalid price): old order must be untouched ---
    long cash_before_bad_move_base = acc_base.my_cash, cash_before_bad_move_opt = acc_opt.my_cash;
    int bad_base = ob_modify_price_baseline(ob_base, &acc_base, new_id_base, -5, 12);
    int bad_opt  = ob_modify_price_opt(ob_opt, &acc_opt, new_id_opt, -5, 12);
    if (bad_base != -1 || bad_opt != -1 ||
        acc_base.my_cash != cash_before_bad_move_base || acc_opt.my_cash != cash_before_bad_move_opt ||
        !order_live_at(ob_base, 1, 97, new_id_base, 12) || !order_live_at(ob_opt, 1, 97, new_id_opt, 12)) {
        printf("FAIL: rejected modify_price mutated state or lost the original order\n");
        fails++;
    }

    if (!accounts_equal(&acc_base, &acc_opt) || !books_equal(ob_base, ob_opt)) {
        printf("FAIL: baseline/opt diverged after the cancel-replace sequence\n");
        fails++;
    }

    if (fails == 0) {
        printf("PASS: cancel-replace lifecycle (qty increase/decrease/rejected-increase, "
               "price move with fresh id, rejected move leaving the original untouched) "
               "identical on baseline and opt\n");
    }
    ob_destroy(ob_base); ob_destroy(ob_opt);
    return fails;
}

static int test_modify_random_stress(void) {
    L3OrderBook *ob_base = ob_create(), *ob_opt = ob_create();
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(ob_base, &acc_base, 100);
    ob_init_opt(ob_opt, &acc_opt, 100);

    int fails = 0;
    const int ITERATIONS = 50000;
    int open_ids[4096], open_count = 0, issued_count = 0;

    for (int t = 0; t < ITERATIONS && fails < 5; t++) {
        int op = rand_range(0, 5);
        int price = 90 + rand_range(0, 20);
        int qty = rand_range(1, 50);

        switch (op) {
        case 0: ob_market_buy_baseline(ob_base, &acc_base, qty, 0);
                ob_market_buy_opt(ob_opt, &acc_opt, qty, 0); break;
        case 1: ob_market_sell_baseline(ob_base, &acc_base, qty, 0);
                ob_market_sell_opt(ob_opt, &acc_opt, qty, 0); break;
        case 2: {
            int idb = ob_place_limit_buy_baseline(ob_base, &acc_base, price, qty);
            int ido = ob_place_limit_buy_opt(ob_opt, &acc_opt, price, qty);
            if (idb != ido) { printf("FAIL at op %d: place_limit_buy id diverged\n", t); fails++; }
            if (idb >= 0 && open_count < 4096) { open_ids[open_count++] = idb; issued_count++; }
            break;
        }
        case 3:
            if (open_count > 0) {
                int i = rand_range(0, open_count - 1);
                int id = open_ids[i];
                open_ids[i] = open_ids[--open_count];
                ob_cancel_order_baseline(ob_base, &acc_base, id);
                ob_cancel_order_opt(ob_opt, &acc_opt, id);
            }
            break;
        case 4:
            if (open_count > 0) {
                int i = rand_range(0, open_count - 1);
                ob_modify_qty_baseline(ob_base, &acc_base, open_ids[i], qty);
                ob_modify_qty_opt(ob_opt, &acc_opt, open_ids[i], qty);
            }
            break;
        case 5:
            if (open_count > 0) {
                int i = rand_range(0, open_count - 1);
                int new_id_base = ob_modify_price_baseline(ob_base, &acc_base, open_ids[i], price, qty);
                int new_id_opt  = ob_modify_price_opt(ob_opt, &acc_opt, open_ids[i], price, qty);
                if (new_id_base != new_id_opt) {
                    printf("FAIL at op %d: modify_price id diverged (base=%d opt=%d)\n",
                           t, new_id_base, new_id_opt);
                    fails++;
                }
                if (new_id_base >= 0) open_ids[i] = new_id_base;
            }
            break;
        }

        if (!accounts_equal(&acc_base, &acc_opt) || !books_equal(ob_base, ob_opt)) {
            printf("FAIL at op %d (kind %d): baseline/opt diverged\n", t, op);
            fails++;
        }
    }

    if (fails == 0) {
        printf("PASS: %d randomized operations (market/limit/cancel/modify_qty/modify_price, "
               "mixed valid and invalid), baseline and opt stayed identical after every single "
               "operation (%d order ids issued)\n", ITERATIONS, issued_count);
    }
    ob_destroy(ob_base); ob_destroy(ob_opt);
    return fails;
}

// ============================================================================
// Test 7: third-party (is_mine=0) order cancel/qty-reduce -- for replaying
// real order flow (LOBSTER etc.), where cancelled/reduced orders
// overwhelmingly belong to anonymous participants, not "the player".
// Confirms the existing is_mine-gated functions correctly ignore such an
// order, and the any-* functions correctly find and mutate it.
// ============================================================================
static int test_any_order_cancel_reduce_lifecycle(void) {
    L3OrderBook *ob_base = ob_create(), *ob_opt = ob_create();
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(ob_base, &acc_base, 100);
    ob_init_opt(ob_opt, &acc_opt, 100);

    int fails = 0;

    if (!ob_add_order_baseline(ob_base, /*is_bid=*/1, 99, 7001, 20, /*is_mine=*/0) ||
        !ob_add_order_opt(ob_opt, /*is_bid=*/1, 99, 7001, 20, /*is_mine=*/0)) {
        printf("FAIL: setup insertion of third-party order failed\n");
        ob_destroy(ob_base); ob_destroy(ob_opt);
        return 1;
    }

    // --- the existing is_mine-gated functions must ignore it entirely ---
    if (ob_cancel_order_baseline(ob_base, &acc_base, 7001) ||
        ob_cancel_order_opt(ob_opt, &acc_opt, 7001) ||
        ob_modify_qty_baseline(ob_base, &acc_base, 7001, 5) ||
        ob_modify_qty_opt(ob_opt, &acc_opt, 7001, 5)) {
        printf("FAIL: is_mine-gated cancel/modify_qty touched a third-party order\n");
        fails++;
    }
    if (!order_live_at(ob_base, 1, 99, 7001, 20) || !order_live_at(ob_opt, 1, 99, 7001, 20)) {
        printf("FAIL: third-party order was mutated by an is_mine-gated call\n");
        fails++;
    }

    // --- reject an over-large reduce, nothing mutated ---
    if (ob_reduce_order_qty_any_baseline(ob_base, 7001, 999) ||
        ob_reduce_order_qty_any_opt(ob_opt, 7001, 999)) {
        printf("FAIL: over-large qty reduce was accepted\n");
        fails++;
    }

    // --- partial reduce, order stays live at new qty ---
    if (!ob_reduce_order_qty_any_baseline(ob_base, 7001, 8) ||
        !ob_reduce_order_qty_any_opt(ob_opt, 7001, 8)) {
        printf("FAIL: valid partial qty reduce was rejected\n");
        fails++;
    }
    if (!order_live_at(ob_base, 1, 99, 7001, 12) || !order_live_at(ob_opt, 1, 99, 7001, 12)) {
        printf("FAIL: partial qty reduce left the wrong quantity resting\n");
        fails++;
    }

    // --- reduce down to exactly 0 fully removes it ---
    if (!ob_reduce_order_qty_any_baseline(ob_base, 7001, 12) ||
        !ob_reduce_order_qty_any_opt(ob_opt, 7001, 12)) {
        printf("FAIL: exact-zero qty reduce was rejected\n");
        fails++;
    }
    if (order_live_at(ob_base, 1, 99, 7001, 0) || order_live_at(ob_opt, 1, 99, 7001, 0)) {
        printf("FAIL: exact-zero qty reduce did not fully remove the order\n");
        fails++;
    }

    // --- another third-party order, cancel it ---
    if (!ob_add_order_baseline(ob_base, /*is_bid=*/0, 101, 7002, 15, /*is_mine=*/0) ||
        !ob_add_order_opt(ob_opt, /*is_bid=*/0, 101, 7002, 15, /*is_mine=*/0)) {
        printf("FAIL: setup insertion of second third-party order failed\n");
        ob_destroy(ob_base); ob_destroy(ob_opt);
        return ++fails;
    }
    if (!ob_cancel_order_any_baseline(ob_base, 7002) || !ob_cancel_order_any_opt(ob_opt, 7002)) {
        printf("FAIL: ob_cancel_order_any_* rejected a live third-party order\n");
        fails++;
    }
    if (order_live_at(ob_base, 0, 101, 7002, 0) || order_live_at(ob_opt, 0, 101, 7002, 0)) {
        printf("FAIL: ob_cancel_order_any_* did not remove the order\n");
        fails++;
    }

    // --- bogus id: both any-* functions must just fail ---
    if (ob_cancel_order_any_baseline(ob_base, 999999) || ob_cancel_order_any_opt(ob_opt, 999999) ||
        ob_reduce_order_qty_any_baseline(ob_base, 999999, 1) || ob_reduce_order_qty_any_opt(ob_opt, 999999, 1)) {
        printf("FAIL: any-* functions accepted a bogus order id\n");
        fails++;
    }

    if (!accounts_equal(&acc_base, &acc_opt) || !books_equal(ob_base, ob_opt)) {
        printf("FAIL: baseline/opt diverged after the any-cancel/reduce sequence\n");
        fails++;
    }

    if (fails == 0) {
        printf("PASS: third-party (is_mine=0) order cancel/qty-reduce (is_mine-gated calls "
               "correctly ignore it, any-* partial reduce/exact-zero-removal/reject-over-large/"
               "bogus-id all correct) identical on baseline and opt\n");
    }
    ob_destroy(ob_base); ob_destroy(ob_opt);
    return fails;
}

// ============================================================================
// Test 8: a resting order getting filled must not touch the leg that was
// already reserved at placement (cash for a buy, inventory for a sell) --
// found by real LOBSTER-driven backtest P&L coming out wrong: a resting
// fill was settling BOTH legs, on top of the reservation place_limit_*
// already made, silently double-charging every single fill.
// ============================================================================
static int test_resting_fill_settlement(void) {
    L3OrderBook *ob_base = ob_create(), *ob_opt = ob_create();
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(ob_base, &acc_base, 100);
    ob_init_opt(ob_opt, &acc_opt, 100);

    int fails = 0;

    // --- resting buy, filled by noise sell flow (is_player=0) ---
    // Placed at base_price (100): better than the seeded best bid (99),
    // so it's the sole order on a brand-new level and gets hit first --
    // nothing from ob_init_* is ahead of it in the queue.
    long cash_before = acc_base.my_cash;
    long inv_before = acc_base.my_inventory;
    int buy_id_base = ob_place_limit_buy_baseline(ob_base, &acc_base, 100, 10);
    int buy_id_opt  = ob_place_limit_buy_opt(ob_opt, &acc_opt, 100, 10);
    if (buy_id_base < 0 || buy_id_opt < 0) {
        printf("FAIL: setup resting buy placement failed\n");
        ob_destroy(ob_base); ob_destroy(ob_opt);
        return 1;
    }
    long reserved = cash_before - acc_base.my_cash; // 10 * 100
    // Noise sell flow walks the bids and should fill our resting buy
    // completely (it's the sole order at the new best bid).
    ob_market_sell_baseline(ob_base, &acc_base, 10, /*is_player=*/0);
    ob_market_sell_opt(ob_opt, &acc_opt, 10, /*is_player=*/0);
    if (order_live_at(ob_base, 1, 100, buy_id_base, 0) || order_live_at(ob_opt, 1, 100, buy_id_opt, 0)) {
        printf("FAIL: resting buy wasn't fully filled by noise sell flow\n");
        fails++;
    }
    // The fill must settle inventory (the unreserved leg) and leave cash
    // exactly where the reservation left it -- not deduct the price again.
    if (acc_base.my_cash != cash_before - reserved || acc_opt.my_cash != cash_before - reserved) {
        printf("FAIL: resting buy fill double-charged cash (base=%ld opt=%ld, expected %ld)\n",
               acc_base.my_cash, acc_opt.my_cash, cash_before - reserved);
        fails++;
    }
    if (acc_base.my_inventory != inv_before + 10 || acc_opt.my_inventory != inv_before + 10) {
        printf("FAIL: resting buy fill didn't credit inventory (base=%d opt=%d, expected %ld)\n",
               acc_base.my_inventory, acc_opt.my_inventory, inv_before + 10);
        fails++;
    }

    // --- resting sell, filled by noise buy flow (is_player=0) ---
    // Placed at base_price (100): better than the seeded best ask (101),
    // same reasoning as the buy above -- sole order, hit first.
    cash_before = acc_base.my_cash;
    inv_before = acc_base.my_inventory;
    int sell_id_base = ob_place_limit_sell_baseline(ob_base, &acc_base, 100, 10);
    int sell_id_opt  = ob_place_limit_sell_opt(ob_opt, &acc_opt, 100, 10);
    if (sell_id_base < 0 || sell_id_opt < 0) {
        printf("FAIL: setup resting sell placement failed\n");
        ob_destroy(ob_base); ob_destroy(ob_opt);
        return ++fails;
    }
    long inv_reserved = inv_before - acc_base.my_inventory; // 10, reserved at placement
    ob_market_buy_baseline(ob_base, &acc_base, 10, /*is_player=*/0);
    ob_market_buy_opt(ob_opt, &acc_opt, 10, /*is_player=*/0);
    if (order_live_at(ob_base, 0, 100, sell_id_base, 0) || order_live_at(ob_opt, 0, 100, sell_id_opt, 0)) {
        printf("FAIL: resting sell wasn't fully filled by noise buy flow\n");
        fails++;
    }
    // Inventory (the reserved leg) must stay exactly where the
    // reservation left it; cash (unreserved) gets the sale proceeds.
    if (acc_base.my_inventory != inv_before - inv_reserved || acc_opt.my_inventory != inv_before - inv_reserved) {
        printf("FAIL: resting sell fill double-charged inventory (base=%d opt=%d, expected %ld)\n",
               acc_base.my_inventory, acc_opt.my_inventory, inv_before - inv_reserved);
        fails++;
    }
    if (acc_base.my_cash != cash_before + (long)10 * 100 || acc_opt.my_cash != cash_before + (long)10 * 100) {
        printf("FAIL: resting sell fill didn't credit sale proceeds (base=%ld opt=%ld, expected %ld)\n",
               acc_base.my_cash, acc_opt.my_cash, cash_before + (long)10 * 100);
        fails++;
    }

    if (!accounts_equal(&acc_base, &acc_opt) || !books_equal(ob_base, ob_opt)) {
        printf("FAIL: baseline/opt diverged after resting-fill settlement\n");
        fails++;
    }

    if (fails == 0) {
        printf("PASS: resting order fill settles only the unreserved leg (cash for a buy fill, "
               "inventory for a sell fill), matching what place_limit_* reserved up front, "
               "identical on baseline and opt\n");
    }
    ob_destroy(ob_base); ob_destroy(ob_opt);
    return fails;
}

int main(void) {
    int failures = 0;
    failures += test_tick_replay();
    failures += test_no_capacity_bounds();
    failures += test_limit_order_lifecycle();
    failures += test_random_stress();
    failures += test_risk_limit_clipping();
    failures += test_risk_limit_end_to_end();
    failures += test_modify_order_lifecycle();
    failures += test_modify_random_stress();
    failures += test_any_order_cancel_reduce_lifecycle();
    failures += test_resting_fill_settlement();

    if (failures == 0) {
        printf("\nALL TESTS PASSED\n");
        return 0;
    }
    printf("\n%d TEST GROUP(S) FAILED\n", failures);
    return 1;
}
