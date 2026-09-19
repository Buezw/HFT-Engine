// ============================================================================
// orderbook_engine.cpp
//
// C++ rewrite around a dynamic, price-indexed book (std::map per side,
// std::deque per level) -- no MAX_PRICE_LEVELS, no MAX_ORDERS_PER_LVL, no
// fixed capacity anywhere. A price level exists exactly when at least one
// order rests there, same as a real venue. See the header for why.
//
// This also quietly removes a few things the old fixed-array design
// needed that a dynamic one doesn't:
//   - ghost/state=2 + ob_clean_ghosts_* -- a filled/cancelled order is
//     just erased from its deque directly, no delayed compaction pass.
//   - ob_update_total_qty_* -- total_qty is maintained incrementally at
//     every mutation site now, for both baseline and opt (there's no
//     "rescan later" fallback anymore because there's nothing to rescan
//     that isn't already correct).
//   - ob_drift_price_*/ob_drift_bid_price_*/ob_drift_ask_price_* -- drift
//     existed to relabel fixed slots as the market moved. A dynamic book
//     just gets a new map key for a new best price; nothing to relabel.
//   - OrderIndex / ob_index_init / the *_opt_indexed function family --
//     opt's order_index member here does that job natively.
//
// baseline vs opt, redefined: both use the exact same dynamic structure
// and matching logic now (there's no "unoptimized" version of "walk price
// levels best to worst" worth keeping once both are backed by the same
// map). The only real difference left is how an order gets found by id
// for cancel/modify: baseline does a full linear scan across every order
// in the book; opt keeps an order_id -> (side, price) index so it jumps
// straight to the right level (O(log n) via the map) and only scans that
// one level's depth from there. Same idea as the old cancel-by-id index,
// just built into one implementation instead of bolted on as a second,
// parallel API.
// ============================================================================
#include "orderbook_engine.h"
#include <map>
#include <deque>
#include <unordered_map>
#include <utility>
#include <algorithm>

namespace {

struct Order {
    int order_id;
    int qty;
    int is_mine;
};

struct PriceLevel {
    int price = 0;
    long total_qty = 0;
    std::deque<Order> queue;
};

bool operator==(const Order &a, const Order &b) {
    return a.order_id == b.order_id && a.qty == b.qty && a.is_mine == b.is_mine;
}
bool operator==(const PriceLevel &a, const PriceLevel &b) {
    return a.price == b.price && a.total_qty == b.total_qty && a.queue == b.queue;
}

struct FoundOrder {
    bool is_bid;
    int price;
    PriceLevel *lvl;
    std::deque<Order>::iterator it;
};

} // namespace

struct L3OrderBook {
    std::map<int, PriceLevel, std::greater<int>> bids; // begin() = best (highest) bid
    std::map<int, PriceLevel, std::less<int>>    asks; // begin() = best (lowest) ask
    // order_id -> (is_bid, price). Only the _opt functions read or write
    // this; a baseline instance just never touches it.
    std::unordered_map<int, std::pair<bool, int>> order_index;
};

