// ============================================================================
// lobster_replay.c
//
// Replays a real NASDAQ order-flow stream (LOBSTER: https://lobsterdata.com)
// through this engine and cross-checks the resulting book state against
// LOBSTER's own independently-reconstructed order-book snapshots --
// external ground truth, not the self-consistency memcmp/ob_books_equal
// tests/test_correctness.c already does between baseline and opt.
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
// Real, unfixable data-completeness gap this file can't paper over: a
// LOBSTER message file starts at market open, not from a genuinely
// empty book -- the opening auction leaves resting orders that were
// submitted before this file's first row. The first several thousand
// messages can reference an order_id this replay never saw created.
// `seen` tracks which ids this file actually placed via a Type 1, so a
// reference to anything else is recognized as pre-window liquidity
// (counted separately, comparison skipped for that row) instead of
// being misreported as a translation bug.
//
// ----------------------------------------------------------------------
// Order translation:
//   Type 1 -> ob_add_order_*(ob, is_bid, price, order_id, size, is_mine=0)
//             directly -- anonymous third-party liquidity, same as the
//             market-maker/noise injection this engine already supports.
//   Type 2 -> ob_reduce_order_qty_any_*(ob, OrderID, Size)
//   Type 3 -> ob_cancel_order_any_*(ob, OrderID)
//   Type 4/5 -> LOBSTER logs these against the RESTING order being hit;
//             the aggressor isn't separately logged. Consecutive 4/5
//             rows sharing the same timestamp and Direction are one
//             aggressor sweep -- summed and applied as a single
//             ob_market_buy_opt/sell_opt(..., is_player=0) call (the
//             direction opposite the resting side), same is_player=0/
//             is_mine=0 "noise-vs-noise" fill path the fill-direction
//             bugfix (commit 030721f) touched, so this replay doubles
//             as a real-data regression check of that fix. The ground-
//             truth comparison is skipped for every row inside a sweep
//             except the last (LOBSTER's book only reflects the FULL
//             sweep once all its rows are applied; ours does too, once
//             flushed).
//   Type 7 -> logged and skipped (trading halt, not a book event).
//
// Drives baseline and opt through the identical translated stream in
// parallel, comparing them (ob_books_equal) at every synced row exactly
// like the existing differential harness -- extends that net to real
// data for free.
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "orderbook_engine.h"
#include "lobster_format.h"

#define LOB_MAX_FILE_LEVELS 64 // defensive cap on parsing the orderbook file's columns

typedef struct {
    long ask_price[LOB_MAX_FILE_LEVELS], ask_size[LOB_MAX_FILE_LEVELS];
    long bid_price[LOB_MAX_FILE_LEVELS], bid_size[LOB_MAX_FILE_LEVELS];
    int  n_levels;
    int  valid;
} LobBookRow;

static int read_orderbook_row(FILE *f, LobBookRow *r) {
    char line[4096];
    if (!fgets(line, sizeof(line), f)) { r->valid = 0; return 0; }
    long vals[4 * LOB_MAX_FILE_LEVELS];
    int n = 0;
    char *p = line;
    while (n < 4 * LOB_MAX_FILE_LEVELS) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        vals[n++] = v;
        p = end;
        while (*p == ',') p++;
    }
    r->n_levels = n / 4;
    for (int k = 0; k < r->n_levels; k++) {
        r->ask_price[k] = vals[4 * k + 0];
        r->ask_size[k]  = vals[4 * k + 1];
        r->bid_price[k] = vals[4 * k + 2];
        r->bid_size[k]  = vals[4 * k + 3];
    }
    r->valid = (r->n_levels > 0);
    return r->valid;
}

// A LOBSTER-filled level's price is a sentinel/dummy (no real order at
// that depth) once the real book runs out before this file's configured
// level count -- guaranteed identifiable by a volume of 0 per LOBSTER's
// own documentation. Treat non-positive or implausibly large prices as
// sentinels too, defensively.
static int is_sentinel(long price, long size) {
    return size == 0 || price <= 0 || price > 100000000000L;
}

