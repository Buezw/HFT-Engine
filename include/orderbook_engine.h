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

typedef struct {
    int order_id;
    int qty;
    int ghost_qty;
    int is_mine;
    int visual_fx; // 0=Normal, 1=Flash, 2=Ghost/pending cleanup
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

#endif // ORDERBOOK_ENGINE_H
