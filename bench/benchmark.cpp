// ============================================================================
// benchmark.cpp
//
// Measures wall-clock time for baseline vs opt under the same synthetic
// workload test_correctness.cpp uses, using std::chrono::steady_clock
// (CLOCK_MONOTONIC on Linux).
//
// Reports total/mean and p50/p90/p99/p99.9/max ns/tick for the full
// per-tick loop, then isolates cancel-by-id and modify-qty-by-id at
// several book depths -- that's the one real algorithmic difference left
// between baseline and opt now that both run on the same dynamic
// std::map/std::deque book (see orderbook_engine.h): baseline does a
// full linear scan across every order in the book to find one by id;
// opt keeps an order_id -> price index and jumps straight to the right
// level. `make depth-sweep` (`--depth-sweep` here) runs just that part,
// at several depths, to show the gap actually widening with depth --
// this used to require recompiling at different -DMAX_ORDERS_PER_LVL
// values; now it's a runtime loop, since there's no compile-time
// capacity left to vary.
//
// NOTE ON REPRESENTATIVENESS: this runs on an x86_64 dev machine, not the
// DE1-SoC's RISC-V core the engine originally targeted, so absolute ns
// numbers aren't what you'd see there. The *relative* speedup is the
// meaningful number to carry over.
// ============================================================================

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#include "orderbook_engine.h"

static long long now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// samples must already be sorted ascending.
static long long percentile(const std::vector<long long> &sorted_samples, double p) {
    int n = static_cast<int>(sorted_samples.size());
    int idx = static_cast<int>(p * (n - 1));
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return sorted_samples[idx];
}

using init_fn = void (*)(L3OrderBook *, EngineAccount *, int);
using add_fn  = int  (*)(L3OrderBook *, int, int, int, int, int);
using buy_fn  = void (*)(L3OrderBook *, EngineAccount *, int, int);
using sell_fn = void (*)(L3OrderBook *, EngineAccount *, int, int);
using place_fn = int (*)(L3OrderBook *, EngineAccount *, int, int);

// tick_ns_out, if non-null, must already hold at least `ticks` entries;
// run_ticks fills it with the wall-clock cost of each individual tick,
// for percentile reporting in main().
static long long run_ticks(int ticks, init_fn init, add_fn add, buy_fn buy, sell_fn sell,
                            std::vector<long long> *tick_ns_out) {
    L3OrderBook *ob = ob_create();
    EngineAccount acc;
    init(ob, &acc, 100);

    long long t0 = now_ns();
    for (int t = 0; t < ticks; t++) {
        long long tick_start = tick_ns_out ? now_ns() : 0;

        if (t % 2 == 0) {
            int q = (t * 13) % 40 + 10;
            add(ob, 1, 99, acc.global_order_id++, q, 0);
            q = (t * 17) % 40 + 10;
            add(ob, 0, 101, acc.global_order_id++, q, 0);
        }

        // Same balance test_correctness.cpp settled on: injection above
        // averages ~30/side every other tick, so consumption needs to be
        // in the same ballpark or the book just grows without bound
        // (there's no fixed capacity anymore to silently cap it for you).
        int vol = 100;
        if ((t * 11) % 100 < 45) sell(ob, &acc, vol, 0);
        else                     buy(ob, &acc, vol, 0);

        if (t % 7 == 0) {
            if (t % 14 == 0) buy(ob, &acc, 15, 1);
            else             sell(ob, &acc, 15, 1);
        }

        if (tick_ns_out) (*tick_ns_out)[t] = now_ns() - tick_start;
    }
    long long t1 = now_ns();
    ob_destroy(ob);
    return t1 - t0;
}

// Builds a book with `depth` distinct one-order bid levels (is_mine=0,
// so cancel/modify has to skip past all of them) plus one is_mine=1
// target order resting on the ask side -- the worst case for a linear
// scan that checks bids before asks, and the case an id-indexed lookup
// doesn't care about at all. Returns the target order's id.
static int build_worst_case(L3OrderBook *ob, EngineAccount *acc, add_fn add, place_fn place_sell,
                             int depth, int target_qty) {
    for (int i = 0; i < depth; i++) {
        add(ob, /*is_bid=*/1, 1000 + i, 500000 + i, 5, /*is_mine=*/0);
    }
    acc->my_inventory = 1000000; // enough to place the target sell regardless of depth
    return place_sell(ob, acc, 2000000, target_qty);
}