namespace {

// Every mutating function below takes a `use_index` flag rather than
// being duplicated wholesale for baseline vs opt -- the two only ever
// differ in whether they consult/maintain order_index, so writing that
// out twice per function would just be the same logic copy-pasted with
// one bit flipped.

std::deque<Order>::iterator scan_level_for(PriceLevel &lvl, int order_id, bool require_mine) {
    for (auto it = lvl.queue.begin(); it != lvl.queue.end(); ++it) {
        if (it->order_id == order_id && (!require_mine || it->is_mine)) return it;
    }
    return lvl.queue.end();
}

bool find_order(L3OrderBook *ob, int order_id, bool require_mine, bool use_index, FoundOrder &out) {
    if (use_index) {
        auto idx_it = ob->order_index.find(order_id);
        if (idx_it == ob->order_index.end()) return false;
        bool is_bid = idx_it->second.first;
        int price = idx_it->second.second;
        if (is_bid) {
            auto lvl_it = ob->bids.find(price);
            if (lvl_it != ob->bids.end()) {
                auto oit = scan_level_for(lvl_it->second, order_id, require_mine);
                if (oit != lvl_it->second.queue.end()) {
                    out = FoundOrder{true, price, &lvl_it->second, oit};
                    return true;
                }
            }
        } else {
            auto lvl_it = ob->asks.find(price);
            if (lvl_it != ob->asks.end()) {
                auto oit = scan_level_for(lvl_it->second, order_id, require_mine);
                if (oit != lvl_it->second.queue.end()) {
                    out = FoundOrder{false, price, &lvl_it->second, oit};
                    return true;
                }
            }
        }
        // In the index but not actually there -- stale, don't fall back
        // to a full scan (that would defeat the point of having an index).
        return false;
    }
    for (auto &kv : ob->bids) {
        auto oit = scan_level_for(kv.second, order_id, require_mine);
        if (oit != kv.second.queue.end()) { out = FoundOrder{true, kv.first, &kv.second, oit}; return true; }
    }
    for (auto &kv : ob->asks) {
        auto oit = scan_level_for(kv.second, order_id, require_mine);
        if (oit != kv.second.queue.end()) { out = FoundOrder{false, kv.first, &kv.second, oit}; return true; }
    }
    return false;
}

// Removes the level itself once its last order is gone -- this is the
// dynamic-book equivalent of a fixed slot just sitting there empty; here
// an empty level genuinely stops existing.
void erase_order(L3OrderBook *ob, FoundOrder &f, bool use_index) {
    if (use_index) ob->order_index.erase(f.it->order_id);
    f.lvl->queue.erase(f.it);
    if (f.lvl->queue.empty()) {
        if (f.is_bid) ob->bids.erase(f.price);
        else          ob->asks.erase(f.price);
    }
}

int add_order_impl(L3OrderBook *ob, int is_bid, int price, int order_id, int qty, int is_mine, bool use_index) {
    if (price <= 0 || qty <= 0) return 0;
    if (is_bid) {
        PriceLevel &lvl = ob->bids[price];
        lvl.price = price;
        lvl.queue.push_back(Order{order_id, qty, is_mine});
        lvl.total_qty += qty;
    } else {
        PriceLevel &lvl = ob->asks[price];
        lvl.price = price;
        lvl.queue.push_back(Order{order_id, qty, is_mine});
        lvl.total_qty += qty;
    }
    if (use_index) ob->order_index[order_id] = {static_cast<bool>(is_bid), price};
    return 1;
}

int place_limit_buy_impl(L3OrderBook *ob, EngineAccount *acc, int price, int qty, bool use_index) {
    if (price <= 0 || qty <= 0) return -1;
    int id = acc->global_order_id;
    if (!add_order_impl(ob, /*is_bid=*/1, price, id, qty, /*is_mine=*/1, use_index)) return -1;
    acc->global_order_id++;
    acc->my_cash -= (long)qty * price;
    return id;
}

int place_limit_sell_impl(L3OrderBook *ob, EngineAccount *acc, int price, int qty, bool use_index) {
    if (price <= 0 || qty <= 0) return -1;
    if (qty > acc->my_inventory) return -1;
    int id = acc->global_order_id;
    if (!add_order_impl(ob, /*is_bid=*/0, price, id, qty, /*is_mine=*/1, use_index)) return -1;
    acc->global_order_id++;
    acc->my_inventory -= qty;
    return id;
}

int cancel_order_impl(L3OrderBook *ob, EngineAccount *acc, int order_id, bool use_index) {
    FoundOrder f;
    if (!find_order(ob, order_id, /*require_mine=*/true, use_index, f)) return 0;
    int qty = f.it->qty;
    if (f.is_bid) acc->my_cash += (long)qty * f.price;
    else          acc->my_inventory += qty;
    f.lvl->total_qty -= qty;
    erase_order(ob, f, use_index);
    return 1;
}

int modify_qty_impl(L3OrderBook *ob, EngineAccount *acc, int order_id, int new_qty, bool use_index) {
    if (new_qty <= 0) return 0;
    FoundOrder f;
    if (!find_order(ob, order_id, /*require_mine=*/true, use_index, f)) return 0;
    int delta = new_qty - f.it->qty;
    if (f.is_bid) {
        long cash_delta = (long)delta * f.price;
        if (delta > 0 && cash_delta > acc->my_cash) return 0;
        acc->my_cash -= cash_delta;
    } else {
        if (delta > 0 && delta > acc->my_inventory) return 0;
        acc->my_inventory -= delta;
    }
    f.it->qty = new_qty;
    f.lvl->total_qty += delta;
    return 1;
}

int modify_price_impl(L3OrderBook *ob, EngineAccount *acc, int order_id, int new_price, int new_qty, bool use_index) {
    FoundOrder f;
    if (!find_order(ob, order_id, /*require_mine=*/true, use_index, f)) return -1;
    bool is_bid = f.is_bid;
    int new_id = is_bid ? place_limit_buy_impl(ob, acc, new_price, new_qty, use_index)
                         : place_limit_sell_impl(ob, acc, new_price, new_qty, use_index);
    if (new_id < 0) return -1; // old order left completely untouched
    cancel_order_impl(ob, acc, order_id, use_index); // refund the old reservation
    return new_id;
}

int cancel_order_any_impl(L3OrderBook *ob, int order_id, bool use_index) {
    FoundOrder f;
    if (!find_order(ob, order_id, /*require_mine=*/false, use_index, f)) return 0;
    f.lvl->total_qty -= f.it->qty;
    erase_order(ob, f, use_index);
    return 1;
}

int reduce_qty_any_impl(L3OrderBook *ob, int order_id, int delta_qty, bool use_index) {
    if (delta_qty <= 0) return 0;
    FoundOrder f;
    if (!find_order(ob, order_id, /*require_mine=*/false, use_index, f)) return 0;
    if (delta_qty > f.it->qty) return 0;
    f.lvl->total_qty -= delta_qty;
    if (delta_qty == f.it->qty) {
        erase_order(ob, f, use_index);
    } else {
        f.it->qty -= delta_qty;
    }
    return 1;
}

// Shared by both market_buy_* (walks asks) and market_sell_* (walks
// bids) -- credit_as_buy picks which direction fills get credited in.
//
// Two fill cases hit the account, and they are NOT symmetric:
//   - is_player: the player's own aggressive market order eating resting
//     liquidity nobody has reserved anything against yet -- apply the
//     full fill (both legs) here.
//   - !is_player && ord_it->is_mine: someone else's flow filling one of
//     the player's own resting limit orders. place_limit_buy_impl/
//     place_limit_sell_impl already reserved one leg of this trade up
//     front (cash for a buy, inventory for a sell) -- see their comments.
//     Touching that same leg again here would double-charge the fill, so
//     only the leg that was NOT pre-reserved gets settled.
// (is_player && ord_it->is_mine, the player crossing their own resting
// order, is skipped entirely just below -- no self-trades.)
template <typename Cmp>
void match_against(L3OrderBook *ob, std::map<int, PriceLevel, Cmp> &levels, EngineAccount *acc,
                    int qty, int is_player, bool credit_as_buy, bool use_index) {
    for (auto lvl_it = levels.begin(); lvl_it != levels.end() && qty > 0;) {
        PriceLevel &lvl = lvl_it->second;
        long filled_here = 0;
        for (auto ord_it = lvl.queue.begin(); ord_it != lvl.queue.end() && qty > 0;) {
            if (is_player && ord_it->is_mine) { ++ord_it; continue; }

            int fill = std::min(qty, ord_it->qty);
            ord_it->qty -= fill;
            qty -= fill;
            filled_here += fill;

            if (is_player) {
                if (credit_as_buy) {
                    acc->my_inventory += fill;
                    acc->my_cash -= (long)fill * lvl.price;
                } else {
                    acc->my_inventory -= fill;
                    acc->my_cash += (long)fill * lvl.price;
                }
                acc->total_fill_volume += fill;
                acc->window_trade_qty += fill;
            } else if (ord_it->is_mine) {
                if (credit_as_buy) acc->my_cash += (long)fill * lvl.price; // resting sell: inventory was reserved at placement
                else               acc->my_inventory += fill;             // resting buy: cash was reserved at placement
                acc->total_fill_volume += fill;
                acc->window_trade_qty += fill;
            }

            if (ord_it->qty == 0) {
                if (use_index) ob->order_index.erase(ord_it->order_id);
                ord_it = lvl.queue.erase(ord_it);
            } else {
                ++ord_it;
            }
        }
        lvl.total_qty -= filled_here;
        if (lvl.queue.empty()) lvl_it = levels.erase(lvl_it);
        else                   ++lvl_it;
    }
}

void init_common(L3OrderBook *ob, EngineAccount *acc) {
    ob->bids.clear();
    ob->asks.clear();
    ob->order_index.clear();
    acc->my_cash = INITIAL_CAPITAL;
    acc->my_inventory = 0;
    acc->current_total_assets = INITIAL_CAPITAL;
    acc->global_order_id = 1000;
    acc->total_fill_volume = 0;
    acc->window_trade_qty = 0;
}

// How many levels ob_init_* seeds by default -- a starting shape, not a
// cap (nothing stops the book from growing past this the moment a real
// order arrives outside it).
constexpr int kInitLevels = 3;

void init_impl(L3OrderBook *ob, EngineAccount *acc, int base_price, bool use_index) {
    init_common(ob, acc);
    for (int i = 0; i < kInitLevels; i++) {
        add_order_impl(ob, 1, base_price - 1 - i, acc->global_order_id++, 40, 0, use_index);
        add_order_impl(ob, 1, base_price - 1 - i, acc->global_order_id++, 80, 0, use_index);
        add_order_impl(ob, 1, base_price - 1 - i, acc->global_order_id++, 60, 0, use_index);
        add_order_impl(ob, 0, base_price + 1 + i, acc->global_order_id++, 50, 0, use_index);
        add_order_impl(ob, 0, base_price + 1 + i, acc->global_order_id++, 100, 0, use_index);
    }
}

const PriceLevel *level_at(const L3OrderBook *ob, int is_bid, int level_index) {
    if (level_index < 0) return nullptr;
    if (is_bid) {
        if (level_index >= (int)ob->bids.size()) return nullptr;
        auto it = ob->bids.begin();
        std::advance(it, level_index);
        return &it->second;
    }
    if (level_index >= (int)ob->asks.size()) return nullptr;
    auto it = ob->asks.begin();
    std::advance(it, level_index);
    return &it->second;
}

} // namespace

