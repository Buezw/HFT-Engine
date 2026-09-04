// ============================================================================
// spsc_ring.h
//
// A single-producer/single-consumer lock-free ring buffer, used to hand
// EngineMsg values from a "receiver" thread to a "matching" thread (see
// bench/threaded_bench.c) without either side ever blocking on a mutex.
//
// Why this is safe without locks: head is written by exactly one thread
// (the producer) and only ever read by the other; tail is written by
// exactly one thread (the consumer) and only ever read by the other. That
// single-writer-per-field invariant is what makes a lock-free SPSC ring
// buffer correct — it would NOT be safe with more than one producer or
// more than one consumer without additional synchronization.
//
// Deliberately NOT wired into the matching engine itself
// (orderbook_engine.c/.h) — the engine's own matching logic
// (ob_market_*_opt etc.) stays exactly as it is, single-threaded, called
// from whichever one thread happens to be draining this queue. Real
// matching engines work the same way: the book's state machine is
// processed by one thread; concurrency comes from decoupling I/O
// (parsing/receiving orders, which has real, unpredictable latency) from
// that single matching thread, not from parallelizing the book itself
// (see engine/README.md's "Threaded ingestion" section for why
// parallelizing a single book's matching logic directly would only add
// lock overhead with nothing to actually run in parallel).
// ============================================================================
#ifndef SPSC_RING_H
#define SPSC_RING_H

#include <stdatomic.h>
#include <stddef.h>

#define SPSC_CAPACITY 4096

typedef enum {
    MSG_MARKET_BUY,
    MSG_MARKET_SELL,
    MSG_STOP // poison pill: tells the consumer thread to exit its loop
} EngineMsgType;

typedef struct {
    EngineMsgType type;
    int qty;
} EngineMsg;

typedef struct {
    EngineMsg buf[SPSC_CAPACITY];
    _Atomic size_t head; // next slot the producer will write
    _Atomic size_t tail; // next slot the consumer will read
} SpscRing;

void spsc_init(SpscRing *r);

// Returns 1 if msg was enqueued, 0 if the ring is full (caller's choice
// whether to retry/spin or drop — bench/threaded_bench.c retries).
int spsc_push(SpscRing *r, EngineMsg msg);

// Returns 1 and fills *out if a message was available, 0 if the ring is
// currently empty (caller's choice whether to spin — the matching thread
// in bench/threaded_bench.c does, deliberately: a blocking wait would
// mean a syscall in the hot path, which is exactly the kind of latency
// this whole design exists to avoid).
int spsc_pop(SpscRing *r, EngineMsg *out);

#endif // SPSC_RING_H
