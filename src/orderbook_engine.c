// ============================================================================
// orderbook_engine.c
//
// Two implementations of the same matching-engine operations:
//   *_baseline  -> line-for-line port of the logic in the original main.c
//   *_opt       -> optimized version, same external behavior
//
// The point is to be able to A/B benchmark them on identical inputs and get
// real numbers instead of guessing.
// ============================================================================
#include "orderbook_engine.h"
#include <string.h>

// ============================================================================
// BASELINE — ported directly from main.c
// ============================================================================

void ob_clean_ghosts_baseline(L3PriceLevel *lvl) {
    int w = 0;
    for (int r = 0; r < lvl->order_count; r++) {
        if (lvl->queue[r].state != 2 && lvl->queue[r].qty > 0) {
            lvl->queue[w++] = lvl->queue[r];
        }
    }
    lvl->order_count = w;
}

void ob_update_total_qty_baseline(L3OrderBook *ob) {
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        int sb = 0, sa = 0;
        for (int q = 0; q < ob->bids[i].order_count; q++) {
            if (ob->bids[i].queue[q].state != 2) sb += ob->bids[i].queue[q].qty;
        }
        ob->bids[i].total_qty = sb;

        for (int q = 0; q < ob->asks[i].order_count; q++) {
            if (ob->asks[i].queue[q].state != 2) sa += ob->asks[i].queue[q].qty;
        }
        ob->asks[i].total_qty = sa;
    }
}

void ob_market_buy_baseline(L3OrderBook *ob, EngineAccount *acc, int qty, int is_player) {
    for (int i = 0; i < MAX_PRICE_LEVELS && qty > 0; i++) {
        for (int q = 0; q < ob->asks[i].order_count && qty > 0; q++) {
            L3Order *ord = &ob->asks[i].queue[q];
            if (ord->qty <= 0 || ord->state == 2) continue;
            if (is_player && ord->is_mine) continue;

            int fill = (qty < ord->qty) ? qty : ord->qty;
            int prev = ord->qty;

            ord->qty -= fill;
            qty -= fill;

            // is_player: the player's own aggressive buy (self-trade already
            // excluded above, so this only ever hits a non-mine ask here) -
            // inventory up, cash down, the ordinary "I bought" direction.
            // !is_player && ord->is_mine: noise flow filled OUR resting ask
            // instead - WE are the seller in that trade, so the direction
            // must flip (inventory down, cash up), not repeat the buy
            // direction. This is the fix for a real bug: both branches used
            // to apply the same sign, which was only ever exercised (and
            // only ever correct) for the is_player=1 case, since main.c
            // never had a way to set is_mine=1 on a resting order at all -
            // the moment the extracted engine's limit-order lifecycle made
            // that reachable, this became a silent direction error that
            // baseline and opt shared identically, so their mutual
            // equivalence tests could never have caught it (see README).
            if (is_player || ord->is_mine) {
                int sign = is_player ? 1 : -1;
                acc->my_inventory += sign * fill;
                acc->my_cash -= sign * (long)fill * ob->asks[i].price;
                acc->total_fill_volume += fill;
                acc->window_trade_qty += fill;
            }

            if (ord->qty == 0) {
                ord->ghost_qty = prev;
                ord->state = 2;
            }
        }
    }
}

void ob_market_sell_baseline(L3OrderBook *ob, EngineAccount *acc, int qty, int is_player) {
    for (int i = 0; i < MAX_PRICE_LEVELS && qty > 0; i++) {
        for (int q = 0; q < ob->bids[i].order_count && qty > 0; q++) {
            L3Order *ord = &ob->bids[i].queue[q];
            if (ord->qty <= 0 || ord->state == 2) continue;
            if (is_player && ord->is_mine) continue;

            int fill = (qty < ord->qty) ? qty : ord->qty;
            int prev = ord->qty;

            ord->qty -= fill;
            qty -= fill;

            // Mirror of ob_market_buy_baseline's fix above: is_player's own
            // sell keeps the original direction (inventory down, cash up);
            // noise flow filling OUR resting bid means WE are the buyer in
            // that trade, so it must flip (inventory up, cash down).
            if (is_player || ord->is_mine) {
                int sign = is_player ? 1 : -1;
                acc->my_inventory -= sign * fill;
                acc->my_cash += sign * (long)fill * ob->bids[i].price;
                acc->total_fill_volume += fill;
                acc->window_trade_qty += fill;
            }

            if (ord->qty == 0) {
                ord->ghost_qty = prev;
                ord->state = 2;
            }
        }
    }
}