L3OrderBook *ob_create() { return new L3OrderBook(); }
void ob_destroy(L3OrderBook *ob) { delete ob; }

void ob_init_baseline(L3OrderBook *ob, EngineAccount *acc, int base_price) { init_impl(ob, acc, base_price, false); }
void ob_init_opt     (L3OrderBook *ob, EngineAccount *acc, int base_price) { init_impl(ob, acc, base_price, true); }

void ob_market_buy_baseline(L3OrderBook *ob, EngineAccount *acc, int qty, int is_player) {
    match_against(ob, ob->asks, acc, qty, is_player, /*credit_as_buy=*/true, /*use_index=*/false);
}
void ob_market_buy_opt(L3OrderBook *ob, EngineAccount *acc, int qty, int is_player) {
    match_against(ob, ob->asks, acc, qty, is_player, /*credit_as_buy=*/true, /*use_index=*/true);
}
void ob_market_sell_baseline(L3OrderBook *ob, EngineAccount *acc, int qty, int is_player) {
    match_against(ob, ob->bids, acc, qty, is_player, /*credit_as_buy=*/false, /*use_index=*/false);
}
void ob_market_sell_opt(L3OrderBook *ob, EngineAccount *acc, int qty, int is_player) {
    match_against(ob, ob->bids, acc, qty, is_player, /*credit_as_buy=*/false, /*use_index=*/true);
}

