#include "spsc_ring.h"

void spsc_init(SpscRing *r) {
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
}

int spsc_push(SpscRing *r, EngineMsg msg) {
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t next = (head + 1) % SPSC_CAPACITY;
    // acquire: must see the consumer's latest tail before deciding "full",
    // otherwise we could report full while a slot the consumer already
    // freed is actually available.
    if (next == atomic_load_explicit(&r->tail, memory_order_acquire)) {
        return 0; // full
    }
    r->buf[head] = msg;
    // release: publishes the write to buf[head] before the new head
    // becomes visible, so the consumer never reads a slot before its
    // contents are actually written.
    atomic_store_explicit(&r->head, next, memory_order_release);
    return 1;
}

int spsc_pop(SpscRing *r, EngineMsg *out) {
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    // acquire: must see the producer's latest head, and (paired with the
    // producer's release above) everything it wrote to buf before that.
    if (tail == atomic_load_explicit(&r->head, memory_order_acquire)) {
        return 0; // empty
    }
    *out = r->buf[tail];
    atomic_store_explicit(&r->tail, (tail + 1) % SPSC_CAPACITY, memory_order_release);
    return 1;
}
