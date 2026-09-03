// ============================================================================
// orderbook_engine.h
//
// Platform-independent core of the L3 matching engine originally written for
// bare-metal RISC-V (DE1-SoC). Extracted so the matching logic can be built,
// unit-tested, and benchmarked on x86 with a normal toolchain (gcc/clang),
// without needing the FPGA board or a RISC-V cross-compiler.
//
// Design constraints carried over from the bare-metal version on purpose:
//   - No malloc/free. Fixed-size static arrays only (same memory model as
//     the original, so benchmark results are representative of the
//     bare-metal build, not an artifact of switching to dynamic allocation).
//   - No floating point (the original board build disables hardware FPU
//     paths / avoids soft-float call overhead).
// ============================================================================
#ifndef ORDERBOOK_ENGINE_H
#define ORDERBOOK_ENGINE_H

#include <stdint.h>

#define MAX_PRICE_LEVELS   3
// Overridable via -DMAX_ORDERS_PER_LVL=N at compile time (see
// `make depth-sweep`), to measure how the clean_ghosts/update_total_qty
// optimizations scale as book depth grows past the board's real value of
// 10 — without touching this file for every depth tested.
#ifndef MAX_ORDERS_PER_LVL
#define MAX_ORDERS_PER_LVL 10
#endif
#define INITIAL_CAPITAL    500000
// Pre-trade position limit for the player's own risk-checked market
// orders (see ob_market_*_risk_checked_* below). Not present in main.c —
// main.c's player_market_sell has no floor at all; a random 100k-op
// stress test against the raw engine drove my_inventory to -168,805
// with zero pushback, which is what motivated adding this.
#define MAX_POSITION       500

typedef struct {
    int order_id;
    int qty;
    int ghost_qty;
    int is_mine;
    // Inherited its name from main.c's L3Order.visual_fx (there: 0=Normal,
    // 1=Flash white on fill, 2=Ghost/pending cleanup — a UI render-state
    // field). Only the Ghost value ever gets set or checked in this
    // extracted engine (main.c's rendering code, which is the only place
    // that ever used Flash, isn't part of this build) — so here it's
    // purely matching-engine state, not a UI flag: 2 means "filled or
    // cancelled, still occupying a queue slot until the next compaction
    // pass removes it." Renamed to `state` to match what it actually does
    // in this codebase, not what it was called in the one it came from.
    int state; // 0=live, 2=ghost (filled/cancelled, pending ob_clean_ghosts_* compaction)
} L3Order;

typedef struct {
    int price;
    int total_qty;
    int order_count;
    L3Order queue[MAX_ORDERS_PER_LVL];
} L3PriceLevel;

typedef struct {
    L3PriceLevel bids[MAX_PRICE_LEVELS];
    L3PriceLevel asks[MAX_PRICE_LEVELS];
} L3OrderBook;

typedef struct {
    long   my_cash;
    int    my_inventory;
    long   current_total_assets;
    int    global_order_id;
    long   total_fill_volume;
    int    window_trade_qty;
} EngineAccount;

// ---- Original (baseline) implementation, functionally identical to main.c ----
void  ob_init_baseline(L3OrderBook *ob, EngineAccount *acc, int base_price);
void  ob_clean_ghosts_baseline(L3PriceLevel *lvl);
void  ob_update_total_qty_baseline(L3OrderBook *ob);
void  ob_market_buy_baseline(L3OrderBook *ob, EngineAccount *acc, int qty, int is_player);
void  ob_market_sell_baseline(L3OrderBook *ob, EngineAccount *acc, int qty, int is_player);

// ---- Optimized implementation (see orderbook_engine.c for the changes) ----
void  ob_init_opt(L3OrderBook *ob, EngineAccount *acc, int base_price);
void  ob_clean_ghosts_opt(L3PriceLevel *lvl);
void  ob_update_total_qty_opt(L3OrderBook *ob);
void  ob_market_buy_opt(L3OrderBook *ob, EngineAccount *acc, int qty, int is_player);
void  ob_market_sell_opt(L3OrderBook *ob, EngineAccount *acc, int qty, int is_player);

// Must be called by ANY code path that pushes a new order directly into
// lvl->queue (e.g. the market-maker liquidity injection in process_tick).
// This is the other half of "opt" incremental maintenance: fills/cancels
// update total_qty incrementally inside ob_market_*_opt, and insertions
// must update it incrementally here — otherwise total_qty silently drifts
// out of sync with the true sum, since ob_update_total_qty_opt is no longer
// called every tick to paper over it.
//
// Bounds-checked: lvl->queue has fixed capacity MAX_ORDERS_PER_LVL. Returns
// 1 if the order was inserted, 0 if the level was already full (order
// dropped, no state changed). The caller must not assume insertion always
// succeeds. See README for why this check lives here rather than at each
// call site.
int   ob_add_order_opt(L3PriceLevel *lvl, L3Order order);

