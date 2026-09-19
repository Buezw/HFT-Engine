// ============================================================================
// lobster_format.h
//
// Pure LOBSTER message-file decoding, shared between tools/lobster_replay.cpp
// (ground-truth validation) and bench/lobster_bench.cpp (real-data latency
// benchmark) — both need to parse the same message stream and skip the
// same dropped ids, but time/compare it differently, so only the format
// decoding itself (no policy) lives here. See lobster_replay.cpp's header
// comment for the column format this parses.
// ============================================================================
#ifndef LOBSTER_FORMAT_H
#define LOBSTER_FORMAT_H

#include <cstdint>
#include <cstdio>
#include <istream>
#include <string>
#include <unordered_set>

// Order-id set. Despite being used as "seen" and "pruned", in practice
// `seen` tracks every id a Type 1 has placed in *this* replay, so later
// Type 2/3/4/5 references can tell a known id from a genuinely unknown
// one (see lobster_replay.cpp/lobster_bench.cpp call sites). This used to
// be a hand-rolled fixed-capacity (1<<20) open-addressing table, sized
// after a real AAPL day's 143,822 distinct Type-1 ids overflowed an
// earlier 65536 cap silently; a std::unordered_set has no cap to outgrow.
using OrderIdSet = std::unordered_set<int32_t>;

struct LobMsg {
    long long time_ns; // seconds-after-midnight * 1e9, rounded -- exact-integer grouping key
    int  type;
    long order_id;
    long size;
    long price;     // LOBSTER units: dollars * 10000
    int  direction; // 1 = affected order is a bid, -1 = affected order is an ask
    bool valid;
};

inline bool read_message(std::istream &in, LobMsg &m) {
    std::string line;
    if (!std::getline(in, line)) { m.valid = false; return false; }
    double t;
    int type, direction;
    long order_id, size, price;
    if (std::sscanf(line.c_str(), "%lf,%d,%ld,%ld,%ld,%d", &t, &type, &order_id, &size, &price, &direction) != 6) {
        m.valid = false;
        return false;
    }
    m.time_ns = static_cast<long long>(t * 1e9 + (t >= 0 ? 0.5 : -0.5));
    m.type = type;
    m.order_id = order_id;
    m.size = size;
    m.price = price;
    m.direction = direction;
    m.valid = true;
    return true;
}

#endif // LOBSTER_FORMAT_H
