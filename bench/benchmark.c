// ============================================================================
// benchmark.c
//
// Measures wall-clock time for the baseline vs optimized engine under the
// identical synthetic workload used in test_correctness.c, using
// clock_gettime(CLOCK_MONOTONIC) for nanosecond-resolution timing.
//
// Reports:
//   - total time for N ticks
//   - average ns/tick, plus p50/p90/p99/max ns/tick (tail latency matters
//     more than the mean for anything that has to run inside a hard
//     per-tick budget — a 1.3x-better average is a different claim than a
//     1.3x-better p99, and only one of them is what a jittery outlier tick
//     would actually cost you)
//   - average ns/clean_ghosts call (isolated, since that's one of the two
//     targeted optimizations)
//
// NOTE ON REPRESENTATIVENESS: this runs on an x86_64 dev machine, not the
// DE1-SoC's RISC-V core, so absolute ns numbers here are NOT the numbers
// you'd see on the actual board (different ISA, no cache hierarchy of that
// exact shape, different clock speed). What IS representative is the
// *relative* speedup between baseline and optimized, since both run the
// same instructions modulo the actual algorithmic difference, on the same
// machine, in the same run. Report the ratio, not the absolute ns, when
// talking about what this proves for the bare-metal target.
// ============================================================================

// clock_gettime/CLOCK_MONOTONIC are POSIX, not ISO C — invisible under
// strict -std=c11 without this feature-test macro (must be defined before
// any system header is included, hence its position here).
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "orderbook_engine.h"

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int cmp_ll(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return (x > y) - (x < y);
}

// samples must already be sorted ascending.
static long long percentile(const long long *sorted_samples, int n, double p) {
    int idx = (int)(p * (n - 1));
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return sorted_samples[idx];
}

// Runs the same synthetic per-tick workload as test_correctness.c against
// whichever function pointers are passed in, and returns total elapsed ns.
typedef int  (*add_fn)(L3PriceLevel *, L3Order);
typedef void (*clean_fn)(L3PriceLevel *);
typedef void (*update_fn)(L3OrderBook *);
typedef void (*buy_fn)(L3OrderBook *, EngineAccount *, int, int);
typedef void (*sell_fn)(L3OrderBook *, EngineAccount *, int, int);

// tick_ns_out, if non-NULL, must point to an array of at least `ticks`
// long longs; run_ticks fills it with the wall-clock cost of each
// individual tick, for percentile reporting in main().
static long long run_ticks(int ticks,
                            add_fn add, clean_fn clean, update_fn update,
                            buy_fn buy, sell_fn sell,
                            int call_update_every_tick,
                            long long *out_clean_ns, long *out_clean_calls,
                            long long *tick_ns_out) {
    L3OrderBook ob;
    EngineAccount acc;
    if (update == ob_update_total_qty_baseline) {
        ob_init_baseline(&ob, &acc, 100);
    } else {
        ob_init_opt(&ob, &acc, 100);
    }

    long long clean_ns_total = 0;
    long clean_calls = 0;

    long long t0 = now_ns();
    for (int t = 0; t < ticks; t++) {
        long long tick_start = tick_ns_out ? now_ns() : 0;

        if (t % 2 == 0) {
            for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
                int q = (t * 13 + i * 7) % 40 + 10;
                add(&ob.bids[i], (L3Order){acc.global_order_id++, q, 0, 0, 1});
                q = (t * 17 + i * 11) % 40 + 10;
                add(&ob.asks[i], (L3Order){acc.global_order_id++, q, 0, 0, 1});
            }
        }

        int vol = 40;
        if ((t * 11) % 100 < 45) sell(&ob, &acc, vol, 0);
        else                     buy(&ob, &acc, vol, 0);

        if (t % 7 == 0) {
            if (t % 14 == 0) buy(&ob, &acc, 15, 1);
            else             sell(&ob, &acc, 15, 1);
        }

        long long c0 = now_ns();
        for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
            clean(&ob.bids[i]);
            clean(&ob.asks[i]);
        }
        clean_ns_total += now_ns() - c0;
        clean_calls += 2 * MAX_PRICE_LEVELS;

        if (call_update_every_tick) update(&ob);

        if (tick_ns_out) tick_ns_out[t] = now_ns() - tick_start;
    }
    long long t1 = now_ns();

    *out_clean_ns = clean_ns_total;
    *out_clean_calls = clean_calls;
    return t1 - t0;
}

