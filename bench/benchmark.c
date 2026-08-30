// ============================================================================
// benchmark.c
//
// Measures wall-clock time for the baseline vs optimized engine under the
// identical synthetic workload used in test_correctness.c, using
// clock_gettime(CLOCK_MONOTONIC) for nanosecond-resolution timing.
//
// Reports:
//   - total time for N ticks
//   - average ns/tick
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
#include <time.h>
#include "orderbook_engine.h"

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Runs the same synthetic per-tick workload as test_correctness.c against
// whichever function pointers are passed in, and returns total elapsed ns.
typedef void (*add_fn)(L3PriceLevel *, L3Order);
typedef void (*clean_fn)(L3PriceLevel *);
typedef void (*update_fn)(L3OrderBook *);
typedef void (*buy_fn)(L3OrderBook *, EngineAccount *, int, int);
typedef void (*sell_fn)(L3OrderBook *, EngineAccount *, int, int);

// Baseline doesn't have an ob_add_order_* — it pushes directly into the
// array. Wrap that as a matching function pointer signature so both arms of
// the benchmark loop look identical and we're not accidentally benchmarking
// different code shapes.
static void add_order_baseline_wrapper(L3PriceLevel *lvl, L3Order order) {
    lvl->queue[lvl->order_count++] = order;
    // Baseline relies on a later full ob_update_total_qty_baseline() call to
    // fix up total_qty, so intentionally do NOT update total_qty here — that
    // full rescan is exactly the cost we are measuring.
}

static long long run_ticks(int ticks,
                            add_fn add, clean_fn clean, update_fn update,
                            buy_fn buy, sell_fn sell,
                            int call_update_every_tick,
                            long long *out_clean_ns, long *out_clean_calls) {
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
        if (t % 2 == 0) {
            for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
                if (ob.bids[i].order_count < MAX_ORDERS_PER_LVL) {
                    int q = (t * 13 + i * 7) % 40 + 10;
                    add(&ob.bids[i], (L3Order){acc.global_order_id++, q, 0, 0, 1});
                }
                if (ob.asks[i].order_count < MAX_ORDERS_PER_LVL) {
                    int q = (t * 17 + i * 11) % 40 + 10;
                    add(&ob.asks[i], (L3Order){acc.global_order_id++, q, 0, 0, 1});
                }
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
    run_ticks(WARMUP, add_order_baseline_wrapper, ob_clean_ghosts_baseline,
              ob_update_total_qty_baseline, ob_market_buy_baseline, ob_market_sell_baseline,
              1, &clean_ns, &clean_calls);
    run_ticks(WARMUP, ob_add_order_opt, ob_clean_ghosts_opt,
              ob_update_total_qty_opt, ob_market_buy_opt, ob_market_sell_opt,
              0, &clean_ns, &clean_calls);

    long long base_total = run_ticks(TICKS, add_order_baseline_wrapper, ob_clean_ghosts_baseline,
                                      ob_update_total_qty_baseline, ob_market_buy_baseline,
                                      ob_market_sell_baseline,
                                      1, /* baseline calls update_total_qty every tick */
                                      &clean_ns, &clean_calls);
    long long base_clean_ns = clean_ns;
    long base_clean_calls = clean_calls;

    long long opt_total = run_ticks(TICKS, ob_add_order_opt, ob_clean_ghosts_opt,
                                     ob_update_total_qty_opt, ob_market_buy_opt,
                                     ob_market_sell_opt,
                                     0, /* opt never needs the full rescan on the hot path */
                                     &clean_ns, &clean_calls);
    long long opt_clean_ns = clean_ns;
    long opt_clean_calls = clean_calls;

    printf("=== Benchmark: %d ticks (synthetic market-maker + player workload) ===\n\n", TICKS);

    printf("[Full tick loop, includes add/match/clean/%s]\n",
           "update_total_qty (baseline: every tick, opt: never on hot path)");
    printf("  baseline : %lld ns total  ->  %.1f ns/tick\n",
           base_total, (double)base_total / TICKS);
    printf("  optimized: %lld ns total  ->  %.1f ns/tick\n",
           opt_total, (double)opt_total / TICKS);
    printf("  speedup  : %.2fx\n\n", (double)base_total / (double)opt_total);

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
            while (ob.bids[i].order_count < MAX_ORDERS_PER_LVL)
                ob.bids[i].queue[ob.bids[i].order_count++] = (L3Order){acc.global_order_id++, 25, 0, 0, 1};
            while (ob.asks[i].order_count < MAX_ORDERS_PER_LVL)
                ob.asks[i].queue[ob.asks[i].order_count++] = (L3Order){acc.global_order_id++, 25, 0, 0, 1};
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