// Baseline counterpart of ob_add_order_opt, with the same bounds-checked
// contract (returns 1/0), so both implementations can be driven through an
// identical function-pointer-compatible API in tests/benchmarks. Baseline
// deliberately does NOT touch total_qty here — it stays correct only after
// the next ob_update_total_qty_baseline() full rescan, matching main.c.
int   ob_add_order_baseline(L3PriceLevel *lvl, L3Order order);

// ----------------------------------------------------------------------
// Resting player limit-order lifecycle (place + cancel-by-id).
//
// NOT present in main.c as a callable function. main.c's L3Order.is_mine
// is *checked* in eight places (self-trade exclusion in the market-order
// matchers, order rendering color, player_cancel_all_orders) but never
// *set* to 1 anywhere in that 919-line file — grep for `is_mine = 1`
// comes back empty. player_cancel_all_orders's cash/inventory refund
// logic implies a "reserve on placement, refund on cancel" model that,
// as shipped, has no placement function to pair with it: a real, if
// incomplete, feature.
//
// The functions below complete that lifecycle inside the portable engine
// (not board/main.c, which stays a verbatim historical artifact): reserve
// cash/inventory at placement, insert an is_mine=1 resting order, and
// cancel-by-id refunds exactly what was reserved. This is what actually
// makes is_mine, the self-trade exclusion, and player_cancel_all_orders's
// refund logic meaningful instead of dead branches.
// ----------------------------------------------------------------------

// Places a resting is_mine buy at bids[level] / sell at asks[level] for
// `qty`, reserving cash (buy) or inventory (sell) immediately, same as a
// real limit order ties up capital the moment it rests in the book.
// Returns the new order's id (>=0) on success, -1 if level is out of
// range, qty <= 0, or the level's queue is already full (nothing
// reserved on failure).
int ob_place_limit_buy_baseline (L3OrderBook *ob, EngineAccount *acc, int level, int qty);
int ob_place_limit_buy_opt      (L3OrderBook *ob, EngineAccount *acc, int level, int qty);
int ob_place_limit_sell_baseline(L3OrderBook *ob, EngineAccount *acc, int level, int qty);
int ob_place_limit_sell_opt     (L3OrderBook *ob, EngineAccount *acc, int level, int qty);

// Cancels a single resting is_mine order by id, wherever it sits in the
// book (bids or asks, any level), refunding the cash/inventory reserved
// at placement. Returns 1 if found and cancelled, 0 if not found (already
// filled, already cancelled, or an id that never existed / isn't ours).
//
// O(orders) linear scan across the whole book. Deliberately not indexed:
// book size is capped at MAX_PRICE_LEVELS * MAX_ORDERS_PER_LVL * 2 orders
// (60 at the board's real depth), so a scan is a handful of cache-line
// reads, not a bottleneck. A production engine handling unbounded depth
// would maintain an order_id -> (side, level, index) map for O(1) cancel;
// not worth the complexity here without evidence (profiling) that this
// scan is ever hot, at this depth.
int ob_cancel_order_baseline(L3OrderBook *ob, EngineAccount *acc, int order_id);
int ob_cancel_order_opt     (L3OrderBook *ob, EngineAccount *acc, int order_id);

// ----------------------------------------------------------------------
// Pre-trade risk limit for the player's own market orders.
//
// main.c's player_market_buy/player_market_sell have no position check
// at all — a real (unconstrained) test run against this engine's raw
// ob_market_sell_*(..., is_player=1) drove my_inventory to -168,805 over
// 100,000 random operations with nothing stopping it. Real venues call
// this a pre-trade position/fat-finger limit: the last check before an
// order is allowed to add more risk than the desk is willing to carry.
//
// These wrap ob_market_buy_*/ob_market_sell_* (unchanged, still exactly
// main.c's behavior — is_player=1 fills, no limit) rather than modifying
// them: main.c's ported functions need to keep being a faithful port,
// since the correctness harness's whole methodology depends on baseline
// staying baseline. The risk check is a layer on top, not a change to
// what "baseline" means.
//
// Behavior: clips the requested qty down to whatever room remains under
// MAX_POSITION before it touches the book (reduce, don't bounce) — mirrors
// how real pre-trade size limiters behave. Returns the qty actually
// submitted to matching, which may be less than requested, or 0 if
// already at the limit (0 is not an error; it means "no trade happened,
// on purpose"). Only meaningful for the player's own position — the
// market-maker/noise side of the simulated book (is_player=0 fills) is
// not the desk's own risk and is intentionally not gated here.
int ob_market_buy_risk_checked_baseline (L3OrderBook *ob, EngineAccount *acc, int qty);
int ob_market_buy_risk_checked_opt      (L3OrderBook *ob, EngineAccount *acc, int qty);
int ob_market_sell_risk_checked_baseline(L3OrderBook *ob, EngineAccount *acc, int qty);
int ob_market_sell_risk_checked_opt     (L3OrderBook *ob, EngineAccount *acc, int qty);

