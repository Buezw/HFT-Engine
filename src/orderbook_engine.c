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
        if (lvl->queue[r].visual_fx != 2 && lvl->queue[r].qty > 0) {
            lvl->queue[w++] = lvl->queue[r];
        }
    }
    lvl->order_count = w;
}

void ob_update_total_qty_baseline(L3OrderBook *ob) {
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        int sb = 0, sa = 0;
        for (int q = 0; q < ob->bids[i].order_count; q++) {
            if (ob->bids[i].queue[q].visual_fx != 2) sb += ob->bids[i].queue[q].qty;
        }
        ob->bids[i].total_qty = sb;

        for (int q = 0; q < ob->asks[i].order_count; q++) {
            if (ob->asks[i].queue[q].visual_fx != 2) sa += ob->asks[i].queue[q].qty;
        }
        ob->asks[i].total_qty = sa;
    }
}

void ob_market_buy_baseline(L3OrderBook *ob, EngineAccount *acc, int qty, int is_player) {
    for (int i = 0; i < MAX_PRICE_LEVELS && qty > 0; i++) {
        for (int q = 0; q < ob->asks[i].order_count && qty > 0; q++) {
            L3Order *ord = &ob->asks[i].queue[q];
            if (ord->qty <= 0 || ord->visual_fx == 2) continue;
            if (is_player && ord->is_mine) continue;

            int fill = (qty < ord->qty) ? qty : ord->qty;
            int prev = ord->qty;

            ord->qty -= fill;
            qty -= fill;

            if (is_player || ord->is_mine) {
                acc->my_inventory += fill;
                acc->my_cash -= (long)fill * ob->asks[i].price;
                acc->total_fill_volume += fill;
                acc->window_trade_qty += fill;
            }

            if (ord->qty == 0) {
                ord->ghost_qty = prev;
                ord->visual_fx = 2;
            }
        }
    }
}

void ob_market_sell_baseline(L3OrderBook *ob, EngineAccount *acc, int qty, int is_player) {
    for (int i = 0; i < MAX_PRICE_LEVELS && qty > 0; i++) {
        for (int q = 0; q < ob->bids[i].order_count && qty > 0; q++) {
            L3Order *ord = &ob->bids[i].queue[q];
            if (ord->qty <= 0 || ord->visual_fx == 2) continue;
            if (is_player && ord->is_mine) continue;

            int fill = (qty < ord->qty) ? qty : ord->qty;
            int prev = ord->qty;

            ord->qty -= fill;
            qty -= fill;

            if (is_player || ord->is_mine) {
                acc->my_inventory -= fill;
                acc->my_cash += (long)fill * ob->bids[i].price;
                acc->total_fill_volume += fill;
                acc->window_trade_qty += fill;
            }

            if (ord->qty == 0) {
                ord->ghost_qty = prev;
                ord->visual_fx = 2;
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
            if (ob->bids[i].queue[q].visual_fx != 2) sb += ob->bids[i].queue[q].qty;
        }
        ob->bids[i].total_qty = sb;
        for (int q = 0; q < ob->asks[i].order_count; q++) {
            if (ob->asks[i].queue[q].visual_fx != 2) sa += ob->asks[i].queue[q].qty;
        }
        ob->asks[i].total_qty = sa;
    }
}

void ob_clean_ghosts_opt(L3PriceLevel *lvl) {
    // Fast path: read-only scan. If nothing needs removing, skip the
    // compaction entirely (no writes to lvl->queue at all).
    int needs_cleanup = 0;
    for (int r = 0; r < lvl->order_count; r++) {
        if (lvl->queue[r].visual_fx == 2 || lvl->queue[r].qty <= 0) {
            needs_cleanup = 1;
            break;
        }
    }
    if (!needs_cleanup) return;

    int w = 0;
    for (int r = 0; r < lvl->order_count; r++) {
        if (lvl->queue[r].visual_fx != 2 && lvl->queue[r].qty > 0) {
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
            if (ord->qty <= 0 || ord->visual_fx == 2 || (is_player && ord->is_mine)) continue;

            int fill = (qty < ord->qty) ? qty : ord->qty;
            int prev = ord->qty;

            ord->qty -= fill;
            qty -= fill;
            filled_here += fill;

            if (is_player || ord->is_mine) {
                acc->my_inventory += fill;
                acc->my_cash -= (long)fill * lvl->price;
                acc->total_fill_volume += fill;
                acc->window_trade_qty += fill;
            }

            if (ord->qty == 0) {
                ord->ghost_qty = prev;
                ord->visual_fx = 2;
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
            if (ord->qty <= 0 || ord->visual_fx == 2 || (is_player && ord->is_mine)) continue;

            int fill = (qty < ord->qty) ? qty : ord->qty;
            int prev = ord->qty;

            ord->qty -= fill;
            qty -= fill;
            filled_here += fill;

            if (is_player || ord->is_mine) {
                acc->my_inventory -= fill;
                acc->my_cash += (long)fill * lvl->price;
                acc->total_fill_volume += fill;
                acc->window_trade_qty += fill;
            }

            if (ord->qty == 0) {
                ord->ghost_qty = prev;
                ord->visual_fx = 2;
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
