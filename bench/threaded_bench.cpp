// ============================================================================
// threaded_bench.cpp
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
// pthread_setaffinity_np is a glibc extension, not POSIX or std::thread
// -- used below to pin each thread to its own core and test whether OS
// scheduler contention, not the queue design itself, explains the
// tail-latency result. (g++/clang++ define _GNU_SOURCE by default on
// Linux, which is what exposes it.)
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sched.h>
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

// steady_clock is CLOCK_MONOTONIC on Linux -- same clock as before.
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

static uint32_t xorshift32(uint32_t &state) {
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return state = x;
}

static void simulate_receive_jitter(uint32_t &rng) {
    int spin = static_cast<int>(xorshift32(rng) % 2000);
    volatile long sink = 0;
    for (int i = 0; i < spin; i++) sink += i;
}

constexpr int N_MESSAGES = 20000;

// ---------------------------------------------------------------------
// Baseline: jitter and matching serialized on one thread.
// ---------------------------------------------------------------------
static std::vector<long long> run_baseline() {
    std::vector<long long> gaps(N_MESSAGES);
    L3OrderBook *ob = ob_create();
    EngineAccount acc;
    ob_init_opt(ob, &acc, 100);
    uint32_t rng = 0xB16B00B5u;

    long long prev = now_ns();
    for (int i = 0; i < N_MESSAGES; i++) {
        simulate_receive_jitter(rng);
        int qty = 5 + static_cast<int>(xorshift32(rng) % 20);
        int is_buy = xorshift32(rng) & 1;

        long long t0 = now_ns();
        if (is_buy) ob_market_buy_opt(ob, &acc, qty, 0);
        else ob_market_sell_opt(ob, &acc, qty, 0);

        gaps[i] = t0 - prev;
        prev = t0;
    }
    ob_destroy(ob);
    return gaps;
}

// ---------------------------------------------------------------------
// Threaded: producer does jitter+push; matching thread pop-and-matches.
// ---------------------------------------------------------------------
static void producer_thread(SpscRing &ring, int core) {
    if (core >= 0) pin_to_core(core);
    uint32_t rng = 0xB16B00B5u; // same seed/sequence as baseline, on purpose — same workload, different architecture
    for (int i = 0; i < N_MESSAGES; i++) {
        simulate_receive_jitter(rng);
        EngineMsg m;
        m.qty = 5 + static_cast<int>(xorshift32(rng) % 20);
        m.type = (xorshift32(rng) & 1) ? MSG_MARKET_BUY : MSG_MARKET_SELL;
        while (!ring.push(m)) { /* ring full: spin-retry */ }
    }
    EngineMsg stop = { MSG_STOP, 0 };
    while (!ring.push(stop)) { }
}

// matcher_core/producer_core: -1 means "don't pin, let the OS scheduler
// decide".
static std::vector<long long> run_threaded(int matcher_core, int producer_core) {
    std::vector<long long> gaps;
    gaps.reserve(N_MESSAGES);
    L3OrderBook *ob = ob_create();
    EngineAccount acc;
    ob_init_opt(ob, &acc, 100);
    // 32 KiB of message slots -- heap, not stack.
    auto ring = std::make_unique<SpscRing>();

    if (matcher_core >= 0) pin_to_core(matcher_core);

    std::thread producer(producer_thread, std::ref(*ring), producer_core);

    long long prev = now_ns();
    for (;;) {
        EngineMsg m;
        while (!ring->pop(m)) { /* spin-wait for the producer */ }
        if (m.type == MSG_STOP) break;

        long long t0 = now_ns();
        if (m.type == MSG_MARKET_BUY) ob_market_buy_opt(ob, &acc, m.qty, 0);
        else ob_market_sell_opt(ob, &acc, m.qty, 0);

        gaps.push_back(t0 - prev);
        prev = t0;
    }

    producer.join();
    ob_destroy(ob);
    return gaps;
}

static void report(const char *label, std::vector<long long> &gaps, long long total_ns) {
    std::sort(gaps.begin(), gaps.end());
    int n = static_cast<int>(gaps.size());
    printf("%-10s  n=%-6d  mean=%6lldns  p50=%6lldns  p90=%6lldns  p99=%7lldns  max=%8lldns  total=%.3fms\n",
           label, n,
           total_ns / n,
           percentile(gaps, 0.50),
           percentile(gaps, 0.90),
           percentile(gaps, 0.99),
           gaps.back(),
           total_ns / 1e6);
}

int main() {
    long long t0 = now_ns();
    std::vector<long long> baseline_gaps = run_baseline();
    long long baseline_total = now_ns() - t0;

    long long t1 = now_ns();
    std::vector<long long> unpinned_gaps = run_threaded(/*matcher_core=*/-1, /*producer_core=*/-1);
    long long unpinned_total = now_ns() - t1;

    long long t2 = now_ns();
    std::vector<long long> pinned_gaps = run_threaded(/*matcher_core=*/0, /*producer_core=*/1);
    long long pinned_total = now_ns() - t2;

    printf("[Matching-thread inter-call gap: single-threaded vs receiver+matcher split]\n");
    printf("(gap = wall-clock time between the start of one matching call and the start of the next;\n");
    printf(" in the single-threaded case that gap IS the simulated receive jitter + prior match time)\n\n");
    report("baseline", baseline_gaps, baseline_total);
    report("unpinned", unpinned_gaps, unpinned_total);
    report("pinned",   pinned_gaps,   pinned_total);

    printf("\n\"pinned\" forces the matching thread onto core 0 and the producer onto core 1\n");
    printf("(pthread_setaffinity_np) — isolating whether the OS scheduler bouncing both\n");
    printf("threads across cores/hyperthreads explains \"unpinned\"'s result, vs. the queue\n");
    printf("design itself.\n");
    printf("\nRun-to-run variance on a shared, non-realtime dev machine is real (same caveat as\n");
    printf("bench/benchmark.cpp) — rerun a few times before trusting any single number here.\n");
    return 0;
}