void ob_init_baseline(L3OrderBook *ob, EngineAccount *acc, int base_price) {
    memset(ob, 0, sizeof(*ob));
    acc->my_cash = INITIAL_CAPITAL;
    acc->my_inventory = 0;
    acc->current_total_assets = INITIAL_CAPITAL;
    acc->global_order_id = 1000;
    acc->total_fill_volume = 0;
    acc->window_trade_qty = 0;

    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        ob->bids[i].price = base_price - 1 - i;
        ob->bids[i].order_count = 3;
        ob->bids[i].queue[0] = (L3Order){acc->global_order_id++, 40, 0, 0, 0};
        ob->bids[i].queue[1] = (L3Order){acc->global_order_id++, 80, 0, 0, 0};
        ob->bids[i].queue[2] = (L3Order){acc->global_order_id++, 60, 0, 0, 0};

        ob->asks[i].price = base_price + 1 + i;
        ob->asks[i].order_count = 2;
        ob->asks[i].queue[0] = (L3Order){acc->global_order_id++, 50, 0, 0, 0};
        ob->asks[i].queue[1] = (L3Order){acc->global_order_id++, 100, 0, 0, 0};
    }
    ob_update_total_qty_baseline(ob);
}

// ============================================================================
// OPTIMIZED
//
// What changed, and why each change is safe / behavior-preserving:
//
// 1. ob_clean_ghosts_opt:
//    Baseline compacts the queue with a two-pointer scan EVERY call, even
//    when there is nothing to remove (the common case: most ticks, most
//    levels have zero ghost orders). We add a cheap "dirty" fast-path: scan
//    once to check whether any ghost/zero-qty entries exist at all; if not,
//    return immediately without touching the array. This turns the common
//    "nothing to clean" case from an O(n) copy loop into an O(n) read-only
//    scan with zero writes, which matters because clean_ghosts_baseline is
//    called on every price level on every tick regardless of whether
//    anything filled.
//
// 2. ob_update_total_qty_opt:
//    Baseline recomputes total_qty by re-summing every order in every level
//    on every tick (a full O(orders) reduction), even though total_qty is
//    already incrementally maintained by every fill/cancel path. We instead
//    maintain total_qty incrementally at the point of mutation (in
//    ob_market_buy_opt / ob_market_sell_opt) and make ob_update_total_qty_opt
//    a no-op recomputation only used defensively/for verification. This is
//    the single biggest win: it removes a full linear rescan from the hot
//    per-tick path.
//
// 3. ob_market_buy_opt / ob_market_sell_opt:
//    Same matching semantics as baseline (price-time priority within a
//    level, skip ghosted/empty orders), but update total_qty incrementally
//    instead of relying on a separate full-book rescan afterward, and use
//    branch layout that keeps the common "no fill" skip-checks together to
//    reduce mispredicts in the tight inner loop.
//
// Everything below still uses ONLY static memory, no malloc, matching the
// bare-metal constraint.
// ============================================================================

void ob_update_total_qty_opt(L3OrderBook *ob) {
    // Kept for parity/verification (e.g. can be called to re-sync from
    // scratch), but NOT called on the per-tick hot path anymore — the hot
    // path maintains total_qty incrementally in the match functions below.
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        int sb = 0, sa = 0;
        for (int q = 0; q < ob->bids[i].order_count; q++) {
            if (ob->bids[i].queue[q].state != 2) sb += ob->bids[i].queue[q].qty;
        }
        ob->bids[i].total_qty = sb;
        for (int q = 0; q < ob->asks[i].order_count; q++) {
            if (ob->asks[i].queue[q].state != 2) sa += ob->asks[i].queue[q].qty;
        }
        ob->asks[i].total_qty = sa;
    }
}

