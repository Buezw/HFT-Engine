// ============================================================================
// lobster_replay.cpp
//
// Replays a real NASDAQ order-flow stream (LOBSTER: https://lobsterdata.com)
// through this engine and cross-checks the resulting book state against
// LOBSTER's own independently-reconstructed order-book snapshots --
// external ground truth, not the self-consistency memcmp/ob_books_equal
// tests/test_correctness.cpp already does between baseline and opt.
//
// This got a LOT simpler after the C++ rewrite (see orderbook_engine.h):
// the old fixed-array engine only tracked a handful of price *ticks*
// around the current best, which meant this file had to convert LOBSTER's
// absolute prices into tick offsets, decide whether to drift the whole
// ladder, and reconstruct LOBSTER's sparse "populated levels" into a
// dense per-tick view before comparing (see git history if curious what
// that looked like). None of that exists anymore -- the engine now
// tracks a price level exactly when a real order rests there, same as
// LOBSTER's own representation, so every LOBSTER price is just inserted
// at that exact price, and ground truth is a direct ob_qty_at_price()
// lookup, not a reconstruction.
//
// LOBSTER publishes a paired message file + orderbook file, one line
// each per event, guaranteed line-for-line synchronized (message line
// N's effect is exactly what orderbook line N shows). Message columns
// (no header row): Time, Type, OrderID, Size, Price, Direction.
// Type: 1=submission, 2=partial cancellation, 3=total deletion,
//       4=execution (visible), 5=execution (hidden), 7=trading halt.
// Direction: which side the AFFECTED (resting) order sits on -- 1=bid
// side, -1=ask side. Price is dollars * 10000 -- inserted into the
// engine as-is, no scaling needed (see above). Orderbook columns (no
// header), per level: AskPrice,AskSize,BidPrice,BidSize, repeated for
// however many levels the file has (inferred at runtime from the line).
// These conventions are corroborated across multiple independent
// academic sources (this sandbox couldn't reach LOBSTER's own docs
// directly -- it's a JS-rendered SPA) -- cross-check against the README
// that ships in the actual downloaded sample zip before trusting output
// from this tool on real data.
//
// ----------------------------------------------------------------------
// Two data-completeness gaps this file works around rather than fixes.
//
// 1. The file starts at market open, not from an empty book -- the
//    opening auction leaves orders submitted before row 1. `seen` tracks
//    the ids this file actually placed, so a reference to anything else
//    is counted as pre-window liquidity and its row skipped.
//
// 2. A level-N extract only carries events that touch the visible top N
//    levels. Measured on this data: of 550k messages across AAPL/GOOG/
//    AMZN, zero are priced outside the window, and an order's odds of
//    never being cancelled or filled in the file rise straight down the
//    book -- 4% for one submitted at level 1, 38-46% at level 5. Those
//    orders were cancelled while too deep to be reported, so replaying
//    the messages alone rests them forever: AAPL ends the day holding
//    6,268 bids priced above the closing ask, a crossed book that can't
//    exist. Left alone they push the tracked best price off and every
//    later row mismatches (that was 99.9% of rows before this).
//
// So the book is resynced to the visible window after each row is
// compared (resync_side): levels the file can't maintain are dropped,
// qty we can't have seen is filled in. Each comparison then asks "apply
// this one message to a book that matched a row ago -- still right?",
// which is the strongest question this data can answer. A price that
// only just entered the window is skipped, since nothing in the file
// could have told us about it (counted, not hidden).
//
// This does mean ground truth feeds state back in, so the harness is
// mutation-tested rather than trusted: type 4 off by one -> 7,329
// mismatches, 1-in-1000 type 3 dropped -> 53, type 1 qty +1 -> 60,835,
// against 0 for the real thing on all three full days.
//
// ----------------------------------------------------------------------
// Order translation:
//   Type 1 -> ob_add_order_*(ob, is_bid, price, order_id, size, is_mine=0)
//             directly -- anonymous third-party liquidity, same as the
//             market-maker/noise injection this engine already supports.
//   Type 2 -> ob_reduce_order_qty_any_*(ob, OrderID, Size)
//   Type 3 -> ob_cancel_order_any_*(ob, OrderID)
//   Type 4 -> ob_reduce_order_qty_any_*(ob, OrderID, Size), same as Type 2.
//             LOBSTER logs the exact resting order that got hit, so take
//             it off by id. This used to be a market order sweeping our
//             own best price, which only works if our book is already
//             perfect -- one miss (e.g. a fill against pre-window
//             liquidity) and it ate the wrong orders from then on, left
//             the right ones resting, and never recovered (AAPL full
//             day: 99.9% mismatches).
//   Type 5 -> nothing. Hidden order, id 0, never in our book, and the
//             visible book LOBSTER reports doesn't change either.
//   Type 7 -> logged and skipped (trading halt, not a book event).
//
// The orderbook file is synced after EVERY message, including each row
// of a same-timestamp sweep (checked on AAPL: the snapshot changes on
// all 5485 such rows), so every row gets compared -- no sweep grouping.
//
// Drives baseline and opt through the identical translated stream in
// parallel, comparing them (ob_books_equal) at every synced row exactly
// like the existing differential harness -- extends that net to real
// data for free.
// ============================================================================
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include "orderbook_engine.h"
#include "lobster_format.h"

