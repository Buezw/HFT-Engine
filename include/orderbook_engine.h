// ============================================================================
// orderbook_engine.h
//
// Second life for this engine: it started as a bare-metal RISC-V program's
// matching logic, got extracted to build/test/benchmark on a normal
// toolchain, and lived for a while with the same constraint the board
// forced on it -- fixed-size arrays, MAX_PRICE_LEVELS price slots per side,
// no malloc. That held up fine for synthetic testing, but running real
// NASDAQ order flow (LOBSTER) through it exposed the actual cost of that
// constraint: once the top tracked price level's liquidity fully drains,
// there's no way to discover the real next-best price, because anything
// outside the narrow window was never recorded in the first place. By the
// end of one real trading day the tracked best bid was off by close to a
// dollar.
//
// The fix real matching engines use is the obvious one: don't cap how many
// price levels you track. This is a C++ rewrite of the core around a
// dynamic, price-indexed book (std::map/std::deque) with no depth limit --
// a level exists exactly when at least one order rests there, same as a
// real venue. Everything that links against it (tests/, bench/, tools/)
// is C++ too now; the free-function ob_* API below is kept as-is so none
// of those call sites had to change shape.
// ============================================================================
#ifndef ORDERBOOK_ENGINE_H
#define ORDERBOOK_ENGINE_H

constexpr long INITIAL_CAPITAL = 500000;
// Pre-trade position limit for the player's own risk-checked market
// orders (see ob_market_*_risk_checked_* below). A random 100k-op stress
// test against the raw engine once drove my_inventory to -168,805 with
// zero pushback, which is what motivated adding this.
constexpr int MAX_POSITION = 500;
// Prices must be positive -- not a depth-window floor anymore (there's no
// window), just basic input validation on insert.
constexpr int MIN_PRICE = 1;

struct EngineAccount {
    long   my_cash;
    int    my_inventory;
    long   current_total_assets;
    int    global_order_id;
    long   total_fill_volume;
    int    window_trade_qty;
};

// Opaque -- the real definition (src/orderbook_engine.cpp) holds a
// std::map<price, level> per side plus (for the opt functions only) an
// order_id index. Nothing outside the engine reaches into it directly;
// use the accessors near the bottom of this file instead.
struct L3OrderBook;

L3OrderBook *ob_create();
void         ob_destroy(L3OrderBook *ob);

// ---- Baseline / optimized, side by side, same as before ----
// *_baseline: linear scan across every order in the book to find one by
// id. *_opt: an order_id -> (side, price) index gets you straight to the
// right price level first (O(log n) via the map), then a scan bounded by
// that one level's depth, not the whole book. Matching itself (the
// market_* functions) is identical between the two now -- there's no
// "unoptimized" version of "walk price levels best-to-worst" worth
// keeping once both use the same dynamic structure; see README for why
// this replaces the old fixed-array baseline/opt split.
void  ob_init_baseline(L3OrderBook *ob, EngineAccount *acc, int base_price);
void  ob_init_opt     (L3OrderBook *ob, EngineAccount *acc, int base_price);

void  ob_market_buy_baseline (L3OrderBook *ob, EngineAccount *acc, int qty, int is_player);
void  ob_market_buy_opt      (L3OrderBook *ob, EngineAccount *acc, int qty, int is_player);
void  ob_market_sell_baseline(L3OrderBook *ob, EngineAccount *acc, int qty, int is_player);
void  ob_market_sell_opt     (L3OrderBook *ob, EngineAccount *acc, int qty, int is_player);

// Inserts a raw order (any is_mine value) at `price`, creating that price
// level if it doesn't exist yet. Used directly for third-party/noise
// liquidity (is_mine=0) the same way it always has been; ob_place_limit_*
// below builds is_mine=1 orders on top of it. Returns 1 on success, 0
// only for invalid input (price <= 0 or qty <= 0) -- there's no capacity
// to run out of anymore.
int   ob_add_order_baseline(L3OrderBook *ob, int is_bid, int price, int order_id, int qty, int is_mine);
int   ob_add_order_opt     (L3OrderBook *ob, int is_bid, int price, int order_id, int qty, int is_mine);

// ----------------------------------------------------------------------
// Resting player limit-order lifecycle (place + cancel-by-id + modify).
// Reserves cash (buy) or inventory (sell) at placement, refunds on
// cancel -- same "is_mine" model as before, just addressed by real price
// instead of a fixed level index. Returns the new order's id (>=0) on
// success, -1 on rejection (bad price, qty <= 0, or can't cover it for
// sells) with nothing reserved.
// ----------------------------------------------------------------------
int ob_place_limit_buy_baseline (L3OrderBook *ob, EngineAccount *acc, int price, int qty);
int ob_place_limit_buy_opt      (L3OrderBook *ob, EngineAccount *acc, int price, int qty);
int ob_place_limit_sell_baseline(L3OrderBook *ob, EngineAccount *acc, int price, int qty);
int ob_place_limit_sell_opt     (L3OrderBook *ob, EngineAccount *acc, int price, int qty);

// Cancels a single resting is_mine order by id, wherever it sits.
// Refunds whatever was reserved at placement. Returns 1 if found and
// cancelled, 0 if not (already filled/cancelled, or not ours).
int ob_cancel_order_baseline(L3OrderBook *ob, EngineAccount *acc, int order_id);
int ob_cancel_order_opt     (L3OrderBook *ob, EngineAccount *acc, int order_id);