void ob_clean_ghosts_opt(L3PriceLevel *lvl) {
    // Fast path: read-only scan. If nothing needs removing, skip the
    // compaction entirely (no writes to lvl->queue at all).
    int needs_cleanup = 0;
    for (int r = 0; r < lvl->order_count; r++) {
        if (lvl->queue[r].state == 2 || lvl->queue[r].qty <= 0) {
            needs_cleanup = 1;
            break;
        }
    }
    if (!needs_cleanup) return;

    int w = 0;
    for (int r = 0; r < lvl->order_count; r++) {
        if (lvl->queue[r].state != 2 && lvl->queue[r].qty > 0) {
            lvl->queue[w++] = lvl->queue[r];
        }
    }
    lvl->order_count = w;
}

void ob_market_buy_opt(L3OrderBook *ob, EngineAccount *acc, int qty, int is_player) {
    for (int i = 0; i < MAX_PRICE_LEVELS && qty > 0; i++) {
        L3PriceLevel *lvl = &ob->asks[i];
        int filled_here = 0;
        for (int q = 0; q < lvl->order_count && qty > 0; q++) {
            L3Order *ord = &lvl->queue[q];
            if (ord->qty <= 0 || ord->state == 2 || (is_player && ord->is_mine)) continue;

            int fill = (qty < ord->qty) ? qty : ord->qty;
            int prev = ord->qty;

            ord->qty -= fill;
            qty -= fill;
            filled_here += fill;

            // See ob_market_buy_baseline for why this needs a sign flip
            // between the two cases now, not the same direction for both.
            if (is_player || ord->is_mine) {
                int sign = is_player ? 1 : -1;
                acc->my_inventory += sign * fill;
                acc->my_cash -= sign * (long)fill * lvl->price;
                acc->total_fill_volume += fill;
                acc->window_trade_qty += fill;
            }

            if (ord->qty == 0) {
                ord->ghost_qty = prev;
                ord->state = 2;
            }
        }
        // Incremental maintenance instead of a full O(orders) rescan later.
        lvl->total_qty -= filled_here;
    }
}

void ob_market_sell_opt(L3OrderBook *ob, EngineAccount *acc, int qty, int is_player) {
    for (int i = 0; i < MAX_PRICE_LEVELS && qty > 0; i++) {
        L3PriceLevel *lvl = &ob->bids[i];
        int filled_here = 0;
        for (int q = 0; q < lvl->order_count && qty > 0; q++) {
            L3Order *ord = &lvl->queue[q];
            if (ord->qty <= 0 || ord->state == 2 || (is_player && ord->is_mine)) continue;

            int fill = (qty < ord->qty) ? qty : ord->qty;
            int prev = ord->qty;

            ord->qty -= fill;
            qty -= fill;
            filled_here += fill;

            // See ob_market_sell_baseline for why this needs a sign flip.
            if (is_player || ord->is_mine) {
                int sign = is_player ? 1 : -1;
                acc->my_inventory -= sign * fill;
                acc->my_cash += sign * (long)fill * lvl->price;
                acc->total_fill_volume += fill;
                acc->window_trade_qty += fill;
            }

            if (ord->qty == 0) {
                ord->ghost_qty = prev;
                ord->state = 2;
            }
        }
        lvl->total_qty -= filled_here;
    }
}

// ----------------------------------------------------------------------
// BUG FOUND DURING HARDENING (fixed here, not present in main.c):
//
// main.c never inserts into lvl->queue without first checking
// `order_count < MAX_ORDERS_PER_LVL` at the call site (see process_tick).
// When the matching logic was extracted into this standalone function,
// that check was dropped — ob_add_order_opt used to write unconditionally
// to lvl->queue[lvl->order_count++]. Since queue[] is a fixed-size array
// (L3Order queue[MAX_ORDERS_PER_LVL]) embedded directly in L3PriceLevel,
// a level that was already at capacity and received one more insert would
// write past the end of queue[] into whatever struct field follows it —
// silent memory corruption, not a crash, and exactly the kind of bug a
// correctness harness can miss if its synthetic workload never happens to
// fill a level to capacity.
//
// The fix: don't rely on every caller to remember an external bounds
// check (main.c did, but it's an easy invariant to lose the moment this
// function is called from a second place). Make the function itself
// refuse an insert into a full level and report that via return value.
// See tests/test_correctness.c for the regression test that fills a
// level to MAX_ORDERS_PER_LVL and asserts the next insert is rejected
// under -fsanitize=address,undefined.
// ----------------------------------------------------------------------
int ob_add_order_opt(L3PriceLevel *lvl, L3Order order) {
    if (lvl->order_count >= MAX_ORDERS_PER_LVL) return 0;
    lvl->queue[lvl->order_count++] = order;
    lvl->total_qty += order.qty; // incremental: keep total_qty in sync at insertion time
    return 1;
}