// cancel rebuilds the whole depth-`d` book every call (cancelling erases
// the target); modify doesn't need to, so the two get their own call
// counts and their own loops rather than sharing one.
static void time_cancel(int depth, int calls, long long &base_ns, long long &opt_ns) {
    base_ns = 0;
    opt_ns = 0;
    for (int i = 0; i < calls; i++) {
        // Destroy + recreate each iteration rather than cancelling the
        // depth liquidity back out order by order -- clearing it with
        // ob_cancel_order_any_* would be O(depth) per cancel (same
        // linear scan this benchmark exists to measure), making teardown
        // alone O(depth^2) across all `calls` iterations. Rebuilding from
        // scratch is O(depth) per iteration, full stop.
        L3OrderBook *ob_base = ob_create(), *ob_opt = ob_create();
        EngineAccount acc_base, acc_opt;
        ob_init_baseline(ob_base, &acc_base, 100);
        ob_init_opt(ob_opt, &acc_opt, 100);
        int id_base = build_worst_case(ob_base, &acc_base, ob_add_order_baseline, ob_place_limit_sell_baseline, depth, 1);
        int id_opt  = build_worst_case(ob_opt,  &acc_opt,  ob_add_order_opt,      ob_place_limit_sell_opt,      depth, 1);
        long long t0 = now_ns();
        ob_cancel_order_baseline(ob_base, &acc_base, id_base);
        base_ns += now_ns() - t0;
        t0 = now_ns();
        ob_cancel_order_opt(ob_opt, &acc_opt, id_opt);
        opt_ns += now_ns() - t0;
        ob_destroy(ob_base);
        ob_destroy(ob_opt);
    }
}

static void time_modify(int depth, int calls, long long &base_ns, long long &opt_ns) {
    L3OrderBook *ob_base = ob_create(), *ob_opt = ob_create();
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(ob_base, &acc_base, 100);
    ob_init_opt(ob_opt, &acc_opt, 100);

    int id_base = build_worst_case(ob_base, &acc_base, ob_add_order_baseline, ob_place_limit_sell_baseline, depth, 5);
    int id_opt  = build_worst_case(ob_opt,  &acc_opt,  ob_add_order_opt,      ob_place_limit_sell_opt,      depth, 5);
    base_ns = 0;
    opt_ns = 0;
    for (int i = 0; i < calls; i++) {
        int new_qty = (i % 2 == 0) ? 6 : 5; // small alternation: stays live, no rejection risk
        long long t0 = now_ns();
        ob_modify_qty_baseline(ob_base, &acc_base, id_base, new_qty);
        base_ns += now_ns() - t0;
        t0 = now_ns();
        ob_modify_qty_opt(ob_opt, &acc_opt, id_opt, new_qty);
        opt_ns += now_ns() - t0;
    }
    ob_destroy(ob_base);
    ob_destroy(ob_opt);
}

