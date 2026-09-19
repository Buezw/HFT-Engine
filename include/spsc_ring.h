// ============================================================================
// spsc_ring.h
//
// A single-producer/single-consumer lock-free ring buffer, used to hand
// EngineMsg values from a "receiver" thread to a "matching" thread (see
// bench/threaded_bench.cpp) without either side ever blocking on a mutex.
//
// Why this is safe without locks: head is written by exactly one thread
// (the producer) and only ever read by the other; tail is written by
// exactly one thread (the consumer) and only ever read by the other. That
// single-writer-per-field invariant is what makes a lock-free SPSC ring
// buffer correct — it would NOT be safe with more than one producer or
// more than one consumer without additional synchronization.
//
// Deliberately NOT wired into the matching engine itself
// (orderbook_engine.cpp/.h) — the engine's own matching logic
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

#include <atomic>
#include <cstddef>

constexpr std::size_t SPSC_CAPACITY = 4096;

enum EngineMsgType {
    MSG_MARKET_BUY,
    MSG_MARKET_SELL,
    MSG_STOP // poison pill: tells the consumer thread to exit its loop
};

struct EngineMsg {
    EngineMsgType type;
    int qty;
};

class SpscRing {
public:
    // Returns true if msg was enqueued, false if the ring is full (caller's
    // choice whether to retry/spin or drop — bench/threaded_bench.cpp retries).
    bool push(const EngineMsg &msg) {
        std::size_t head = head_.load(std::memory_order_relaxed);
        std::size_t next = (head + 1) % SPSC_CAPACITY;
        // acquire: must see the consumer's latest tail before deciding "full",
        // otherwise we could report full while a slot the consumer already
        // freed is actually available.
        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // full
        }
        buf_[head] = msg;
        // release: publishes the write to buf_[head] before the new head
        // becomes visible, so the consumer never reads a slot before its
        // contents are actually written.
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Returns true and fills out if a message was available, false if the
    // ring is currently empty (caller's choice whether to spin — the
    // matching thread in bench/threaded_bench.cpp does, deliberately: a
    // blocking wait would mean a syscall in the hot path, which is exactly
    // the kind of latency this whole design exists to avoid).
    bool pop(EngineMsg &out) {
        std::size_t tail = tail_.load(std::memory_order_relaxed);
        // acquire: must see the producer's latest head, and (paired with the
        // producer's release above) everything it wrote to buf_ before that.
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        out = buf_[tail];
        tail_.store((tail + 1) % SPSC_CAPACITY, std::memory_order_release);
        return true;
    }

private:
    EngineMsg buf_[SPSC_CAPACITY]{};
    std::atomic<std::size_t> head_{0}; // next slot the producer will write
    std::atomic<std::size_t> tail_{0}; // next slot the consumer will read
};

#endif // SPSC_RING_H