int ob_add_order_baseline(L3PriceLevel *lvl, L3Order order) {
    if (lvl->order_count >= MAX_ORDERS_PER_LVL) return 0;
    lvl->queue[lvl->order_count++] = order;
    return 1;
}

void ob_init_opt(L3OrderBook *ob, EngineAccount *acc, int base_price) {
    memset(ob, 0, sizeof(*ob));
    acc->my_cash = INITIAL_CAPITAL;
    acc->my_inventory = 0;
    acc->current_total_assets = INITIAL_CAPITAL;
    acc->global_order_id = 1000;
    acc->total_fill_volume = 0;
    acc->window_trade_qty = 0;

    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        ob->bids[i].price = base_price - 1 - i;
        ob->bids[i].order_count = 3;
        ob->bids[i].queue[0] = (L3Order){acc->global_order_id++, 40, 0, 0, 0};
        ob->bids[i].queue[1] = (L3Order){acc->global_order_id++, 80, 0, 0, 0};
        ob->bids[i].queue[2] = (L3Order){acc->global_order_id++, 60, 0, 0, 0};

        ob->asks[i].price = base_price + 1 + i;
        ob->asks[i].order_count = 2;
        ob->asks[i].queue[0] = (L3Order){acc->global_order_id++, 50, 0, 0, 0};
        ob->asks[i].queue[1] = (L3Order){acc->global_order_id++, 100, 0, 0, 0};
    }
    ob_update_total_qty_opt(ob); // one-time full computation at init is fine
}

// ============================================================================
// RESTING PLAYER LIMIT ORDERS — completes the is_mine lifecycle that
// main.c checks but never populates (see header comment for the full
// story). Not a port of anything in main.c; a deliberate, clearly-scoped
// extension of the extracted engine.
// ============================================================================

int ob_place_limit_buy_baseline(L3OrderBook *ob, EngineAccount *acc, int level, int qty) {
    if (level < 0 || level >= MAX_PRICE_LEVELS || qty <= 0) return -1;
    int id = acc->global_order_id;
    L3Order order = {id, qty, 0, /*is_mine=*/1, 0};
    if (!ob_add_order_baseline(&ob->bids[level], order)) return -1;
    acc->global_order_id++;
    acc->my_cash -= (long)qty * ob->bids[level].price; // reserved, same as main.c's cancel refund implies
    return id;
}

int ob_place_limit_buy_opt(L3OrderBook *ob, EngineAccount *acc, int level, int qty) {
    if (level < 0 || level >= MAX_PRICE_LEVELS || qty <= 0) return -1;
    int id = acc->global_order_id;
    L3Order order = {id, qty, 0, /*is_mine=*/1, 0};
    if (!ob_add_order_opt(&ob->bids[level], order)) return -1; // total_qty updated incrementally inside
    acc->global_order_id++;
    acc->my_cash -= (long)qty * ob->bids[level].price;
    return id;
}

int ob_place_limit_sell_baseline(L3OrderBook *ob, EngineAccount *acc, int level, int qty) {
    if (level < 0 || level >= MAX_PRICE_LEVELS || qty <= 0) return -1;
    if (qty > acc->my_inventory) return -1; // can't sell inventory you don't have
    int id = acc->global_order_id;
    L3Order order = {id, qty, 0, /*is_mine=*/1, 0};
    if (!ob_add_order_baseline(&ob->asks[level], order)) return -1;
    acc->global_order_id++;
    acc->my_inventory -= qty;
    return id;
}

