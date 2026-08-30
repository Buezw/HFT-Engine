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

    printf("[Per-tick latency distribution, ns — mean hides tail behavior,\n");
    printf(" so this is the number that actually matters for a hard per-tick budget]\n");
    printf("  %-12s %8s %8s %8s %8s %8s\n", "", "p50", "p90", "p99", "p99.9", "max");
    printf("  %-12s %8lld %8lld %8lld %8lld %8lld\n", "baseline",
           percentile(base_tick_ns, TICKS, 0.50), percentile(base_tick_ns, TICKS, 0.90),
           percentile(base_tick_ns, TICKS, 0.99), percentile(base_tick_ns, TICKS, 0.999),
           base_tick_ns[TICKS - 1]);
    printf("  %-12s %8lld %8lld %8lld %8lld %8lld\n", "optimized",
           percentile(opt_tick_ns, TICKS, 0.50), percentile(opt_tick_ns, TICKS, 0.90),
           percentile(opt_tick_ns, TICKS, 0.99), percentile(opt_tick_ns, TICKS, 0.999),
           opt_tick_ns[TICKS - 1]);
    printf("  speedup at p50: %.2fx, at p99: %.2fx\n",
           (double)percentile(base_tick_ns, TICKS, 0.50) / (double)percentile(opt_tick_ns, TICKS, 0.50),
           (double)percentile(base_tick_ns, TICKS, 0.99) / (double)percentile(opt_tick_ns, TICKS, 0.99));
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
        const int CALLS = 5000000;
        long long t0 = now_ns();
        for (int i = 0; i < CALLS; i++) ob_update_total_qty_baseline(&ob);
        long long dt = now_ns() - t0;
        printf("[Isolated cost of the full-rescan ob_update_total_qty_baseline,\n");
        printf(" book at max depth (%d levels x %d orders each)]\n",
               MAX_PRICE_LEVELS, MAX_ORDERS_PER_LVL);
        printf("  %lld ns / %d calls -> %.1f ns/call\n", dt, CALLS, (double)dt / CALLS);
        printf("  In the optimized engine this call is REMOVED from the hot\n");
        printf("  per-tick path entirely (0 ns/tick), since total_qty is kept\n");
        printf("  in sync incrementally by ob_market_*_opt / ob_add_order_opt.\n");
        printf("  This is the dominant contribution to the full-tick speedup above.\n");
    }

    return 0;
}