constexpr int LOB_MAX_FILE_LEVELS = 64; // defensive cap on parsing the orderbook file's columns

struct LobBookRow {
    long ask_price[LOB_MAX_FILE_LEVELS], ask_size[LOB_MAX_FILE_LEVELS];
    long bid_price[LOB_MAX_FILE_LEVELS], bid_size[LOB_MAX_FILE_LEVELS];
    int  n_levels;
    bool valid;
};

static bool read_orderbook_row(std::istream &in, LobBookRow &r) {
    std::string line;
    if (!std::getline(in, line)) { r.valid = false; return false; }
    long vals[4 * LOB_MAX_FILE_LEVELS];
    int n = 0;
    const char *p = line.c_str();
    while (n < 4 * LOB_MAX_FILE_LEVELS) {
        char *end;
        long v = std::strtol(p, &end, 10);
        if (end == p) break;
        vals[n++] = v;
        p = end;
        while (*p == ',') p++;
    }
    r.n_levels = n / 4;
    for (int k = 0; k < r.n_levels; k++) {
        r.ask_price[k] = vals[4 * k + 0];
        r.ask_size[k]  = vals[4 * k + 1];
        r.bid_price[k] = vals[4 * k + 2];
        r.bid_size[k]  = vals[4 * k + 3];
    }
    r.valid = (r.n_levels > 0);
    return r.valid;
}

// A LOBSTER-filled level's price is a sentinel/dummy (no real order at
// that depth) once the real book runs out before this file's configured
// level count -- guaranteed identifiable by a volume of 0 per LOBSTER's
// own documentation. Treat non-positive or implausibly large prices as
// sentinels too, defensively.
static bool is_sentinel(long price, long size) {
    return size == 0 || price <= 0 || price > 100000000000L;
}

// Checks our own best price against LOBSTER's, then every level LOBSTER
// actually reported via a direct price lookup -- no reconstruction
// needed now that both sides only ever represent populated levels (see
// file header). Returns 1 if everything checkable matched, 0 with `why`
// set on the first mismatch.
// A price only just arrived in LOBSTER's top-5 window was resting outside
// it until now, where the file reports nothing -- we cannot have built it,
// so it isn't ours to get wrong. Checkable = visible on this side a row
// ago too. (*n_skipped counts what that excuses, so it stays visible.)
static bool was_visible(const long *prev_price, const long *prev_size, int prev_n, long price) {
    for (int k = 0; k < prev_n; k++) {
        if (is_sentinel(prev_price[k], prev_size[k])) break;
        if (prev_price[k] == price) return true;
    }
    return false;
}

static bool compare_side(const L3OrderBook *ob, int is_bid, const long *lob_price,
                          const long *lob_size, int lob_n, const long *prev_price,
                          const long *prev_size, int prev_n, long &n_skipped,
                          long &n_checked, std::string &why) {
    char buf[128];
    if (lob_n == 0 || is_sentinel(lob_price[0], lob_size[0])) return true; // nothing to check this row
    int our_best = ob_level_price(ob, is_bid, 0);
    if (our_best != (int)lob_price[0]) {
        if (!was_visible(prev_price, prev_size, prev_n, lob_price[0])) { n_skipped++; return true; }
        snprintf(buf, sizeof(buf), "%s best price mismatch: engine=%d lobster=%ld",
                 is_bid ? "bid" : "ask", our_best, lob_price[0]);
        why = buf;
        return false;
    }
    for (int k = 0; k < lob_n; k++) {
        if (is_sentinel(lob_price[k], lob_size[k])) break;
        if (!was_visible(prev_price, prev_size, prev_n, lob_price[k])) { n_skipped++; continue; }
        n_checked++;
        long our_qty = ob_qty_at_price(ob, is_bid, (int)lob_price[k]);
        if (our_qty != lob_size[k]) {
            snprintf(buf, sizeof(buf), "%s qty mismatch at price %ld: engine=%ld lobster=%ld",
                     is_bid ? "bid" : "ask", lob_price[k], our_qty, lob_size[k]);
            why = buf;
            return false;
        }
    }
    return true;
}