int ob_place_limit_sell_opt(L3OrderBook *ob, EngineAccount *acc, int level, int qty) {
    if (level < 0 || level >= MAX_PRICE_LEVELS || qty <= 0) return -1;
    if (qty > acc->my_inventory) return -1;
    int id = acc->global_order_id;
    L3Order order = {id, qty, 0, /*is_mine=*/1, 0};
    if (!ob_add_order_opt(&ob->asks[level], order)) return -1;
    acc->global_order_id++;
    acc->my_inventory -= qty;
    return id;
}

// Shared linear scan: both bids and asks, all levels, for a live (not yet
// ghosted/filled) is_mine order with this id. See header for why this is
// deliberately O(orders) rather than indexed.
static L3Order *find_live_mine_order(L3OrderBook *ob, int order_id, int *out_is_bid, int *out_level) {
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        for (int q = 0; q < ob->bids[i].order_count; q++) {
            L3Order *o = &ob->bids[i].queue[q];
            if (o->order_id == order_id && o->is_mine && o->qty > 0 && o->state != 2) {
                *out_is_bid = 1; *out_level = i; return o;
            }
        }
        for (int q = 0; q < ob->asks[i].order_count; q++) {
            L3Order *o = &ob->asks[i].queue[q];
            if (o->order_id == order_id && o->is_mine && o->qty > 0 && o->state != 2) {
                *out_is_bid = 0; *out_level = i; return o;
            }
        }
    }
    return NULL;
}

int ob_cancel_order_baseline(L3OrderBook *ob, EngineAccount *acc, int order_id) {
    int is_bid, level;
    L3Order *o = find_live_mine_order(ob, order_id, &is_bid, &level);
    if (!o) return 0;

    int qty = o->qty;
    o->ghost_qty = qty;
    o->qty = 0;
    o->state = 2; // same ghost mechanism a full fill uses; ob_clean_ghosts_* compacts it out later

    if (is_bid) acc->my_cash += (long)qty * ob->bids[level].price;
    else        acc->my_inventory += qty;
    return 1;
}

int ob_cancel_order_opt(L3OrderBook *ob, EngineAccount *acc, int order_id) {
    int is_bid, level;
    L3Order *o = find_live_mine_order(ob, order_id, &is_bid, &level);
    if (!o) return 0;

    int qty = o->qty;
    o->ghost_qty = qty;
    o->qty = 0;
    o->state = 2;

    if (is_bid) {
        acc->my_cash += (long)qty * ob->bids[level].price;
        ob->bids[level].total_qty -= qty; // incremental, matches ob_market_*_opt's pattern
    } else {
        acc->my_inventory += qty;
        ob->asks[level].total_qty -= qty;
    }
    return 1;
}

// ============================================================================
// CANCEL-REPLACE (order modification). See header for why quantity changes
// and price/level changes are two different functions with two different
// queue-priority behaviors, not one "modify" call.
// ============================================================================
int ob_modify_qty_baseline(L3OrderBook *ob, EngineAccount *acc, int order_id, int new_qty) {
    if (new_qty <= 0) return 0;
    int is_bid, level;
    L3Order *o = find_live_mine_order(ob, order_id, &is_bid, &level);
    if (!o) return 0;

    int delta = new_qty - o->qty;
    if (is_bid) {
        long cash_delta = (long)delta * ob->bids[level].price;
        if (delta > 0 && cash_delta > acc->my_cash) return 0; // can't cover the increase
        acc->my_cash -= cash_delta;
    } else {
        if (delta > 0 && delta > acc->my_inventory) return 0;
        acc->my_inventory -= delta;
    }
    o->qty = new_qty;
    // total_qty stays correct only after the next ob_update_total_qty_baseline()
    // rescan — same as every other baseline mutation site, matching main.c.
    return 1;
}