// Checks our own best price against LOBSTER's, then every level LOBSTER
// actually reported via a direct price lookup -- no reconstruction
// needed now that both sides only ever represent populated levels (see
// file header). Returns 1 if everything checkable matched, 0 with `why`
// set on the first mismatch.
static int compare_side(const L3OrderBook *ob, int is_bid, const long *lob_price,
                         const long *lob_size, int lob_n, char *why, size_t whylen) {
    if (lob_n == 0 || is_sentinel(lob_price[0], lob_size[0])) return 1; // nothing to check this row
    int our_best = ob_level_price(ob, is_bid, 0);
    if (our_best != (int)lob_price[0]) {
        snprintf(why, whylen, "%s best price mismatch: engine=%d lobster=%ld",
                 is_bid ? "bid" : "ask", our_best, lob_price[0]);
        return 0;
    }
    for (int k = 0; k < lob_n; k++) {
        if (is_sentinel(lob_price[k], lob_size[k])) break;
        long our_qty = ob_qty_at_price(ob, is_bid, (int)lob_price[k]);
        if (our_qty != lob_size[k]) {
            snprintf(why, whylen, "%s qty mismatch at price %ld: engine=%ld lobster=%ld",
                     is_bid ? "bid" : "ask", lob_price[k], our_qty, lob_size[k]);
            return 0;
        }
    }
    return 1;
}

typedef struct {
    long total_messages;
    long type_count[8]; // indexed 1..7
    long unknown_order_ref;
    long compared_rows;
    long mismatches;
    long baseline_opt_divergences;
} Stats;