// ----------------------------------------------------------------------
// O(1) cancel-by-id index (opt only, x86 side — not part of the memcmp'd
// L3OrderBook/L3PriceLevel/L3Order layout the baseline<->opt correctness
// harness in tests/test_correctness.c relies on).
//
// ob_cancel_order_opt (above) is a deliberate O(orders-in-book) linear
// scan across the whole book, justified there by the book being capped at
// ~60 orders at the board's real depth. That justification stops holding
// once MAX_ORDERS_PER_LVL is scaled up (see `make depth-sweep`, which
// drives it to 400) — at that depth a cancel walks up to 2400 order
// slots. This section adds an O(1)-lookup path for that case, without
// touching ob_cancel_order_opt or the shared L3OrderBook/L3PriceLevel/
// L3Order types at all, because of two hard constraints already baked
// into this codebase:
//
//   1. tests/test_correctness.c proves baseline == opt with a raw
//      memcmp(a, b, sizeof(L3OrderBook)). Any extra per-order bookkeeping
//      (a self-index, a generation counter, ...) added to L3Order or
//      L3PriceLevel would make that memcmp fail immediately, since
//      baseline has no equivalent field to keep in sync. So the index
//      lives in its own struct, populated/maintained by new wrapper
//      functions, not inside the order/level structs themselves.
//
//   2. bench/benchmark.c drives ob_clean_ghosts_opt through a generic
//      `void (*)(L3PriceLevel *)` function pointer, identically to
//      baseline's clean_ghosts, so the two stay directly comparable.
//      Giving ob_clean_ghosts_opt an extra parameter to keep an index in
//      sync during compaction would break that shared calling contract.
//      So ob_clean_ghosts_opt is left untouched, which means a compaction
//      can still silently move an indexed order to a new slot within its
//      level without the index knowing.
//
// The design that fits both constraints: the index caches (level, slot)
// per order id. A lookup checks the cached slot first — O(1), and correct
// whenever no compaction has touched that level since the order was
// placed or last found, which is the common case. If the cached slot
// doesn't match (a compaction moved it), the fallback rescans only that
// ONE level (bounded by MAX_ORDERS_PER_LVL), never the rest of the book.
// So this is O(1) amortized with an O(single-level-depth) worst case,
// strictly better than baseline's unconditional O(whole-book-depth) in
// every case, and exactly O(1) in the common one.
//
// Open addressing, linear probing, fixed-size static array — no malloc,
// same memory model as everything else here. Capacity is a flat 8192:
// the maximum possible concurrently-live is_mine orders across every
// depth `make depth-sweep` tests (DEPTHS up to 400 in the Makefile) is
// 2 sides * MAX_PRICE_LEVELS(3) * 400 = 2400, so 8192 keeps the load
// factor under ~30% even at the largest swept depth.
// ----------------------------------------------------------------------
#define ORDER_INDEX_CAPACITY 8192

typedef struct {
    int32_t        order_id; // -1 = empty slot, -2 = tombstone (cancelled/removed)
    L3PriceLevel  *lvl;
    uint16_t       slot;
    uint8_t        is_bid;
} OrderIndexEntry;

typedef struct {
    OrderIndexEntry buckets[ORDER_INDEX_CAPACITY];
} OrderIndex;

// Must be called once before first use (equivalent of ob_init_* for the
// index itself). Marks every bucket empty.
void ob_index_init(OrderIndex *idx);

// Same contract as ob_place_limit_buy_opt/ob_place_limit_sell_opt (see
// above), but also registers the new order in `idx` so it can later be
// found in O(1) by ob_cancel_order_opt_indexed. Internally calls the
// existing, already-tested ob_place_limit_buy_opt/sell_opt — this does
// not duplicate or reimplement the placement logic, only adds indexing
// on top of it.
int ob_place_limit_buy_opt_indexed (L3OrderBook *ob, EngineAccount *acc, OrderIndex *idx, int level, int qty);
int ob_place_limit_sell_opt_indexed(L3OrderBook *ob, EngineAccount *acc, OrderIndex *idx, int level, int qty);

// O(1) amortized counterpart to ob_cancel_order_opt: hashes order_id to
// its cached (level, slot) instead of scanning the whole book. Falls back
// to scanning only the one level the index points at if the cached slot
// went stale (see file comment above for exactly when/why that happens).
// Same return contract as ob_cancel_order_opt: 1 if found and cancelled,
// 0 if not (already gone, or never existed).
int ob_cancel_order_opt_indexed(L3OrderBook *ob, EngineAccount *acc, OrderIndex *idx, int order_id);

#endif // ORDERBOOK_ENGINE_H