int ob_modify_qty_opt(L3OrderBook *ob, EngineAccount *acc, int order_id, int new_qty) {
    if (new_qty <= 0) return 0;
    int is_bid, level;
    L3Order *o = find_live_mine_order(ob, order_id, &is_bid, &level);
    if (!o) return 0;

    L3PriceLevel *lvl = is_bid ? &ob->bids[level] : &ob->asks[level];
    int delta = new_qty - o->qty;
    if (is_bid) {
        long cash_delta = (long)delta * lvl->price;
        if (delta > 0 && cash_delta > acc->my_cash) return 0;
        acc->my_cash -= cash_delta;
    } else {
        if (delta > 0 && delta > acc->my_inventory) return 0;
        acc->my_inventory -= delta;
    }
    o->qty = new_qty;
    lvl->total_qty += delta; // incremental, matches every other opt mutation site
    return 1;
}

int ob_modify_price_baseline(L3OrderBook *ob, EngineAccount *acc, int order_id, int new_level, int new_qty) {
    int is_bid, level;
    L3Order *o = find_live_mine_order(ob, order_id, &is_bid, &level);
    if (!o) return -1;

    int new_id = is_bid
        ? ob_place_limit_buy_baseline(ob, acc, new_level, new_qty)
        : ob_place_limit_sell_baseline(ob, acc, new_level, new_qty);
    if (new_id < 0) return -1; // old order left completely untouched

    ob_cancel_order_baseline(ob, acc, order_id); // refund the old reservation
    return new_id;
}

int ob_modify_price_opt(L3OrderBook *ob, EngineAccount *acc, int order_id, int new_level, int new_qty) {
    int is_bid, level;
    L3Order *o = find_live_mine_order(ob, order_id, &is_bid, &level);
    if (!o) return -1;

    int new_id = is_bid
        ? ob_place_limit_buy_opt(ob, acc, new_level, new_qty)
        : ob_place_limit_sell_opt(ob, acc, new_level, new_qty);
    if (new_id < 0) return -1;

    ob_cancel_order_opt(ob, acc, order_id);
    return new_id;
}

// ============================================================================
// PRICE DRIFT. See header for what this is, why it's needed, and the
// honest limitation (an order resting through a drift gets repriced along
// with its level, a direct consequence of the fixed-slot book model).
// ============================================================================
int ob_drift_price_baseline(L3OrderBook *ob, int delta) {
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        if (ob->bids[i].price + delta < MIN_PRICE) return 0;
    }
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        ob->bids[i].price += delta;
        ob->asks[i].price += delta;
    }
    return 1;
}

int ob_drift_price_opt(L3OrderBook *ob, int delta) {
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        if (ob->bids[i].price + delta < MIN_PRICE) return 0;
    }
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        ob->bids[i].price += delta;
        ob->asks[i].price += delta;
    }
    return 1;
}

// ============================================================================
// PRE-TRADE RISK LIMIT — a layer on top of ob_market_buy_*/ob_market_sell_*,
// not a change to them. See header comment for the -168,805 stress-test
// result that motivated this and why it wraps rather than modifies the
// baseline-faithful matching functions.
// ============================================================================

