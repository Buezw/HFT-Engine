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
#define MAX_ORDERS_PER_LVL 10
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

#endif // ORDERBOOK_ENGINE_H