// Quantity change in place -- same id, same queue position (keeps time
// priority). Increasing reserves the difference; decreasing refunds it.
// Returns 1 on success, 0 if order_id isn't a live order of ours,
// new_qty <= 0, or an increase can't be covered.
int ob_modify_qty_baseline(L3OrderBook *ob, EngineAccount *acc, int order_id, int new_qty);
int ob_modify_qty_opt     (L3OrderBook *ob, EngineAccount *acc, int order_id, int new_qty);

// Price change -- cancel-old + place-new (place first, only cancel the
// old one once the new placement is confirmed, so a rejected move leaves
// the original completely untouched). New id, back of the new price's
// queue, same as placing any other new order. Returns the new id (>=0)
// or -1 on rejection.
int ob_modify_price_baseline(L3OrderBook *ob, EngineAccount *acc, int order_id, int new_price, int new_qty);
int ob_modify_price_opt     (L3OrderBook *ob, EngineAccount *acc, int order_id, int new_price, int new_qty);

// ----------------------------------------------------------------------
// Third-party (is_mine=0) order cancel / qty-reduce -- for replaying real
// order flow (LOBSTER etc.), where cancelled/reduced orders overwhelm-
// ingly belong to anonymous participants, not "the player". Same as
// ob_cancel_order_*/ob_modify_qty_* above minus the is_mine filter and
// the cash/inventory refund (third-party orders never reserved any).
//
// ob_reduce_order_qty_any_*: delta_qty is an amount to REMOVE (LOBSTER
// Type 2 semantics), not a new absolute quantity. Rejects (nothing
// mutated) if delta_qty <= 0 or > the order's current qty; equal to the
// current qty fully removes the order.
// ----------------------------------------------------------------------
int ob_cancel_order_any_baseline(L3OrderBook *ob, int order_id);
int ob_cancel_order_any_opt     (L3OrderBook *ob, int order_id);
int ob_reduce_order_qty_any_baseline(L3OrderBook *ob, int order_id, int delta_qty);
int ob_reduce_order_qty_any_opt     (L3OrderBook *ob, int order_id, int delta_qty);

// ----------------------------------------------------------------------
// Pre-trade risk limit for the player's own market orders. Wraps
// ob_market_buy_*/ob_market_sell_* (unchanged) rather than modifying
// them. Clips the requested qty down to whatever room remains under
// MAX_POSITION before it touches the book. Returns the qty actually
// submitted (may be less than requested, or 0 -- not an error, just "no
// trade, on purpose").
// ----------------------------------------------------------------------
int ob_market_buy_risk_checked_baseline (L3OrderBook *ob, EngineAccount *acc, int qty);
int ob_market_buy_risk_checked_opt      (L3OrderBook *ob, EngineAccount *acc, int qty);
int ob_market_sell_risk_checked_baseline(L3OrderBook *ob, EngineAccount *acc, int qty);
int ob_market_sell_risk_checked_opt     (L3OrderBook *ob, EngineAccount *acc, int qty);

// ----------------------------------------------------------------------
// Read-only accessors -- L3OrderBook is opaque now, so this is how
// outside code (tests, benchmarks, tools/book_trace.cpp, the LOBSTER
// tools) reads book state instead of reaching into struct fields
// directly. level_index counts from the best price (0 = best). All
// return -1 (or 0 for ob_level_order_at) if is_bid/level_index/slot is
// out of range, rather than crashing on a bad index.
// ----------------------------------------------------------------------
int  ob_num_levels(const L3OrderBook *ob, int is_bid);
int  ob_level_price(const L3OrderBook *ob, int is_bid, int level_index);
long ob_level_qty(const L3OrderBook *ob, int is_bid, int level_index);
int  ob_level_order_count(const L3OrderBook *ob, int is_bid, int level_index);
int  ob_level_order_at(const L3OrderBook *ob, int is_bid, int level_index, int slot,
                        int *out_order_id, int *out_qty, int *out_is_mine);

// Deep, semantic equality (same price levels in the same order, same qty
// at each, same orders resting at each one) -- for baseline/opt
// differential testing. Implemented natively over the internal
// std::map/std::deque (a single linear pass, O(total orders)) rather
// than composed from the per-index accessors above, which would be
// O(levels) *per call* and O(levels^2) if you looped them -- exactly
// the mistake this function exists to let callers avoid.
int ob_books_equal(const L3OrderBook *a, const L3OrderBook *b);

// Dumps up to max_levels levels of one side (best first) into two
// parallel arrays in a single pass -- O(levels), not O(levels) per
// index the way calling ob_level_price/ob_level_qty in a loop would be.
// Returns how many levels were actually written.
int ob_dump_levels(const L3OrderBook *ob, int is_bid, int max_levels, int *out_prices, long *out_qtys);

// Total resting qty at an exact price (0 if nothing rests there, not an
// error -- a price with no orders just isn't a level). O(log levels).
// This is what makes comparing against an external ground truth (see the
// LOBSTER tools) a direct lookup instead of the tick-offset
// reconstruction the old fixed-array engine needed: both sides now only
// ever represent prices that actually have resting orders, so there's no
// representational gap to bridge anymore.
long ob_qty_at_price(const L3OrderBook *ob, int is_bid, int price);

#endif // ORDERBOOK_ENGINE_H
