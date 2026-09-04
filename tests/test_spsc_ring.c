// ============================================================================
// test_spsc_ring.c
//
// Same "correctness before trusting a benchmark" discipline as
// test_correctness.c: this proves spsc_ring.h's push/pop before
// bench/threaded_bench.c's numbers mean anything, and includes a genuine
// multi-threaded stress test, not just single-threaded boundary checks.
//
// Build this specifically under ThreadSanitizer (`make tsan`), not just
// ASan: ASan catches memory-safety bugs, not data races. A lock-free
// queue's entire risk surface is data races on head/tail — TSan is the
// tool that actually exercises that, the same way `make asan` was the
// tool that actually caught the real out-of-bounds write in
// ob_add_order_opt (see README) instead of just reasoning it was fine.
// ============================================================================
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include "spsc_ring.h"

static int failures = 0;

// ============================================================================
// Test 1: basic FIFO ordering, single-threaded.
// ============================================================================
static void test_fifo_order(void) {
    SpscRing r;
    spsc_init(&r);

    for (int i = 0; i < 10; i++) {
        EngineMsg m = { MSG_MARKET_BUY, i };
        if (!spsc_push(&r, m)) {
            printf("FAIL: push %d unexpectedly failed on an empty-ish ring\n", i);
            failures++;
        }
    }

    for (int i = 0; i < 10; i++) {
        EngineMsg m;
        if (!spsc_pop(&r, &m)) {
            printf("FAIL: pop %d unexpectedly failed\n", i);
            failures++;
            continue;
        }
        if (m.qty != i) {
            printf("FAIL: FIFO order violated — expected qty=%d, got %d\n", i, m.qty);
            failures++;
        }
    }

    EngineMsg m;
    if (spsc_pop(&r, &m)) {
        printf("FAIL: pop succeeded on a ring that should be empty (got qty=%d)\n", m.qty);
        failures++;
    }

    if (failures == 0) printf("PASS: single-threaded FIFO push/pop order\n");
}

// ============================================================================
// Test 2: capacity boundary. A ring of SPSC_CAPACITY slots can only ever
// hold SPSC_CAPACITY-1 live messages at once (one slot is sacrificed so
// head==tail unambiguously means "empty" rather than being ambiguous with
// "full") — push must start failing exactly there, and popping one must
// free exactly one slot back up.
// ============================================================================
static void test_capacity_boundary(void) {
    SpscRing r;
    spsc_init(&r);

    int pushed = 0;
    while (spsc_push(&r, (EngineMsg){ MSG_MARKET_SELL, pushed })) pushed++;

    if (pushed != SPSC_CAPACITY - 1) {
        printf("FAIL: expected exactly %d successful pushes before the ring reported full, got %d\n",
               SPSC_CAPACITY - 1, pushed);
        failures++;
    }

    EngineMsg m;
    if (!spsc_pop(&r, &m) || m.qty != 0) {
        printf("FAIL: pop after filling to capacity did not return the oldest message\n");
        failures++;
    }

    if (!spsc_push(&r, (EngineMsg){ MSG_MARKET_SELL, 999 })) {
        printf("FAIL: push failed to reclaim the slot freed by the pop above\n");
        failures++;
    }

    if (failures == 0)
        printf("PASS: ring reports full at exactly SPSC_CAPACITY-1 messages, and popping frees a slot back up\n");
}

// ============================================================================
// Test 3: a real multi-threaded stress test. One producer thread pushes
// N_ITEMS sequential integers; one consumer thread pops and verifies
// they arrive in that exact order, with no value skipped or duplicated —
// the only property that actually matters for an SPSC queue, and the one
// no single-threaded test can exercise, since it requires two threads
// genuinely racing on the same head/tail fields.
// ============================================================================
#define N_ITEMS 2000000

static SpscRing g_ring;

static void *stress_producer(void *arg) {
    (void)arg;
    for (int i = 0; i < N_ITEMS; i++) {
        EngineMsg m = { MSG_MARKET_BUY, i };
        while (!spsc_push(&g_ring, m)) { /* ring full: spin-retry */ }
    }
    EngineMsg stop = { MSG_STOP, 0 };
    while (!spsc_push(&g_ring, stop)) { }
    return NULL;
}

static void test_multithreaded_stress(void) {
    spsc_init(&g_ring);

    pthread_t producer;
    pthread_create(&producer, NULL, stress_producer, NULL);

    int expected = 0;
    int local_failures = 0;
    for (;;) {
        EngineMsg m;
        while (!spsc_pop(&g_ring, &m)) { /* spin-wait */ }
        if (m.type == MSG_STOP) break;
        if (m.qty != expected) {
            if (local_failures < 5) {
                printf("FAIL: multithreaded stress — expected qty=%d, got %d (message %d)\n",
                       expected, m.qty, expected);
            }
            local_failures++;
        }
        expected++;
    }

    pthread_join(producer, NULL);

    if (expected != N_ITEMS) {
        printf("FAIL: consumer only saw %d of %d items before MSG_STOP\n", expected, N_ITEMS);
        local_failures++;
    }

    if (local_failures > 0) {
        failures += local_failures;
        return;
    }
    printf("PASS: %d items crossed the ring between two real threads, in exact order, none lost or duplicated\n", N_ITEMS);
}

int main(void) {
    test_fifo_order();
    test_capacity_boundary();
    test_multithreaded_stress();

    if (failures == 0) {
        printf("\nALL TESTS PASSED\n");
        return 0;
    }
    printf("\n%d TEST(S) FAILED\n", failures);
    return 1;
}