int main(void) {
    const int TICKS = 2000000;
    const int WARMUP = 200000;

    long long clean_ns; long clean_calls;

    // Warm up (page faults, branch predictor, etc.) before the real measurement.
    run_ticks(WARMUP, ob_add_order_baseline, ob_clean_ghosts_baseline,
              ob_update_total_qty_baseline, ob_market_buy_baseline, ob_market_sell_baseline,
              1, &clean_ns, &clean_calls, NULL);
    run_ticks(WARMUP, ob_add_order_opt, ob_clean_ghosts_opt,
              ob_update_total_qty_opt, ob_market_buy_opt, ob_market_sell_opt,
              0, &clean_ns, &clean_calls, NULL);

    long long *base_tick_ns = malloc((size_t)TICKS * sizeof(long long));
    long long *opt_tick_ns  = malloc((size_t)TICKS * sizeof(long long));
    if (!base_tick_ns || !opt_tick_ns) {
        fprintf(stderr, "out of memory allocating %d-tick latency sample buffers\n", TICKS);
        return 1;
    }

    long long base_total = run_ticks(TICKS, ob_add_order_baseline, ob_clean_ghosts_baseline,
                                      ob_update_total_qty_baseline, ob_market_buy_baseline,
                                      ob_market_sell_baseline,
                                      1, /* baseline calls update_total_qty every tick */
                                      &clean_ns, &clean_calls, base_tick_ns);
    long long base_clean_ns = clean_ns;
    long base_clean_calls = clean_calls;

    long long opt_total = run_ticks(TICKS, ob_add_order_opt, ob_clean_ghosts_opt,
                                     ob_update_total_qty_opt, ob_market_buy_opt,
                                     ob_market_sell_opt,
                                     0, /* opt never needs the full rescan on the hot path */
                                     &clean_ns, &clean_calls, opt_tick_ns);
    long long opt_clean_ns = clean_ns;
    long opt_clean_calls = clean_calls;

    qsort(base_tick_ns, (size_t)TICKS, sizeof(long long), cmp_ll);
    qsort(opt_tick_ns,  (size_t)TICKS, sizeof(long long), cmp_ll);

    printf("=== Benchmark: %d ticks (synthetic market-maker + player workload) ===\n\n", TICKS);

    printf("[Full tick loop, includes add/match/clean/%s]\n",
           "update_total_qty (baseline: every tick, opt: never on hot path)");
    printf("  baseline : %lld ns total  ->  %.1f ns/tick (mean)\n",
           base_total, (double)base_total / TICKS);
    printf("  optimized: %lld ns total  ->  %.1f ns/tick (mean)\n",
           opt_total, (double)opt_total / TICKS);
    printf("  speedup  : %.2fx (mean)\n\n", (double)base_total / (double)opt_total);

    long long base_p50 = percentile(base_tick_ns, TICKS, 0.50);
    long long base_p90 = percentile(base_tick_ns, TICKS, 0.90);
    long long base_p99 = percentile(base_tick_ns, TICKS, 0.99);
    long long base_p999 = percentile(base_tick_ns, TICKS, 0.999);
    long long opt_p50  = percentile(opt_tick_ns, TICKS, 0.50);
    long long opt_p90  = percentile(opt_tick_ns, TICKS, 0.90);
    long long opt_p99  = percentile(opt_tick_ns, TICKS, 0.99);
    long long opt_p999 = percentile(opt_tick_ns, TICKS, 0.999);

    printf("[Per-tick latency distribution, ns — mean hides tail behavior,\n");
    printf(" so this is the number that actually matters for a hard per-tick budget]\n");
    printf("  %-12s %8s %8s %8s %8s %8s\n", "", "p50", "p90", "p99", "p99.9", "max");
    printf("  %-12s %8lld %8lld %8lld %8lld %8lld\n", "baseline",
           base_p50, base_p90, base_p99, base_p999, base_tick_ns[TICKS - 1]);
    printf("  %-12s %8lld %8lld %8lld %8lld %8lld\n", "optimized",
           opt_p50, opt_p90, opt_p99, opt_p999, opt_tick_ns[TICKS - 1]);
    printf("  speedup at p50: %.2fx, at p99: %.2fx\n",
           (double)base_p50 / (double)opt_p50, (double)base_p99 / (double)opt_p99);
    printf("  (max is a single-sample outlier — OS scheduling noise on a\n");
    printf("   non-realtime dev machine, not signal; p99/p99.9 are the\n");
    printf("   numbers worth trusting from this environment)\n\n");

    free(base_tick_ns);
    free(opt_tick_ns);

    printf("[clean_ghosts only, isolated timing, %ld calls each]\n", base_clean_calls);
    printf("  baseline : %lld ns total  ->  %.1f ns/call\n",
           base_clean_ns, (double)base_clean_ns / base_clean_calls);
    printf("  optimized: %lld ns total  ->  %.1f ns/call\n",
           opt_clean_ns, (double)opt_clean_ns / opt_clean_calls);
    printf("  speedup  : %.2fx\n\n", (double)base_clean_ns / (double)opt_clean_ns);

    printf("NOTE: absolute ns figures are from this x86_64 dev machine, not the\n");
    printf("DE1-SoC RISC-V core the original main.c targets. The relative speedup\n");
    printf("is the meaningful number to carry over; see comment header in this file.\n\n");

    // Isolate just the cost of ob_update_total_qty_baseline's full rescan,
    // since that (not clean_ghosts) is the optimization expected to matter
    // most as book depth grows. Measured standalone, called once per tick,
    // on an already-populated book.
    {
        L3OrderBook ob; EngineAccount acc;
        ob_init_baseline(&ob, &acc, 100);
        // Fill every level to MAX_ORDERS_PER_LVL so the rescan does real work.
        for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
            while (ob_add_order_baseline(&ob.bids[i], (L3Order){acc.global_order_id++, 25, 0, 0, 1})) {}
            while (ob_add_order_baseline(&ob.asks[i], (L3Order){acc.global_order_id++, 25, 0, 0, 1})) {}
        }
        const int CALLS = 2000000;
        long long t0 = now_ns();
        for (int i = 0; i < CALLS; i++) ob_update_total_qty_baseline(&ob);
        long long dt = now_ns() - t0;
        double rescan_ns_per_call = (double)dt / CALLS;
        printf("[Isolated cost of the full-rescan ob_update_total_qty_baseline,\n");
        printf(" book at max depth (%d levels x %d orders each)]\n",
               MAX_PRICE_LEVELS, MAX_ORDERS_PER_LVL);
        printf("  %lld ns / %d calls -> %.1f ns/call\n", dt, CALLS, rescan_ns_per_call);
        printf("  In the optimized engine this call is REMOVED from the hot\n");
        printf("  per-tick path entirely (0 ns/tick), since total_qty is kept\n");
        printf("  in sync incrementally by ob_market_*_opt / ob_add_order_opt.\n");
        printf("  This is the dominant contribution to the full-tick speedup above.\n\n");

        // Single machine-parseable line consumed by `make depth-sweep` (see
        // Makefile / README): lets that target compile this file at several
        // MAX_ORDERS_PER_LVL values and tabulate how the gap grows with
        // depth, instead of just asserting that it does.
        printf("SWEEP_ROW depth=%d mean_ns_base=%.1f mean_ns_opt=%.1f "
               "p50_ns_base=%lld p50_ns_opt=%lld p99_ns_base=%lld p99_ns_opt=%lld "
               "rescan_ns_per_call_base=%.1f\n",
               MAX_ORDERS_PER_LVL,
               (double)base_total / TICKS, (double)opt_total / TICKS,
               base_p50, opt_p50, base_p99, opt_p99,
               rescan_ns_per_call);
    }

    // Isolate the cost of ob_cancel_order_opt (whole-book linear scan) vs
    // ob_cancel_order_opt_indexed (O(1)-amortized hashed lookup), both at
    // the SAME worst-case position: bids/asks[0..N-2] filled to capacity
    // with non-mine liquidity, and the order under test placed last in
    // asks[MAX_PRICE_LEVELS-1] — the last slot find_live_mine_order's
    // linear scan would ever reach, since it walks all bids levels before
    // any asks level. Both variants get the identical setup, so this
    // isolates the algorithmic difference, not a lucky/unlucky position.
    {
        static L3OrderBook ob_lin, ob_idx;
        static EngineAccount acc_lin, acc_idx;
        static OrderIndex idx; // ORDER_INDEX_CAPACITY buckets — static, not on the stack

        ob_init_opt(&ob_lin, &acc_lin, 100);
        ob_init_opt(&ob_idx, &acc_idx, 100);
        ob_index_init(&idx);
        // Bypass the normal "earn inventory via a market buy" path — this
        // is a latency microbenchmark for cancel, not a test of sell-side
        // placement rules, and both engines need enough inventory to place
        // repeated limit sells without the risk-limit/inventory check
        // ever getting in the way of the measurement.
        acc_lin.my_inventory = 1000000;
        acc_idx.my_inventory = 1000000;

        for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
            while (ob_add_order_opt(&ob_lin.bids[i], (L3Order){acc_lin.global_order_id++, 5, 0, 0, 0})) {}
            while (ob_add_order_opt(&ob_idx.bids[i], (L3Order){acc_idx.global_order_id++, 5, 0, 0, 0})) {}
        }
        for (int i = 0; i < MAX_PRICE_LEVELS - 1; i++) {
            while (ob_add_order_opt(&ob_lin.asks[i], (L3Order){acc_lin.global_order_id++, 5, 0, 0, 0})) {}
            while (ob_add_order_opt(&ob_idx.asks[i], (L3Order){acc_idx.global_order_id++, 5, 0, 0, 0})) {}
        }
        int last = MAX_PRICE_LEVELS - 1;
        while (ob_lin.asks[last].order_count < MAX_ORDERS_PER_LVL - 1) {
            ob_add_order_opt(&ob_lin.asks[last], (L3Order){acc_lin.global_order_id++, 5, 0, 0, 0});
            ob_add_order_opt(&ob_idx.asks[last], (L3Order){acc_idx.global_order_id++, 5, 0, 0, 0});
        }

        const int CANCEL_CALLS = 500000;
        long long lin_ns = 0, idx_ns = 0;

        for (int i = 0; i < CANCEL_CALLS; i++) {
            int id = ob_place_limit_sell_opt(&ob_lin, &acc_lin, last, 1); // untimed setup
            long long t0 = now_ns();
            ob_cancel_order_opt(&ob_lin, &acc_lin, id);                  // TIMED: whole-book scan
            lin_ns += now_ns() - t0;
            ob_clean_ghosts_opt(&ob_lin.asks[last]);                     // untimed: reclaim the slot
        }
        for (int i = 0; i < CANCEL_CALLS; i++) {
            int id = ob_place_limit_sell_opt_indexed(&ob_idx, &acc_idx, &idx, last, 1); // untimed
            long long t0 = now_ns();
            ob_cancel_order_opt_indexed(&ob_idx, &acc_idx, &idx, id);    // TIMED: hashed lookup
            idx_ns += now_ns() - t0;
            ob_clean_ghosts_opt(&ob_idx.asks[last]);                     // untimed: reclaim the slot
        }

        printf("[Cancel-by-id, worst-case position (last slot of the last ask level, "
               "book depth %d), %d calls each]\n", MAX_ORDERS_PER_LVL, CANCEL_CALLS);
        printf("  linear scan (ob_cancel_order_opt)        : %lld ns total -> %.1f ns/call\n",
               lin_ns, (double)lin_ns / CANCEL_CALLS);
        printf("  indexed     (ob_cancel_order_opt_indexed) : %lld ns total -> %.1f ns/call\n",
               idx_ns, (double)idx_ns / CANCEL_CALLS);
        printf("  speedup: %.2fx\n", (double)lin_ns / (double)idx_ns);
        printf("  (at this depth the linear scan visits up to %d order slots per call; "
               "the indexed lookup does not grow with depth)\n\n",
               2 * MAX_PRICE_LEVELS * MAX_ORDERS_PER_LVL);
    }

    // Same worst-case-position methodology, this time for ob_modify_qty_opt
    // (uses the same whole-book linear scan cancel does internally) vs
    // ob_modify_qty_opt_indexed (O(1) hashed lookup) — a single resting
    // order at the last slot of the last ask level, modified repeatedly in
    // place rather than cancelled+replaced each time.
    {
        static L3OrderBook ob_lin, ob_idx;
        static EngineAccount acc_lin, acc_idx;
        static OrderIndex idx;

        ob_init_opt(&ob_lin, &acc_lin, 100);
        ob_init_opt(&ob_idx, &acc_idx, 100);
        ob_index_init(&idx);
        acc_lin.my_inventory = 1000000;
        acc_idx.my_inventory = 1000000;

        for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
            while (ob_add_order_opt(&ob_lin.bids[i], (L3Order){acc_lin.global_order_id++, 5, 0, 0, 0})) {}
            while (ob_add_order_opt(&ob_idx.bids[i], (L3Order){acc_idx.global_order_id++, 5, 0, 0, 0})) {}
        }
        for (int i = 0; i < MAX_PRICE_LEVELS - 1; i++) {
            while (ob_add_order_opt(&ob_lin.asks[i], (L3Order){acc_lin.global_order_id++, 5, 0, 0, 0})) {}
            while (ob_add_order_opt(&ob_idx.asks[i], (L3Order){acc_idx.global_order_id++, 5, 0, 0, 0})) {}
        }
        int last = MAX_PRICE_LEVELS - 1;
        while (ob_lin.asks[last].order_count < MAX_ORDERS_PER_LVL - 1) {
            ob_add_order_opt(&ob_lin.asks[last], (L3Order){acc_lin.global_order_id++, 5, 0, 0, 0});
            ob_add_order_opt(&ob_idx.asks[last], (L3Order){acc_idx.global_order_id++, 5, 0, 0, 0});
        }
        int lin_id = ob_place_limit_sell_opt(&ob_lin, &acc_lin, last, 5);
        int idx_id = ob_place_limit_sell_opt_indexed(&ob_idx, &acc_idx, &idx, last, 5);

        const int MODIFY_CALLS = 500000;
        long long lin_ns = 0, idx_ns = 0;

        for (int i = 0; i < MODIFY_CALLS; i++) {
            int new_qty = (i % 2 == 0) ? 6 : 5; // small alternation: stays live, no rejection risk
            long long t0 = now_ns();
            ob_modify_qty_opt(&ob_lin, &acc_lin, lin_id, new_qty); // TIMED: whole-book scan
            lin_ns += now_ns() - t0;
        }
        for (int i = 0; i < MODIFY_CALLS; i++) {
            int new_qty = (i % 2 == 0) ? 6 : 5;
            long long t0 = now_ns();
            ob_modify_qty_opt_indexed(&ob_idx, &acc_idx, &idx, idx_id, new_qty); // TIMED: hashed lookup
            idx_ns += now_ns() - t0;
        }

        printf("[Modify-qty-by-id, worst-case position (last slot of the last ask level, "
               "book depth %d), %d calls each]\n", MAX_ORDERS_PER_LVL, MODIFY_CALLS);
        printf("  linear scan (ob_modify_qty_opt)        : %lld ns total -> %.1f ns/call\n",
               lin_ns, (double)lin_ns / MODIFY_CALLS);
        printf("  indexed     (ob_modify_qty_opt_indexed) : %lld ns total -> %.1f ns/call\n",
               idx_ns, (double)idx_ns / MODIFY_CALLS);
        printf("  speedup: %.2fx\n", (double)lin_ns / (double)idx_ns);
        printf("  (same story as cancel above: the indexed lookup does not grow with depth,\n"
               "   which matters here specifically because a qty-only modify keeps its queue\n"
               "   position — it's the operation you'd actually want to be cheap if you're\n"
               "   repricing/resizing a resting quote often, not the price-changing kind that's\n"
               "   cancel-old+place-new either way)\n\n");
    }

    return 0;
}