struct Stats {
    long total_messages;
    long type_count[8]; // indexed 1..7
    long unknown_order_ref;
    long out_of_window_ref;
    long exec_rejected;
    long pruned_orders;
    long filler_qty;
    long excess_qty;
    long compared_rows;
    long levels_skipped_new;
    long levels_checked;
    long mismatches;
    long baseline_opt_divergences;
};

static void print_stats(const Stats &s) {
    printf("\n---- lobster_replay summary ----\n");
    printf("total messages:            %ld\n", s.total_messages);
    printf("  type 1 (submission):     %ld\n", s.type_count[1]);
    printf("  type 2 (partial cancel): %ld\n", s.type_count[2]);
    printf("  type 3 (deletion):       %ld\n", s.type_count[3]);
    printf("  type 4 (exec visible):   %ld\n", s.type_count[4]);
    printf("  type 5 (exec hidden):    %ld\n", s.type_count[5]);
    printf("  type 7 (halt):           %ld\n", s.type_count[7]);
    printf("refs to pre-window orders: %ld (liquidity from before this file's first row -- can't be validated, see README)\n",
           s.unknown_order_ref);
    printf("refs to out-of-window ids: %ld (order had left LOBSTER's top-5 window, see header)\n",
           s.out_of_window_ref);
    printf("exec rejected by engine:   %ld (type 4 on a known id that wasn't resting or had less qty)\n",
           s.exec_rejected);
    printf("window resync: %ld orders pruned, %ld qty filled in, %ld qty trimmed\n",
           s.pruned_orders, s.filler_qty, s.excess_qty);
    printf("ground-truth comparisons:  %ld\n", s.compared_rows);
    printf("  levels checked: %ld, skipped as new to window: %ld\n",
           s.levels_checked, s.levels_skipped_new);
    printf("ground-truth mismatches:   %ld\n", s.mismatches);
    printf("baseline/opt divergences:  %ld\n", s.baseline_opt_divergences);
}

// ----------------------------------------------------------------------
// Window resync. The level-5 message file only carries events that touch
// the visible top 5 levels (checked: 0 of 550k messages across AAPL/GOOG/
// AMZN price outside it), so an order that drifts deeper gets cancelled
// off-file and we'd rest it forever. Everything outside the window is
// therefore unmaintainable by construction: drop it, and top up the qty
// LOBSTER shows that we can't have seen submitted. Runs AFTER the row is
// compared, so each comparison asks "apply this one message to a book
// that matched a row ago -- still right?" instead of measuring how far
// the drift has accumulated.
// Filler orders carry negative ids; nothing in the file can reference one.
// ----------------------------------------------------------------------
static int next_filler_id = -1;

static void drop_order(L3OrderBook *base, L3OrderBook *opt, int id, OrderIdSet &pruned, Stats &st) {
    ob_cancel_order_any_baseline(base, id);
    ob_cancel_order_any_opt(opt, id);
    if (id > 0) pruned.insert(static_cast<int32_t>(id));
    st.pruned_orders++;
}

// clears every order resting at level_index (counted from the best)
static void drop_level(L3OrderBook *base, L3OrderBook *opt, int is_bid, int level_index,
                        OrderIdSet &pruned, Stats &st) {
    int id, qty, mine;
    while (ob_level_order_at(opt, is_bid, level_index, 0, &id, &qty, &mine)) {
        drop_order(base, opt, id, pruned, st);
    }
}

static int level_index_of(const L3OrderBook *ob, int is_bid, int price) {
    int n = ob_num_levels(ob, is_bid);
    for (int i = 0; i < n; i++) if (ob_level_price(ob, is_bid, i) == price) return i;
    return -1;
}

static bool price_visible(const long *vp, int m, int price) {
    for (int k = 0; k < m; k++) if (vp[k] == price) return true;
    return false;
}