static void print_stats(const Stats *s) {
    printf("\n---- lobster_replay summary ----\n");
    printf("total messages:            %ld\n", s->total_messages);
    printf("  type 1 (submission):     %ld\n", s->type_count[1]);
    printf("  type 2 (partial cancel): %ld\n", s->type_count[2]);
    printf("  type 3 (deletion):       %ld\n", s->type_count[3]);
    printf("  type 4 (exec visible):   %ld\n", s->type_count[4]);
    printf("  type 5 (exec hidden):    %ld\n", s->type_count[5]);
    printf("  type 7 (halt):           %ld\n", s->type_count[7]);
    printf("refs to pre-window orders: %ld (liquidity from before this file's first row -- can't be validated, see README)\n",
           s->unknown_order_ref);
    printf("ground-truth comparisons:  %ld\n", s->compared_rows);
    printf("ground-truth mismatches:   %ld\n", s->mismatches);
    printf("baseline/opt divergences:  %ld\n", s->baseline_opt_divergences);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <message.csv> <orderbook.csv> [max_mismatches=5]\n", argv[0]);
        return 2;
    }
    int max_mismatches = (argc > 3) ? (int)strtol(argv[3], NULL, 10) : 5;

    FILE *msg_f = fopen(argv[1], "r");
    FILE *book_f = fopen(argv[2], "r");
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
    memset(&acc_base, 0, sizeof(acc_base));
    memset(&acc_opt, 0, sizeof(acc_opt));
    acc_base.my_cash = acc_opt.my_cash = INITIAL_CAPITAL;

    DroppedSet seen; // ids actually placed via a Type 1 here -- see file header
    dropped_init(&seen);
    Stats stats;
    memset(&stats, 0, sizeof(stats));

    // One-message lookahead so a Type 4/5 aggressor sweep spanning
    // several consecutive rows only gets applied (and ground-truth
    // checked) once, on its last row -- see file header.
    LobMsg cur, next;
    int have_next = read_message(msg_f, &next);

    long pending_qty = 0;
    int pending_direction = 0;
    int pending_active = 0;
    int pending_has_unknown = 0;

    int mismatches_reported = 0;

    while (have_next) {
        cur = next;
        have_next = read_message(msg_f, &next);
        stats.total_messages++;
        if (cur.type >= 1 && cur.type <= 7) stats.type_count[cur.type]++;

        LobBookRow row;
        int have_row = read_orderbook_row(book_f, &row);

        int is_bid = (cur.direction == 1);
        int do_compare = 1; // most rows are synced immediately

        switch (cur.type) {
        case 1:
            ob_add_order_baseline(ob_base, is_bid, (int)cur.price, (int)cur.order_id, (int)cur.size, 0);
            ob_add_order_opt(ob_opt, is_bid, (int)cur.price, (int)cur.order_id, (int)cur.size, 0);
            dropped_add(&seen, (int32_t)cur.order_id);
            break;
        case 2:
            if (!dropped_contains(&seen, (int32_t)cur.order_id)) {
                stats.unknown_order_ref++;
                do_compare = 0;
            } else {
                ob_reduce_order_qty_any_baseline(ob_base, (int)cur.order_id, (int)cur.size);
                ob_reduce_order_qty_any_opt(ob_opt, (int)cur.order_id, (int)cur.size);
            }
            break;
        case 3:
            if (!dropped_contains(&seen, (int32_t)cur.order_id)) {
                stats.unknown_order_ref++;
                do_compare = 0;
            } else {
                ob_cancel_order_any_baseline(ob_base, (int)cur.order_id);
                ob_cancel_order_any_opt(ob_opt, (int)cur.order_id);
            }
            break;
        case 4:
        case 5: {
            // order_id 0 marks a hidden order (Type 5) -- the exchange
            // never reveals its id, that's not evidence of missing state.
            if (cur.order_id != 0 && !dropped_contains(&seen, (int32_t)cur.order_id)) {
                pending_has_unknown = 1;
            }
            if (!pending_active) {
                pending_active = 1;
                pending_direction = cur.direction;
                pending_qty = 0;
            }
            pending_qty += cur.size;
            int this_is_last = !(have_next && (next.type == 4 || next.type == 5) &&
                                  next.time_ns == cur.time_ns && next.direction == cur.direction);
            if (!this_is_last) {
                do_compare = 0; // engine hasn't caught up to this row yet
            } else {
                if (pending_direction == 1) { // resting side was a bid -> aggressor sold
                    ob_market_sell_baseline(ob_base, &acc_base, (int)pending_qty, /*is_player=*/0);
                    ob_market_sell_opt(ob_opt, &acc_opt, (int)pending_qty, /*is_player=*/0);
                } else { // resting side was an ask -> aggressor bought
                    ob_market_buy_baseline(ob_base, &acc_base, (int)pending_qty, /*is_player=*/0);
                    ob_market_buy_opt(ob_opt, &acc_opt, (int)pending_qty, /*is_player=*/0);
                }
                pending_active = 0;
                pending_qty = 0;
                if (pending_has_unknown) {
                    stats.unknown_order_ref++;
                    do_compare = 0;
                    pending_has_unknown = 0;
                }
            }
            break;
        }
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

        if (do_compare && have_row) {
            stats.compared_rows++;
            char why[128];
            int bid_ok = compare_side(ob_opt, 1, row.bid_price, row.bid_size, row.n_levels, why, sizeof(why));
            int ask_ok = bid_ok ? compare_side(ob_opt, 0, row.ask_price, row.ask_size, row.n_levels, why, sizeof(why))
                                 : 0;
            if (!bid_ok || !ask_ok) {
                stats.mismatches++;
                if (mismatches_reported < max_mismatches) {
                    printf("GROUND-TRUTH MISMATCH at message %ld (type %d, order_id %ld): %s\n",
                           stats.total_messages, cur.type, cur.order_id, why);
                    mismatches_reported++;
                }
                if (mismatches_reported >= max_mismatches) {
                    fprintf(stderr, "aborting: too many ground-truth mismatches\n");
                    break;
                }
            }
        }

        if (stats.total_messages % 100000 == 0) {
            fprintf(stderr, "...%ld messages processed\n", stats.total_messages);
        }
    }

    print_stats(&stats);
    fclose(msg_f);
    fclose(book_f);
    ob_destroy(ob_base);
    ob_destroy(ob_opt);
    return (stats.mismatches == 0 && stats.baseline_opt_divergences == 0) ? 0 : 1;
}