int ob_add_order_baseline(L3OrderBook *ob, int is_bid, int price, int order_id, int qty, int is_mine) {
    return add_order_impl(ob, is_bid, price, order_id, qty, is_mine, false);
}
int ob_add_order_opt(L3OrderBook *ob, int is_bid, int price, int order_id, int qty, int is_mine) {
    return add_order_impl(ob, is_bid, price, order_id, qty, is_mine, true);
}

int ob_place_limit_buy_baseline (L3OrderBook *ob, EngineAccount *acc, int price, int qty) { return place_limit_buy_impl(ob, acc, price, qty, false); }
int ob_place_limit_buy_opt      (L3OrderBook *ob, EngineAccount *acc, int price, int qty) { return place_limit_buy_impl(ob, acc, price, qty, true); }
int ob_place_limit_sell_baseline(L3OrderBook *ob, EngineAccount *acc, int price, int qty) { return place_limit_sell_impl(ob, acc, price, qty, false); }
int ob_place_limit_sell_opt     (L3OrderBook *ob, EngineAccount *acc, int price, int qty) { return place_limit_sell_impl(ob, acc, price, qty, true); }

int ob_cancel_order_baseline(L3OrderBook *ob, EngineAccount *acc, int order_id) { return cancel_order_impl(ob, acc, order_id, false); }
int ob_cancel_order_opt     (L3OrderBook *ob, EngineAccount *acc, int order_id) { return cancel_order_impl(ob, acc, order_id, true); }