int ob_market_buy_risk_checked_baseline(L3OrderBook *ob, EngineAccount *acc, int qty) {
    int room = MAX_POSITION - acc->my_inventory;
    if (room <= 0) return 0; // already at/over the long limit; no trade, not an error
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
    int room = MAX_POSITION + acc->my_inventory; // symmetric: how far from -MAX_POSITION
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

// ============================================================================
// O(1) CANCEL-BY-ID INDEX — see header for the full design rationale (why
// it's a separate struct instead of extra fields on L3Order/L3PriceLevel,
// why ob_clean_ghosts_opt is left untouched, why the fallback is bounded
// to one level instead of requiring perfect invariants across a shared
// compaction routine).
// ============================================================================

#define ORDER_INDEX_EMPTY     (-1)
#define ORDER_INDEX_TOMBSTONE (-2)

static uint32_t order_index_hash(int32_t order_id) {
    return (uint32_t)order_id * 2654435761u; // Knuth multiplicative hash
}

void ob_index_init(OrderIndex *idx) {
    for (int i = 0; i < ORDER_INDEX_CAPACITY; i++) {
        idx->buckets[i].order_id = ORDER_INDEX_EMPTY;
    }
}

// Linear-probe insert. Stops at the first EMPTY *or* TOMBSTONE bucket, so
// slots freed by order_index_remove get reused — without that, a long
// run of place/cancel pairs (see the randomized stress test) would fill
// the table with tombstones and never free capacity back up.
static void order_index_insert(OrderIndex *idx, int32_t order_id, L3PriceLevel *lvl, int slot, int is_bid) {
    uint32_t h = order_index_hash(order_id) & (ORDER_INDEX_CAPACITY - 1);
    for (int i = 0; i < ORDER_INDEX_CAPACITY; i++) {
        uint32_t b = (h + (uint32_t)i) & (ORDER_INDEX_CAPACITY - 1);
        int32_t cur = idx->buckets[b].order_id;
        if (cur == ORDER_INDEX_EMPTY || cur == ORDER_INDEX_TOMBSTONE) {
            idx->buckets[b].order_id = order_id;
            idx->buckets[b].lvl      = lvl;
            idx->buckets[b].slot     = (uint16_t)slot;
            idx->buckets[b].is_bid   = (uint8_t)is_bid;
            return;
        }
    }
    // Table full: capacity is sized with headroom for every depth this
    // project tests (see header). Unreachable in practice; a silent
    // no-op rather than a crash if it ever is reached — losing the O(1)
    // fast path for one order just means the cancel below won't find it
    // in the index and reports "not found" instead of corrupting memory.
}

// Probing MUST continue past tombstones (only an EMPTY bucket terminates
// the search) — a tombstone means "something used to be here", not "the
// probe chain stops here".
static int order_index_lookup(const OrderIndex *idx, int32_t order_id,
                               L3PriceLevel **out_lvl, int *out_slot, int *out_is_bid) {
    uint32_t h = order_index_hash(order_id) & (ORDER_INDEX_CAPACITY - 1);
    for (int i = 0; i < ORDER_INDEX_CAPACITY; i++) {
        uint32_t b = (h + (uint32_t)i) & (ORDER_INDEX_CAPACITY - 1);
        int32_t cur = idx->buckets[b].order_id;
        if (cur == ORDER_INDEX_EMPTY) return 0; // never inserted
        if (cur == order_id) {
            *out_lvl    = idx->buckets[b].lvl;
            *out_slot   = idx->buckets[b].slot;
            *out_is_bid = idx->buckets[b].is_bid;
            return 1;
        }
        // tombstone or a different id occupying this bucket: keep probing
    }
    return 0;
}

static void order_index_remove(OrderIndex *idx, int32_t order_id) {
    uint32_t h = order_index_hash(order_id) & (ORDER_INDEX_CAPACITY - 1);
    for (int i = 0; i < ORDER_INDEX_CAPACITY; i++) {
        uint32_t b = (h + (uint32_t)i) & (ORDER_INDEX_CAPACITY - 1);
        int32_t cur = idx->buckets[b].order_id;
        if (cur == ORDER_INDEX_EMPTY) return; // not present
        if (cur == order_id) {
            idx->buckets[b].order_id = ORDER_INDEX_TOMBSTONE;
            return;
        }
    }
}

int ob_place_limit_buy_opt_indexed(L3OrderBook *ob, EngineAccount *acc, OrderIndex *idx, int level, int qty) {
    int id = ob_place_limit_buy_opt(ob, acc, level, qty);
    if (id < 0) return -1;
    L3PriceLevel *lvl = &ob->bids[level];
    order_index_insert(idx, id, lvl, lvl->order_count - 1, /*is_bid=*/1);
    return id;
}

int ob_place_limit_sell_opt_indexed(L3OrderBook *ob, EngineAccount *acc, OrderIndex *idx, int level, int qty) {
    int id = ob_place_limit_sell_opt(ob, acc, level, qty);
    if (id < 0) return -1;
    L3PriceLevel *lvl = &ob->asks[level];
    order_index_insert(idx, id, lvl, lvl->order_count - 1, /*is_bid=*/0);
    return id;
}

// Shared by ob_cancel_order_opt_indexed and the indexed modify functions
// below: looks up order_id via the index, validates the cached slot, and
// falls back to a bounded single-level rescan if the cache went stale
// (see the header comment on OrderIndex for exactly when/why that
// happens). Drops a stale index entry itself when the order can't be
// found at all, so every caller gets that cleanup for free instead of
// repeating it. Returns NULL (nothing found) or the live order, with
// *out_lvl/*out_is_bid set to its level/side.
static L3Order *find_indexed_live_order(OrderIndex *idx, int order_id, L3PriceLevel **out_lvl, int *out_is_bid) {
    L3PriceLevel *lvl = NULL;
    int slot = -1, is_bid = 0;
    if (!order_index_lookup(idx, order_id, &lvl, &slot, &is_bid)) return NULL;

    L3Order *o = NULL;
    if (slot >= 0 && slot < lvl->order_count &&
        lvl->queue[slot].order_id == order_id && lvl->queue[slot].is_mine &&
        lvl->queue[slot].qty > 0 && lvl->queue[slot].state != 2) {
        o = &lvl->queue[slot]; // O(1) fast path: cached slot still valid
    } else {
        // Cache stale — a compaction moved this order since it was placed
        // or last found. Bounded fallback: rescan only this ONE level
        // (see header comment), never the rest of the book.
        for (int q = 0; q < lvl->order_count; q++) {
            if (lvl->queue[q].order_id == order_id && lvl->queue[q].is_mine &&
                lvl->queue[q].qty > 0 && lvl->queue[q].state != 2) {
                o = &lvl->queue[q];
                break;
            }
        }
    }

    if (!o) {
        order_index_remove(idx, order_id); // already gone; drop the stale entry
        return NULL;
    }
    *out_lvl = lvl;
    *out_is_bid = is_bid;
    return o;
}

int ob_cancel_order_opt_indexed(L3OrderBook *ob, EngineAccount *acc, OrderIndex *idx, int order_id) {
    (void)ob; // the cached level pointer carries everything needed; kept as a
              // parameter only for calling-convention symmetry with ob_cancel_order_opt
    L3PriceLevel *lvl = NULL;
    int is_bid = 0;
    L3Order *o = find_indexed_live_order(idx, order_id, &lvl, &is_bid);
    if (!o) return 0;

    int qty = o->qty;
    o->ghost_qty = qty;
    o->qty = 0;
    o->state = 2;

    if (is_bid) acc->my_cash += (long)qty * lvl->price;
    else        acc->my_inventory += qty;
    lvl->total_qty -= qty;

    order_index_remove(idx, order_id);
    return 1;
}

int ob_modify_qty_opt_indexed(L3OrderBook *ob, EngineAccount *acc, OrderIndex *idx, int order_id, int new_qty) {
    (void)ob;
    if (new_qty <= 0) return 0;
    L3PriceLevel *lvl = NULL;
    int is_bid = 0;
    L3Order *o = find_indexed_live_order(idx, order_id, &lvl, &is_bid);
    if (!o) return 0;

    int delta = new_qty - o->qty;
    if (is_bid) {
        long cash_delta = (long)delta * lvl->price;
        if (delta > 0 && cash_delta > acc->my_cash) return 0;
        acc->my_cash -= cash_delta;
    } else {
        if (delta > 0 && delta > acc->my_inventory) return 0;
        acc->my_inventory -= delta;
    }
    o->qty = new_qty;
    lvl->total_qty += delta;
    return 1;
}

int ob_modify_price_opt_indexed(L3OrderBook *ob, EngineAccount *acc, OrderIndex *idx, int order_id, int new_level, int new_qty) {
    L3PriceLevel *lvl = NULL;
    int is_bid = 0;
    L3Order *o = find_indexed_live_order(idx, order_id, &lvl, &is_bid);
    if (!o) return -1;

    int new_id = is_bid
        ? ob_place_limit_buy_opt_indexed(ob, acc, idx, new_level, new_qty)
        : ob_place_limit_sell_opt_indexed(ob, acc, idx, new_level, new_qty);
    if (new_id < 0) return -1;

    ob_cancel_order_opt_indexed(ob, acc, idx, order_id);
    return new_id;
}
