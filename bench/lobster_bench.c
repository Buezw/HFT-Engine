// ============================================================================
// lobster_bench.c
//
// bench/benchmark.c's depth-sweep measures baseline-vs-opt latency under a
// deterministic SYNTHETIC workload (fixed modular-arithmetic quantities,
// uniform level selection). This measures the same thing -- ns/call,
// baseline vs opt, p50/p90/p99/p99.9/max via the identical qsort+
// percentile() method -- but driven by a REAL LOBSTER message stream, so
// the inter-arrival pattern, order sizes, and which side/level gets hit
// all come from actual market activity instead of an assumption. Compare
// the resulting table against README's synthetic depth-sweep numbers: if
// real order flow's tail latency looks meaningfully different, the
// synthetic benchmark was hiding something.
//
// Reuses tools/lobster_format.h for the pure message-parsing pieces (same
// LOBSTER Type 1/2/3/4/5/7 translation tools/lobster_replay.c uses -- see
// its header comment for the full rationale) but does NOT do ground-truth
// validation against the paired orderbook file -- that correctness
// question is lobster_replay.c's job (see `make lobster-test`, which
// `lobster-bench` depends on, mirroring how `bench` already depends on
// `test`: "correctness first, always").
//
// Only the primary translated engine call is timed per message (the
// add/cancel/reduce/market_buy-sell that message causes); ghost
// compaction/total_qty bookkeeping between messages is real, necessary
// upkeep (same cadence tools/lobster_replay.c uses, to avoid artificial
// queue-full drops) but untimed housekeeping, not what's being measured --
// same separation bench/benchmark.c already draws for clean_ghosts.
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

// samples must already be sorted ascending -- identical to bench/benchmark.c's.
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
        fprintf(stderr, "usage: %s <message.csv> [tick_size=100]\n", argv[0]);
        return 2;
    }
    long tick_size = (argc > 2) ? strtol(argv[2], NULL, 10) : 100;

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

    L3OrderBook ob_base, ob_opt;
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(&ob_base, &acc_base, 1);
    ob_init_opt(&ob_opt, &acc_opt, 1);
    memset(&ob_base, 0, sizeof(ob_base));
    memset(&ob_opt, 0, sizeof(ob_opt));
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        ob_base.bids[i].price = -i;
        ob_base.asks[i].price = 2 + i;
        ob_opt.bids[i].price  = ob_base.bids[i].price;
        ob_opt.asks[i].price  = ob_base.asks[i].price;
    }

    DroppedSet dropped;
    dropped_init(&dropped);

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
            long best = (is_bid ? ob_opt.bids[0].price : ob_opt.asks[0].price) * tick_size;
            long ticks_from_best = is_bid ? (best - cur.price) / tick_size
                                           : (cur.price - best) / tick_size;
            long long t0b, t0o;
            int level;
            if (ticks_from_best < 0) {
                long delta_ticks = -ticks_from_best;
                int delta = (int)(is_bid ? delta_ticks : -delta_ticks);
                t0b = now_ns();
                int ok_b = ob_drift_price_baseline(&ob_base, delta);
                long long t1b = now_ns();
                t0o = now_ns();
                int ok_o = ob_drift_price_opt(&ob_opt, delta);
                long long t1o = now_ns();
                if (!ok_b || !ok_o) { dropped_add(&dropped, (int32_t)cur.order_id); break; }
                base_ns[n_samples] = t1b - t0b;
                opt_ns[n_samples]  = t1o - t0o;
                n_samples++;
                level = 0;
            } else if (ticks_from_best >= MAX_PRICE_LEVELS) {
                dropped_add(&dropped, (int32_t)cur.order_id);
                break;
            } else {
                level = (int)ticks_from_best;
            }
            L3Order o = {(int)cur.order_id, (int)cur.size, 0, /*is_mine=*/0, 0};
            L3PriceLevel *lb = is_bid ? &ob_base.bids[level] : &ob_base.asks[level];
            L3PriceLevel *lo = is_bid ? &ob_opt.bids[level]  : &ob_opt.asks[level];
            t0b = now_ns();
            int ok_b = ob_add_order_baseline(lb, o);
            long long t1b = now_ns();
            t0o = now_ns();
            int ok_o = ob_add_order_opt(lo, o);
            long long t1o = now_ns();
            if (!ok_b || !ok_o) dropped_add(&dropped, (int32_t)cur.order_id);
            base_ns[n_samples] = t1b - t0b;
            opt_ns[n_samples]  = t1o - t0o;
            n_samples++;
            ob_update_total_qty_baseline(&ob_base);
            break;
        }
        case 2:
            if (!dropped_contains(&dropped, (int32_t)cur.order_id)) {
                long long t0b = now_ns();
                ob_reduce_order_qty_any_baseline(&ob_base, (int)cur.order_id, (int)cur.size);
                long long t1b = now_ns();
                long long t0o = now_ns();
                ob_reduce_order_qty_any_opt(&ob_opt, (int)cur.order_id, (int)cur.size);
                long long t1o = now_ns();
                base_ns[n_samples] = t1b - t0b;
                opt_ns[n_samples]  = t1o - t0o;
                n_samples++;
            }
            break;
        case 3:
            if (!dropped_contains(&dropped, (int32_t)cur.order_id)) {
                long long t0b = now_ns();
                ob_cancel_order_any_baseline(&ob_base, (int)cur.order_id);
                long long t1b = now_ns();
                long long t0o = now_ns();
                ob_cancel_order_any_opt(&ob_opt, (int)cur.order_id);
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
                if (pending_direction == 1) ob_market_sell_baseline(&ob_base, &acc_base, (int)pending_qty, 0);
                else                        ob_market_buy_baseline(&ob_base, &acc_base, (int)pending_qty, 0);
                long long t1b = now_ns();
                long long t0o = now_ns();
                if (pending_direction == 1) ob_market_sell_opt(&ob_opt, &acc_opt, (int)pending_qty, 0);
                else                        ob_market_buy_opt(&ob_opt, &acc_opt, (int)pending_qty, 0);
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

        for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
            ob_clean_ghosts_baseline(&ob_base.bids[i]);
            ob_clean_ghosts_opt(&ob_opt.bids[i]);
            ob_clean_ghosts_baseline(&ob_base.asks[i]);
            ob_clean_ghosts_opt(&ob_opt.asks[i]);
        }
        ob_update_total_qty_baseline(&ob_base);
    }
    fclose(msg_f);

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

    // n_samples counts timed engine calls, not raw message rows: a Type 1
    // that improves the best contributes two (the drift, then the add),
    // a multi-row Type 4/5 sweep contributes one (only the flush is
    // timed). That's intentional -- this characterizes the real cost of
    // processing one real market event, not any single primitive in
    // isolation (bench/benchmark.c's per-operation microbenchmarks
    // already do that).
    printf("---- lobster_bench: %ld timed engine calls from %s (MAX_PRICE_LEVELS=%d, MAX_ORDERS_PER_LVL=%d) ----\n",
           n_samples, argv[1], MAX_PRICE_LEVELS, MAX_ORDERS_PER_LVL);
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
    printf("  (compare against README's synthetic depth-sweep table at the\n"
           "   matching MAX_ORDERS_PER_LVL -- a materially different p99/p99.9\n"
           "   here means the synthetic workload was hiding real tail behavior.)\n");

    free(base_ns);
    free(opt_ns);
    return 0;
}