static void run_depth_sweep() {
    const int DEPTHS[] = {10, 50, 100, 500, 2000, 5000};
    const int CALLS_CANCEL = 2000;
    const int CALLS_MODIFY = 200000;

    printf("=== Depth sweep: cancel-by-id / modify-qty-by-id, baseline (linear scan) vs opt (indexed) ===\n\n");
    printf("%8s %14s %14s %10s | %14s %14s %10s\n",
           "depth", "cancel_base_ns", "cancel_opt_ns", "speedup", "modify_base_ns", "modify_opt_ns", "speedup");
    for (int depth : DEPTHS) {
        long long cb, co, mb, mo;
        time_cancel(depth, CALLS_CANCEL, cb, co);
        time_modify(depth, CALLS_MODIFY, mb, mo);
        double cancel_base_per = (double)cb / CALLS_CANCEL, cancel_opt_per = (double)co / CALLS_CANCEL;
        double modify_base_per = (double)mb / CALLS_MODIFY, modify_opt_per = (double)mo / CALLS_MODIFY;
        printf("%8d %14.1f %14.1f %9.2fx | %14.1f %14.1f %9.2fx\n",
               depth, cancel_base_per, cancel_opt_per, cancel_base_per / cancel_opt_per,
               modify_base_per, modify_opt_per, modify_base_per / modify_opt_per);
    }
    printf("\n(depth = distinct bid levels the target order has to be scanned past;\n"
           " baseline's cost should grow with depth, opt's shouldn't -- that's the\n"
           " whole point of keeping an id index instead of a linear scan)\n");
}

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--depth-sweep") == 0) {
        run_depth_sweep();
        return 0;
    }

    const int TICKS = 2000000;
    const int WARMUP = 200000;

    // Warm up (page faults, branch predictor, etc.) before the real measurement.
    run_ticks(WARMUP, ob_init_baseline, ob_add_order_baseline, ob_market_buy_baseline, ob_market_sell_baseline, nullptr);
    run_ticks(WARMUP, ob_init_opt,      ob_add_order_opt,      ob_market_buy_opt,      ob_market_sell_opt,      nullptr);

    std::vector<long long> base_tick_ns(TICKS), opt_tick_ns(TICKS);

    long long base_total = run_ticks(TICKS, ob_init_baseline, ob_add_order_baseline,
                                      ob_market_buy_baseline, ob_market_sell_baseline, &base_tick_ns);
    long long opt_total  = run_ticks(TICKS, ob_init_opt, ob_add_order_opt,
                                      ob_market_buy_opt, ob_market_sell_opt, &opt_tick_ns);

    std::sort(base_tick_ns.begin(), base_tick_ns.end());
    std::sort(opt_tick_ns.begin(), opt_tick_ns.end());

    printf("=== Benchmark: %d ticks (synthetic market-maker + player workload) ===\n\n", TICKS);
    printf("[Full tick loop: add + match]\n");
    printf("  baseline : %lld ns total  ->  %.1f ns/tick (mean)\n", base_total, (double)base_total / TICKS);
    printf("  optimized: %lld ns total  ->  %.1f ns/tick (mean)\n", opt_total, (double)opt_total / TICKS);
    printf("  speedup  : %.2fx (mean)\n\n", (double)base_total / (double)opt_total);

    long long base_p50 = percentile(base_tick_ns, 0.50);
    long long base_p90 = percentile(base_tick_ns, 0.90);
    long long base_p99 = percentile(base_tick_ns, 0.99);
    long long base_p999 = percentile(base_tick_ns, 0.999);
    long long opt_p50  = percentile(opt_tick_ns, 0.50);
    long long opt_p90  = percentile(opt_tick_ns, 0.90);
    long long opt_p99  = percentile(opt_tick_ns, 0.99);
    long long opt_p999 = percentile(opt_tick_ns, 0.999);

    printf("[Per-tick latency distribution, ns]\n");
    printf("  %-12s %8s %8s %8s %8s %8s\n", "", "p50", "p90", "p99", "p99.9", "max");
    printf("  %-12s %8lld %8lld %8lld %8lld %8lld\n", "baseline",
           base_p50, base_p90, base_p99, base_p999, base_tick_ns.back());
    printf("  %-12s %8lld %8lld %8lld %8lld %8lld\n", "optimized",
           opt_p50, opt_p90, opt_p99, opt_p999, opt_tick_ns.back());
    printf("  speedup at p50: %.2fx, at p99: %.2fx\n\n",
           (double)base_p50 / (double)opt_p50, (double)base_p99 / (double)opt_p99);

    printf("NOTE: absolute ns figures are from this x86_64 dev machine, not the\n");
    printf("DE1-SoC RISC-V core the original main.c targets. The relative speedup\n");
    printf("is the meaningful number to carry over.\n\n");

    printf("Run with --depth-sweep (or `make depth-sweep`) for cancel-by-id /\n");
    printf("modify-qty-by-id timing across several book depths -- that's the\n");
    printf("actual algorithmic difference left between baseline and opt.\n");

    return 0;
}
