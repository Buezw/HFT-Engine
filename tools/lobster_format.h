// ============================================================================
// lobster_format.h
//
// Pure LOBSTER message-file decoding, shared between tools/lobster_replay.c
// (ground-truth validation) and bench/lobster_bench.c (real-data latency
// benchmark) — both need to parse the same message stream and skip the
// same dropped ids, but time/compare it differently, so only the format
// decoding itself (no policy) lives here. See lobster_replay.c's header
// comment for the column format this parses.
// ============================================================================
#ifndef LOBSTER_FORMAT_H
#define LOBSTER_FORMAT_H

#include <stdio.h>
#include <stdint.h>

#define DROPPED_SET_CAP 65536 // ids this tool chose not to represent in the book

// Small fixed-capacity id set (no malloc, same discipline as the rest of
// this codebase) — remembers ids deliberately not placed (out-of-window or
// queue-full), so later Type 2/3/4/5 references to them are silently
// ignored instead of being misreported as lookup failures.
typedef struct { int32_t ids[DROPPED_SET_CAP]; } DroppedSet;

static inline void dropped_init(DroppedSet *s) {
    for (int i = 0; i < DROPPED_SET_CAP; i++) s->ids[i] = -1;
}
static inline uint32_t dropped_hash(int32_t id) { return (uint32_t)id * 2654435761u; }
static inline void dropped_add(DroppedSet *s, int32_t id) {
    uint32_t h = dropped_hash(id) & (DROPPED_SET_CAP - 1);
    for (int i = 0; i < DROPPED_SET_CAP; i++) {
        uint32_t idx = (h + (uint32_t)i) & (DROPPED_SET_CAP - 1);
        if (s->ids[idx] == -1 || s->ids[idx] == id) { s->ids[idx] = id; return; }
    }
}
static inline int dropped_contains(const DroppedSet *s, int32_t id) {
    uint32_t h = dropped_hash(id) & (DROPPED_SET_CAP - 1);
    for (int i = 0; i < DROPPED_SET_CAP; i++) {
        uint32_t idx = (h + (uint32_t)i) & (DROPPED_SET_CAP - 1);
        if (s->ids[idx] == -1) return 0;
        if (s->ids[idx] == id) return 1;
    }
    return 0;
}

typedef struct {
    long long time_ns; // seconds-after-midnight * 1e9, rounded -- exact-integer grouping key
    int  type;
    long order_id;
    long size;
    long price;     // LOBSTER units: dollars * 10000
    int  direction; // 1 = affected order is a bid, -1 = affected order is an ask
    int  valid;
} LobMsg;

static inline int read_message(FILE *f, LobMsg *m) {
    char line[256];
    if (!fgets(line, sizeof(line), f)) { m->valid = 0; return 0; }
    double t;
    int type, direction;
    long order_id, size, price;
    if (sscanf(line, "%lf,%d,%ld,%ld,%ld,%d", &t, &type, &order_id, &size, &price, &direction) != 6) {
        m->valid = 0;
        return 0;
    }
    m->time_ns = (long long)(t * 1e9 + (t >= 0 ? 0.5 : -0.5));
    m->type = type;
    m->order_id = order_id;
    m->size = size;
    m->price = price;
    m->direction = direction;
    m->valid = 1;
    return 1;
}

#endif // LOBSTER_FORMAT_H
