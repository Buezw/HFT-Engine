// ============================================================================
// lobster_bench.cpp
//
// Times baseline vs opt under a real LOBSTER message stream instead of
// bench/benchmark.cpp's synthetic workload -- same p50/p90/p99/p99.9/max
// method, real inter-arrival pattern and order sizes instead of an
// assumption. Compare against benchmark.cpp's numbers: if real order
// flow's tail latency looks meaningfully different, the synthetic
// benchmark was hiding something.
//
// Reuses tools/lobster_format.h for the pure message-parsing pieces (see
// tools/lobster_replay.cpp's header for the full Type 1-7 translation
// rationale, now much simpler post-C++-rewrite: LOBSTER prices go
// straight into the engine, no tick/window conversion needed). Doesn't
// do ground-truth validation against the paired orderbook file -- that's
// lobster_replay.cpp's job (`make lobster-test`, which this depends on,
// mirroring how `bench` already depends on `test`).
//
// Only the primary translated engine call is timed per message. Type 4
// is a reduce-by-id like Type 2, Type 5 (hidden) isn't timed at all.
// ============================================================================
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include "orderbook_engine.h"
#include "lobster_format.h"

static long long now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

static long long percentile(const std::vector<long long> &sorted, double p) {
    int n = static_cast<int>(sorted.size());
    int idx = static_cast<int>(p * (n - 1));
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

static long count_lines(const char *path) {
    std::ifstream f(path, std::ios::binary);
    return static_cast<long>(std::count(std::istreambuf_iterator<char>(f),
                                         std::istreambuf_iterator<char>(), '\n'));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <message.csv>\n", argv[0]);
        return 2;
    }

    std::ifstream msg_f(argv[1]);
    if (!msg_f) {
        fprintf(stderr, "could not open %s\n", argv[1]);
        return 2;
    }

    // Reserved up front (one sample per line, at most) so no reallocation
    // ever lands between two timed calls.
    long capacity = count_lines(argv[1]) + 1;
    std::vector<long long> base_ns, opt_ns;
    base_ns.reserve(static_cast<size_t>(capacity));
    opt_ns.reserve(static_cast<size_t>(capacity));

    L3OrderBook *ob_base = ob_create(), *ob_opt = ob_create();

    OrderIdSet seen;

    LobMsg cur;

    while (read_message(msg_f, cur)) {
        int is_bid = (cur.direction == 1);

        switch (cur.type) {
        case 1: {
            long long t0b = now_ns();
            ob_add_order_baseline(ob_base, is_bid, (int)cur.price, (int)cur.order_id, (int)cur.size, 0);
            long long t1b = now_ns();
            long long t0o = now_ns();
            ob_add_order_opt(ob_opt, is_bid, (int)cur.price, (int)cur.order_id, (int)cur.size, 0);
            long long t1o = now_ns();
            seen.insert(static_cast<int32_t>(cur.order_id));
            base_ns.push_back(t1b - t0b);
            opt_ns.push_back(t1o - t0o);
            break;
        }
        case 2:
        case 4: // execution of a known resting order -- same removal as a partial cancel
            if (seen.count(static_cast<int32_t>(cur.order_id))) {
                long long t0b = now_ns();
                ob_reduce_order_qty_any_baseline(ob_base, (int)cur.order_id, (int)cur.size);
                long long t1b = now_ns();
                long long t0o = now_ns();
                ob_reduce_order_qty_any_opt(ob_opt, (int)cur.order_id, (int)cur.size);
                long long t1o = now_ns();
                base_ns.push_back(t1b - t0b);
                opt_ns.push_back(t1o - t0o);
            }
            break;
        case 3:
            if (seen.count(static_cast<int32_t>(cur.order_id))) {
                long long t0b = now_ns();
                ob_cancel_order_any_baseline(ob_base, (int)cur.order_id);
                long long t1b = now_ns();
                long long t0o = now_ns();
                ob_cancel_order_any_opt(ob_opt, (int)cur.order_id);
                long long t1o = now_ns();
                base_ns.push_back(t1b - t0b);
                opt_ns.push_back(t1o - t0o);
            }
            break;
        default:
            break; // type 5 (hidden exec), 7 (halt) or unrecognized -- not a timed book event
        }
    }
    ob_destroy(ob_base);
    ob_destroy(ob_opt);

    long n_samples = static_cast<long>(base_ns.size());
    if (n_samples == 0) {
        fprintf(stderr, "no timed messages found in %s\n", argv[1]);
        return 2;
    }

    long long base_total = 0, opt_total = 0;
    for (long i = 0; i < n_samples; i++) { base_total += base_ns[i]; opt_total += opt_ns[i]; }
    std::sort(base_ns.begin(), base_ns.end());
    std::sort(opt_ns.begin(), opt_ns.end());

    printf("---- lobster_bench: %ld timed engine calls from %s ----\n", n_samples, argv[1]);
    printf("  baseline : %lld ns total  ->  %.1f ns/call (mean)\n",
           base_total, (double)base_total / (double)n_samples);
    printf("  optimized: %lld ns total  ->  %.1f ns/call (mean)\n",
           opt_total, (double)opt_total / (double)n_samples);

    long long base_p50  = percentile(base_ns, 0.50);
    long long base_p90  = percentile(base_ns, 0.90);
    long long base_p99  = percentile(base_ns, 0.99);
    long long base_p999 = percentile(base_ns, 0.999);
    long long opt_p50   = percentile(opt_ns, 0.50);
    long long opt_p90   = percentile(opt_ns, 0.90);
    long long opt_p99   = percentile(opt_ns, 0.99);
    long long opt_p999  = percentile(opt_ns, 0.999);

    printf("  %-12s %8s %8s %8s %8s %8s\n", "", "p50", "p90", "p99", "p99.9", "max");
    printf("  %-12s %8lld %8lld %8lld %8lld %8lld\n", "baseline",
           base_p50, base_p90, base_p99, base_p999, base_ns.back());
    printf("  %-12s %8lld %8lld %8lld %8lld %8lld\n", "optimized",
           opt_p50, opt_p90, opt_p99, opt_p999, opt_ns.back());
    printf("  speedup at p50: %.2fx, at p99: %.2fx\n",
           (double)base_p50 / (double)opt_p50, (double)base_p99 / (double)opt_p99);
    printf("  (note: opt only wins on cancel/reduce/exec, where its order_id index\n"
           "   actually gets used -- Type 1 submissions are pure insert cost,\n"
           "   identical logic on both sides, so don't expect a speedup there.)\n");
    return 0;
}