int ob_modify_qty_baseline(L3OrderBook *ob, EngineAccount *acc, int order_id, int new_qty) { return modify_qty_impl(ob, acc, order_id, new_qty, false); }
int ob_modify_qty_opt     (L3OrderBook *ob, EngineAccount *acc, int order_id, int new_qty) { return modify_qty_impl(ob, acc, order_id, new_qty, true); }

int ob_modify_price_baseline(L3OrderBook *ob, EngineAccount *acc, int order_id, int new_price, int new_qty) { return modify_price_impl(ob, acc, order_id, new_price, new_qty, false); }
int ob_modify_price_opt     (L3OrderBook *ob, EngineAccount *acc, int order_id, int new_price, int new_qty) { return modify_price_impl(ob, acc, order_id, new_price, new_qty, true); }

int ob_cancel_order_any_baseline(L3OrderBook *ob, int order_id) { return cancel_order_any_impl(ob, order_id, false); }
int ob_cancel_order_any_opt     (L3OrderBook *ob, int order_id) { return cancel_order_any_impl(ob, order_id, true); }

int ob_reduce_order_qty_any_baseline(L3OrderBook *ob, int order_id, int delta_qty) { return reduce_qty_any_impl(ob, order_id, delta_qty, false); }
int ob_reduce_order_qty_any_opt     (L3OrderBook *ob, int order_id, int delta_qty) { return reduce_qty_any_impl(ob, order_id, delta_qty, true); }

