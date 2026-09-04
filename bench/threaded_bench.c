// ============================================================================
// threaded_bench.c
//
// Tests a real claim about splitting "receiving orders" from "matching
// them" onto two threads (see include/spsc_ring.h for why the matching
// logic itself stays single-threaded either way): does decoupling actually
// protect the matching thread from jitter on the receive side, or is it
// just complexity for nothing?
//
// simulate_receive_jitter() stands in for the unpredictable cost of really
// receiving a message (socket read, packet reassembly, parsing) — a busy
// spin of randomized length, not nanosleep, so the simulated cost is
// CPU-bound and reproducible instead of at the mercy of OS scheduler
// granularity.
//
//   - run_baseline(): one thread does jitter, then immediately matches,
//     back to back. The gap between consecutive matching calls is jitter +
//     match time, every single time — nothing hides it.
//   - run_threaded(): a producer thread does jitter + pushes into an
//     spsc_ring; the matching thread does nothing but pop-and-match in a
//     tight spin loop. As long as the producer keeps a little backlog
//     queued on average, the matching thread's own inter-call gaps stop
//     tracking the producer's jitter directly.
//
// What this does NOT claim: decoupling doesn't make any single message
// arrive-to-matched faster — the jitter still has to happen somewhere on
// that message's path. What it protects is the MATCHING THREAD's own
// timeline: in a real system there's usually one matching thread and many
// noisy I/O sources feeding it, so keeping that one thread's execution
// smooth and predictable is the actual point, not shaving total latency
// for any individual message.
// ============================================================================
// _GNU_SOURCE (superset of _POSIX_C_SOURCE) is needed for
// pthread_setaffinity_np — a glibc extension, not POSIX — used below to
// pin each thread to its own core and test whether OS scheduler
// contention, not the queue design itself, explains the tail-latency
// result.
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include "orderbook_engine.h"
#include "spsc_ring.h"

static void pin_to_core(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    // Best-effort: if this fails (e.g. a cgroup that doesn't expose `core`),
    // the OS scheduler is free to move the thread — that's a legitimate
    // outcome to fall back to, not treated as a fatal error here.
}

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

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

static void simulate_receive_jitter(uint32_t *rng) {
    int spin = (int)(xorshift32(rng) % 2000);
    volatile long sink = 0;
    for (int i = 0; i < spin; i++) sink += i;
}

#define N_MESSAGES 20000

static void clean_all(L3OrderBook *ob) {
    for (int lvl = 0; lvl < MAX_PRICE_LEVELS; lvl++) {
        ob_clean_ghosts_opt(&ob->bids[lvl]);
        ob_clean_ghosts_opt(&ob->asks[lvl]);
    }
}

// ---------------------------------------------------------------------
// Baseline: jitter and matching serialized on one thread.
// ---------------------------------------------------------------------
static void run_baseline(long long *gaps_out) {
    L3OrderBook ob;
    EngineAccount acc;
    ob_init_opt(&ob, &acc, 100);
    uint32_t rng = 0xB16B00B5u;

    long long prev = now_ns();
    for (int i = 0; i < N_MESSAGES; i++) {
        simulate_receive_jitter(&rng);
        int qty = 5 + (int)(xorshift32(&rng) % 20);
        int is_buy = xorshift32(&rng) & 1;

        long long t0 = now_ns();
        if (is_buy) ob_market_buy_opt(&ob, &acc, qty, 0);
        else ob_market_sell_opt(&ob, &acc, qty, 0);
        clean_all(&ob);

        gaps_out[i] = t0 - prev;
        prev = t0;
    }
}

// ---------------------------------------------------------------------
// Threaded: producer does jitter+push; matching thread pop-and-matches.
// ---------------------------------------------------------------------
static SpscRing g_ring;

static int g_producer_core = -1; // -1 = don't pin, let the OS scheduler decide

static void *producer_thread(void *arg) {
    (void)arg;
    if (g_producer_core >= 0) pin_to_core(g_producer_core);
    uint32_t rng = 0xB16B00B5u; // same seed/sequence as baseline, on purpose — same workload, different architecture
    for (int i = 0; i < N_MESSAGES; i++) {
        simulate_receive_jitter(&rng);
        EngineMsg m;
        m.qty = 5 + (int)(xorshift32(&rng) % 20);
        m.type = (xorshift32(&rng) & 1) ? MSG_MARKET_BUY : MSG_MARKET_SELL;
        while (!spsc_push(&g_ring, m)) { /* ring full: spin-retry */ }
    }
    EngineMsg stop = { MSG_STOP, 0 };
    while (!spsc_push(&g_ring, stop)) { }
    return NULL;
}