static void resync_side(L3OrderBook *base, L3OrderBook *opt, int is_bid, const long *px,
                         const long *sz, int n_levels, OrderIdSet &pruned, Stats &st) {
    long vp[LOB_MAX_FILE_LEVELS], vq[LOB_MAX_FILE_LEVELS];
    int m = 0;
    for (int k = 0; k < n_levels; k++) {
        if (is_sentinel(px[k], sz[k])) break;
        vp[m] = px[k]; vq[m] = sz[k]; m++;
    }
    if (m == 0) return; // no visible window on this side -- nothing to bound our book with

    // Our levels should be exactly the visible ones. Deeper than level 5 is
    // the obvious case, but a price BETWEEN two visible levels counts too:
    // LOBSTER not listing it means nothing rests there, and the event that
    // emptied it is missing for the same reason. Deepest-first, so dropping
    // a level doesn't shift an index we still have to look at.
    for (int i = ob_num_levels(opt, is_bid) - 1; i >= 0; i--) {
        if (!price_visible(vp, m, ob_level_price(opt, is_bid, i))) {
            drop_level(base, opt, is_bid, i, pruned, st);
        }
    }

    for (int k = 0; k < m; k++) {
        long ours = ob_qty_at_price(opt, is_bid, (int)vp[k]);
        long diff = vq[k] - ours;
        if (diff > 0) { // liquidity that entered while this price was out of the window
            ob_add_order_baseline(base, is_bid, (int)vp[k], next_filler_id, (int)diff, 0);
            ob_add_order_opt(opt, is_bid, (int)vp[k], next_filler_id, (int)diff, 0);
            next_filler_id--;
            st.filler_qty += diff;
        } else if (diff < 0) { // resting qty LOBSTER no longer shows -- trim from the back
            long excess = -diff;
            int li = level_index_of(opt, is_bid, (int)vp[k]);
            while (excess > 0 && li >= 0) {
                int slot = ob_level_order_count(opt, is_bid, li) - 1;
                if (slot < 0) break;
                int id, qty, mine;
                if (!ob_level_order_at(opt, is_bid, li, slot, &id, &qty, &mine)) break;
                if (qty <= excess) {
                    drop_order(base, opt, id, pruned, st);
                    excess -= qty;
                } else {
                    ob_reduce_order_qty_any_baseline(base, id, (int)excess);
                    ob_reduce_order_qty_any_opt(opt, id, (int)excess);
                    if (id > 0) pruned.insert(static_cast<int32_t>(id)); // file's qty for it no longer matches ours
                    excess = 0;
                }
                li = level_index_of(opt, is_bid, (int)vp[k]);
            }
            st.excess_qty += -diff;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <message.csv> <orderbook.csv> [max_mismatches=5]\n", argv[0]);
        return 2;
    }
    int max_mismatches = (argc > 3) ? static_cast<int>(std::strtol(argv[3], nullptr, 10)) : 5;

    std::ifstream msg_f(argv[1]);
    std::ifstream book_f(argv[2]);
    if (!msg_f || !book_f) {
        fprintf(stderr, "could not open input files\n");
        return 2;
    }

    L3OrderBook *ob_base = ob_create(), *ob_opt = ob_create();
    EngineAccount acc_base, acc_opt;
    ob_init_baseline(ob_base, &acc_base, 1);
    ob_init_opt(ob_opt, &acc_opt, 1);
    // Real replay starts from an empty book, not ob_init_*'s synthetic
    // placeholder market-maker quotes -- destroy and recreate rather
    // than seed-then-clear.
    ob_destroy(ob_base); ob_destroy(ob_opt);
    ob_base = ob_create(); ob_opt = ob_create();

    OrderIdSet seen;   // ids actually placed via a Type 1 here -- see file header
    OrderIdSet pruned; // ids we dropped at the window edge
    Stats stats{};

    LobMsg cur;
    LobBookRow prev_row;
    bool have_prev_row = false;
    int mismatches_reported = 0;

    while (read_message(msg_f, cur)) {
        stats.total_messages++;
        if (cur.type >= 1 && cur.type <= 7) stats.type_count[cur.type]++;

        LobBookRow row;
        bool have_row = read_orderbook_row(book_f, row);
        bool comparable = have_row && have_prev_row;

        int is_bid = (cur.direction == 1);
        bool do_compare = true; // most rows are synced immediately

        switch (cur.type) {
        case 1:
            ob_add_order_baseline(ob_base, is_bid, (int)cur.price, (int)cur.order_id, (int)cur.size, 0);
            ob_add_order_opt(ob_opt, is_bid, (int)cur.price, (int)cur.order_id, (int)cur.size, 0);
            seen.insert(static_cast<int32_t>(cur.order_id));
            break;
        case 2:
            if (!seen.count(static_cast<int32_t>(cur.order_id))) {
                stats.unknown_order_ref++;
                do_compare = 0;
            } else if (pruned.count(static_cast<int32_t>(cur.order_id))) {
                stats.out_of_window_ref++;
                do_compare = 0;
            } else {
                ob_reduce_order_qty_any_baseline(ob_base, (int)cur.order_id, (int)cur.size);
                ob_reduce_order_qty_any_opt(ob_opt, (int)cur.order_id, (int)cur.size);
            }
            break;
        case 3:
            if (!seen.count(static_cast<int32_t>(cur.order_id))) {
                stats.unknown_order_ref++;
                do_compare = 0;
            } else if (pruned.count(static_cast<int32_t>(cur.order_id))) {
                stats.out_of_window_ref++;
                do_compare = 0;
            } else {
                ob_cancel_order_any_baseline(ob_base, (int)cur.order_id);
                ob_cancel_order_any_opt(ob_opt, (int)cur.order_id);
            }
            break;
        case 4:
            if (!seen.count(static_cast<int32_t>(cur.order_id))) {
                stats.unknown_order_ref++;
                do_compare = 0;
            } else if (pruned.count(static_cast<int32_t>(cur.order_id))) {
                stats.out_of_window_ref++;
                do_compare = 0;
            } else {
                int ok_b = ob_reduce_order_qty_any_baseline(ob_base, (int)cur.order_id, (int)cur.size);
                int ok_o = ob_reduce_order_qty_any_opt(ob_opt, (int)cur.order_id, (int)cur.size);
                if (!ok_b || !ok_o) {
                    stats.exec_rejected++;
                    if (mismatches_reported < max_mismatches) {
                        printf("EXEC REJECTED at message %ld (order_id %ld, size %ld)\n",
                               stats.total_messages, cur.order_id, cur.size);
                        mismatches_reported++;
                    }
                }
            }
            break;
        case 5:
            break; // hidden order -- nothing of ours to touch, visible book unchanged
        case 7:
        default:
            do_compare = 0; // halt marker or unrecognized type -- not a book event
            break;
        }

        if (!ob_books_equal(ob_base, ob_opt)) {
            stats.baseline_opt_divergences++;
            if (mismatches_reported < max_mismatches) {
                printf("BASELINE/OPT DIVERGENCE at message %ld (type %d, order_id %ld)\n",
                       stats.total_messages, cur.type, cur.order_id);
                mismatches_reported++;
            }
            if (mismatches_reported >= max_mismatches) {
                fprintf(stderr, "aborting: too many baseline/opt divergences\n");
                break;
            }
        }

        if (do_compare && comparable) {
            stats.compared_rows++;
            std::string why;
            bool bid_ok = compare_side(ob_opt, 1, row.bid_price, row.bid_size, row.n_levels,
                                        prev_row.bid_price, prev_row.bid_size, prev_row.n_levels,
                                        stats.levels_skipped_new, stats.levels_checked, why);
            bool ask_ok = bid_ok && compare_side(ob_opt, 0, row.ask_price, row.ask_size, row.n_levels,
                                                  prev_row.ask_price, prev_row.ask_size, prev_row.n_levels,
                                                  stats.levels_skipped_new, stats.levels_checked, why);
            if (!bid_ok || !ask_ok) {
                stats.mismatches++;
                if (mismatches_reported < max_mismatches) {
                    printf("GROUND-TRUTH MISMATCH at message %ld (type %d, order_id %ld): %s\n",
                           stats.total_messages, cur.type, cur.order_id, why.c_str());
                    mismatches_reported++;
                }
                if (mismatches_reported >= max_mismatches) {
                    fprintf(stderr, "aborting: too many ground-truth mismatches\n");
                    break;
                }
            }
        }

        if (have_row) {
            prev_row = row;
            have_prev_row = true;
            resync_side(ob_base, ob_opt, 1, row.bid_price, row.bid_size, row.n_levels, pruned, stats);
            resync_side(ob_base, ob_opt, 0, row.ask_price, row.ask_size, row.n_levels, pruned, stats);
        }

        if (stats.total_messages % 100000 == 0) {
            fprintf(stderr, "...%ld messages processed\n", stats.total_messages);
        }
    }

    print_stats(stats);
    ob_destroy(ob_base);
    ob_destroy(ob_opt);
    return (stats.mismatches == 0 && stats.exec_rejected == 0 && stats.baseline_opt_divergences == 0) ? 0 : 1;
}