int ob_market_buy_risk_checked_baseline(L3OrderBook *ob, EngineAccount *acc, int qty) {
    int room = MAX_POSITION - acc->my_inventory;
    if (room <= 0) return 0;
    int clipped = (qty < room) ? qty : room;
    ob_market_buy_baseline(ob, acc, clipped, /*is_player=*/1);
    return clipped;
}
int ob_market_buy_risk_checked_opt(L3OrderBook *ob, EngineAccount *acc, int qty) {
    int room = MAX_POSITION - acc->my_inventory;
    if (room <= 0) return 0;
    int clipped = (qty < room) ? qty : room;
    ob_market_buy_opt(ob, acc, clipped, /*is_player=*/1);
    return clipped;
}
int ob_market_sell_risk_checked_baseline(L3OrderBook *ob, EngineAccount *acc, int qty) {
    int room = MAX_POSITION + acc->my_inventory;
    if (room <= 0) return 0;
    int clipped = (qty < room) ? qty : room;
    ob_market_sell_baseline(ob, acc, clipped, /*is_player=*/1);
    return clipped;
}
int ob_market_sell_risk_checked_opt(L3OrderBook *ob, EngineAccount *acc, int qty) {
    int room = MAX_POSITION + acc->my_inventory;
    if (room <= 0) return 0;
    int clipped = (qty < room) ? qty : room;
    ob_market_sell_opt(ob, acc, clipped, /*is_player=*/1);
    return clipped;
}

int ob_num_levels(const L3OrderBook *ob, int is_bid) {
    return (int)(is_bid ? ob->bids.size() : ob->asks.size());
}

int ob_level_price(const L3OrderBook *ob, int is_bid, int level_index) {
    const PriceLevel *lvl = level_at(ob, is_bid, level_index);
    return lvl ? lvl->price : -1;
}

long ob_level_qty(const L3OrderBook *ob, int is_bid, int level_index) {
    const PriceLevel *lvl = level_at(ob, is_bid, level_index);
    return lvl ? lvl->total_qty : -1;
}

int ob_level_order_count(const L3OrderBook *ob, int is_bid, int level_index) {
    const PriceLevel *lvl = level_at(ob, is_bid, level_index);
    return lvl ? (int)lvl->queue.size() : -1;
}

int ob_level_order_at(const L3OrderBook *ob, int is_bid, int level_index, int slot,
                       int *out_order_id, int *out_qty, int *out_is_mine) {
    const PriceLevel *lvl = level_at(ob, is_bid, level_index);
    if (!lvl || slot < 0 || slot >= (int)lvl->queue.size()) return 0;
    const Order &o = lvl->queue[slot];
    *out_order_id = o.order_id;
    *out_qty = o.qty;
    *out_is_mine = o.is_mine;
    return 1;
}

int ob_books_equal(const L3OrderBook *a, const L3OrderBook *b) {
    // std::map::operator== / std::deque::operator== are each a single
    // linear pass (using the Order/PriceLevel operator== above) -- this
    // is O(total orders), not the O(levels^2) a caller would get looping
    // the per-index accessors above to do the same comparison by hand.
    return (a->bids == b->bids) && (a->asks == b->asks);
}

long ob_qty_at_price(const L3OrderBook *ob, int is_bid, int price) {
    if (is_bid) {
        auto it = ob->bids.find(price);
        return it == ob->bids.end() ? 0 : it->second.total_qty;
    }
    auto it = ob->asks.find(price);
    return it == ob->asks.end() ? 0 : it->second.total_qty;
}

int ob_dump_levels(const L3OrderBook *ob, int is_bid, int max_levels, int *out_prices, long *out_qtys) {
    int n = 0;
    if (is_bid) {
        for (auto it = ob->bids.begin(); it != ob->bids.end() && n < max_levels; ++it, ++n) {
            out_prices[n] = it->first;
            out_qtys[n] = it->second.total_qty;
        }
    } else {
        for (auto it = ob->asks.begin(); it != ob->asks.end() && n < max_levels; ++it, ++n) {
            out_prices[n] = it->first;
            out_qtys[n] = it->second.total_qty;
        }
    }
    return n;
}