// matcher_core/producer_core: -1 means "don't pin, let the OS scheduler
// decide" (the g_producer_core global is the only way to hand the core
// number into producer_thread's pthread_create signature without changing
// it — fine here since only one producer ever runs at a time).
static int run_threaded(long long *gaps_out, int matcher_core, int producer_core) {
    L3OrderBook ob;
    EngineAccount acc;
    ob_init_opt(&ob, &acc, 100);
    spsc_init(&g_ring);

    if (matcher_core >= 0) pin_to_core(matcher_core);
    g_producer_core = producer_core;

    pthread_t producer;
    pthread_create(&producer, NULL, producer_thread, NULL);

    long long prev = now_ns();
    int count = 0;
    for (;;) {
        EngineMsg m;
        while (!spsc_pop(&g_ring, &m)) { /* spin-wait for the producer */ }
        if (m.type == MSG_STOP) break;

        long long t0 = now_ns();
        if (m.type == MSG_MARKET_BUY) ob_market_buy_opt(&ob, &acc, m.qty, 0);
        else ob_market_sell_opt(&ob, &acc, m.qty, 0);
        clean_all(&ob);

        gaps_out[count++] = t0 - prev;
        prev = t0;
    }

    pthread_join(producer, NULL);
    return count;
}

static void report(const char *label, long long *gaps, int n, long long total_ns) {
    qsort(gaps, (size_t)n, sizeof(long long), cmp_ll);
    printf("%-10s  n=%-6d  mean=%6lldns  p50=%6lldns  p90=%6lldns  p99=%7lldns  max=%8lldns  total=%.3fms\n",
           label, n,
           total_ns / n,
           percentile(gaps, n, 0.50),
           percentile(gaps, n, 0.90),
           percentile(gaps, n, 0.99),
           gaps[n - 1],
           total_ns / 1e6);
}

int main(void) {
    long long *baseline_gaps = malloc(sizeof(long long) * N_MESSAGES);
    long long *unpinned_gaps = malloc(sizeof(long long) * N_MESSAGES);
    long long *pinned_gaps   = malloc(sizeof(long long) * N_MESSAGES);
    if (!baseline_gaps || !unpinned_gaps || !pinned_gaps) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    long long t0 = now_ns();
    run_baseline(baseline_gaps);
    long long baseline_total = now_ns() - t0;

    long long t1 = now_ns();
    int unpinned_n = run_threaded(unpinned_gaps, /*matcher_core=*/-1, /*producer_core=*/-1);
    long long unpinned_total = now_ns() - t1;

    long long t2 = now_ns();
    int pinned_n = run_threaded(pinned_gaps, /*matcher_core=*/0, /*producer_core=*/1);
    long long pinned_total = now_ns() - t2;

    printf("[Matching-thread inter-call gap: single-threaded vs receiver+matcher split]\n");
    printf("(gap = wall-clock time between the start of one matching call and the start of the next;\n");
    printf(" in the single-threaded case that gap IS the simulated receive jitter + prior match time)\n\n");
    report("baseline", baseline_gaps, N_MESSAGES, baseline_total);
    report("unpinned", unpinned_gaps, unpinned_n, unpinned_total);
    report("pinned",   pinned_gaps,   pinned_n,   pinned_total);

    printf("\n\"pinned\" forces the matching thread onto core 0 and the producer onto core 1\n");
    printf("(pthread_setaffinity_np) — isolating whether the OS scheduler bouncing both\n");
    printf("threads across cores/hyperthreads explains \"unpinned\"'s result, vs. the queue\n");
    printf("design itself.\n");
    printf("\nRun-to-run variance on a shared, non-realtime dev machine is real (same caveat as\n");
    printf("bench/benchmark.c) — rerun a few times before trusting any single number here.\n");

    free(baseline_gaps);
    free(unpinned_gaps);
    free(pinned_gaps);
    return 0;
}
