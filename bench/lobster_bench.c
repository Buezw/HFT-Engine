// ============================================================================
// lobster_bench.c
//
// Times baseline vs opt under a real LOBSTER message stream instead of
// bench/benchmark.c's synthetic workload -- same p50/p90/p99/p99.9/max
// method, real inter-arrival pattern and order sizes instead of an
// assumption. Compare against benchmark.c's numbers: if real order
// flow's tail latency looks meaningfully different, the synthetic
// benchmark was hiding something.
//
// Reuses tools/lobster_format.h for the pure message-parsing pieces (see
// tools/lobster_replay.c's header for the full Type 1-7 translation
// rationale, now much simpler post-C++-rewrite: LOBSTER prices go
// straight into the engine, no tick/window conversion needed). Doesn't
// do ground-truth validation against the paired orderbook file -- that's
// lobster_replay.c's job (`make lobster-test`, which this depends on,
// mirroring how `bench` already depends on `test`).
//
// Only the primary translated engine call is timed per message.
// ============================================================================
#define _POSIX_C_SOURCE 200809L // clock_gettime/CLOCK_MONOTONIC are POSIX, not ISO C

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "orderbook_engine.h"
#include "lobster_format.h"

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int cmp_ll(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return (x > y) - (x < y);
}

static long long percentile(const long long *sorted, int n, double p) {
    int idx = (int)(p * (n - 1));
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

static long count_lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) if (c == '\n') n++;
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <message.csv>\n", argv[0]);
        return 2;
    }

    FILE *msg_f = fopen(argv[1], "r");
    if (!msg_f) {
        fprintf(stderr, "could not open %s\n", argv[1]);
        return 2;
    }

    long capacity = count_lines(argv[1]) + 1;
    long long *base_ns = malloc(sizeof(long long) * (size_t)capacity);
    long long *opt_ns  = malloc(sizeof(long long) * (size_t)capacity);
    if (!base_ns || !opt_ns) {
        fprintf(stderr, "allocation failed for %ld samples\n", capacity);
        fclose(msg_f);
        free(base_ns);
        free(opt_ns);
        return 2;
    }
    long n_samples = 0;

    L3OrderBook *ob_base = ob_create(), *ob_opt = ob_create();
    EngineAccount acc_base, acc_opt;
    memset(&acc_base, 0, sizeof(acc_base));
    memset(&acc_opt, 0, sizeof(acc_opt));
    acc_base.my_cash = acc_opt.my_cash = INITIAL_CAPITAL;

    DroppedSet seen;
    dropped_init(&seen);

    LobMsg cur, next;
    int have_next = read_message(msg_f, &next);

    long pending_qty = 0;
    int pending_direction = 0;
    int pending_active = 0;

    while (have_next) {
        cur = next;
        have_next = read_message(msg_f, &next);
        int is_bid = (cur.direction == 1);

        switch (cur.type) {
        case 1: {
            long long t0b = now_ns();
            ob_add_order_baseline(ob_base, is_bid, (int)cur.price, (int)cur.order_id, (int)cur.size, 0);
            long long t1b = now_ns();
            long long t0o = now_ns();
            ob_add_order_opt(ob_opt, is_bid, (int)cur.price, (int)cur.order_id, (int)cur.size, 0);
            long long t1o = now_ns();
            dropped_add(&seen, (int32_t)cur.order_id);
            base_ns[n_samples] = t1b - t0b;
            opt_ns[n_samples]  = t1o - t0o;
            n_samples++;
            break;
        }
        case 2:
            if (dropped_contains(&seen, (int32_t)cur.order_id)) {
                long long t0b = now_ns();
                ob_reduce_order_qty_any_baseline(ob_base, (int)cur.order_id, (int)cur.size);
                long long t1b = now_ns();
                long long t0o = now_ns();
                ob_reduce_order_qty_any_opt(ob_opt, (int)cur.order_id, (int)cur.size);
                long long t1o = now_ns();
                base_ns[n_samples] = t1b - t0b;
                opt_ns[n_samples]  = t1o - t0o;
                n_samples++;
            }
            break;
        case 3:
            if (dropped_contains(&seen, (int32_t)cur.order_id)) {
                long long t0b = now_ns();
                ob_cancel_order_any_baseline(ob_base, (int)cur.order_id);
                long long t1b = now_ns();
                long long t0o = now_ns();
                ob_cancel_order_any_opt(ob_opt, (int)cur.order_id);
                long long t1o = now_ns();
                base_ns[n_samples] = t1b - t0b;
                opt_ns[n_samples]  = t1o - t0o;
                n_samples++;
            }
            break;
        case 4:
        case 5: {
            if (!pending_active) { pending_active = 1; pending_direction = cur.direction; pending_qty = 0; }
            pending_qty += cur.size;
            int this_is_last = !(have_next && (next.type == 4 || next.type == 5) &&
                                  next.time_ns == cur.time_ns && next.direction == cur.direction);
            if (this_is_last) {
                long long t0b = now_ns();
                if (pending_direction == 1) ob_market_sell_baseline(ob_base, &acc_base, (int)pending_qty, 0);
                else                        ob_market_buy_baseline(ob_base, &acc_base, (int)pending_qty, 0);
                long long t1b = now_ns();
                long long t0o = now_ns();
                if (pending_direction == 1) ob_market_sell_opt(ob_opt, &acc_opt, (int)pending_qty, 0);
                else                        ob_market_buy_opt(ob_opt, &acc_opt, (int)pending_qty, 0);
                long long t1o = now_ns();
                base_ns[n_samples] = t1b - t0b;
                opt_ns[n_samples]  = t1o - t0o;
                n_samples++;
                pending_active = 0;
                pending_qty = 0;
            }
            break;
        }
        default:
            break; // type 7 (halt) or unrecognized -- not a timed book event
        }
    }
    fclose(msg_f);
    ob_destroy(ob_base);
    ob_destroy(ob_opt);

    if (n_samples == 0) {
        fprintf(stderr, "no timed messages found in %s\n", argv[1]);
        free(base_ns);
        free(opt_ns);
        return 2;
    }

    long long base_total = 0, opt_total = 0;
    for (long i = 0; i < n_samples; i++) { base_total += base_ns[i]; opt_total += opt_ns[i]; }
    qsort(base_ns, (size_t)n_samples, sizeof(long long), cmp_ll);
    qsort(opt_ns,  (size_t)n_samples, sizeof(long long), cmp_ll);

    printf("---- lobster_bench: %ld timed engine calls from %s ----\n", n_samples, argv[1]);
    printf("  baseline : %lld ns total  ->  %.1f ns/call (mean)\n",
           base_total, (double)base_total / (double)n_samples);
    printf("  optimized: %lld ns total  ->  %.1f ns/call (mean)\n",
           opt_total, (double)opt_total / (double)n_samples);

    long long base_p50  = percentile(base_ns, (int)n_samples, 0.50);
    long long base_p90  = percentile(base_ns, (int)n_samples, 0.90);
    long long base_p99  = percentile(base_ns, (int)n_samples, 0.99);
    long long base_p999 = percentile(base_ns, (int)n_samples, 0.999);
    long long opt_p50   = percentile(opt_ns, (int)n_samples, 0.50);
    long long opt_p90   = percentile(opt_ns, (int)n_samples, 0.90);
    long long opt_p99   = percentile(opt_ns, (int)n_samples, 0.99);
    long long opt_p999  = percentile(opt_ns, (int)n_samples, 0.999);

    printf("  %-12s %8s %8s %8s %8s %8s\n", "", "p50", "p90", "p99", "p99.9", "max");
    printf("  %-12s %8lld %8lld %8lld %8lld %8lld\n", "baseline",
           base_p50, base_p90, base_p99, base_p999, base_ns[n_samples - 1]);
    printf("  %-12s %8lld %8lld %8lld %8lld %8lld\n", "optimized",
           opt_p50, opt_p90, opt_p99, opt_p999, opt_ns[n_samples - 1]);
    printf("  speedup at p50: %.2fx, at p99: %.2fx\n",
           (double)base_p50 / (double)opt_p50, (double)base_p99 / (double)opt_p99);
    printf("  (note: opt only wins on cancel/reduce, where its order_id index\n"
           "   actually gets used -- Type 1 submissions are pure insert cost,\n"
           "   identical logic on both sides, so don't expect a speedup there.)\n");

    free(base_ns);
    free(opt_ns);
    return 0;
}
