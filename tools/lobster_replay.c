// ============================================================================
// lobster_replay.c
//
// Replays a real NASDAQ order-flow stream (LOBSTER: https://lobsterdata.com)
// through this engine and cross-checks the resulting book state against
// LOBSTER's own independently-reconstructed order-book snapshots — external
// ground truth, not the self-consistency memcmp tests/test_correctness.c
// already does between baseline and opt.
//
// LOBSTER publishes a paired message file + orderbook file, one line each
// per event, guaranteed line-for-line synchronized (message line N's effect
// is exactly what orderbook line N shows). Message columns (no header row):
//   Time, Type, OrderID, Size, Price, Direction
// Type: 1=submission, 2=partial cancellation, 3=total deletion,
//       4=execution (visible), 5=execution (hidden), 7=trading halt.
// Direction: which side the AFFECTED (resting) order sits on — 1=bid
// side, -1=ask side. Price is dollars * 10000. Orderbook columns (no
// header), per level: AskPrice,AskSize,BidPrice,BidSize, repeated for
// however many levels the file has (inferred at runtime from the line,
// not hardcoded here). These conventions are corroborated across multiple
// independent academic sources (this sandbox couldn't reach LOBSTER's own
// docs directly — it's a JS-rendered SPA) — cross-check against the README
// that ships in the actual downloaded sample zip before trusting output
// from this tool on real data, and adjust the two spots marked below if it
// disagrees.
//
// ----------------------------------------------------------------------
// Fixed-slot-model mismatch this tool has to bridge (see README/plan):
// this engine has MAX_PRICE_LEVELS *ticks* per side, always exactly one
// tick apart (ob_init_*/ob_drift_price_* never change that spacing) — not
// MAX_PRICE_LEVELS *populated price levels* the way LOBSTER's orderbook
// file lists them (its "level 2" can be many ticks away from "level 1" if
// the ticks between them are empty). Comparing "engine slot i" directly
// against "LOBSTER column i" would report false mismatches whenever the
// real book has any gap in the compared range. Instead, compare_side()
// below reconstructs a dense per-tick view from LOBSTER's sparse listed
// levels (a tick between two listed prices is genuinely empty in real
// data — LOBSTER only lists a level when at least one order rests there)
// and compares that, tick for tick, against the engine's fixed slots.
//
// ----------------------------------------------------------------------
// Order translation:
//   Type 1 -> if this price improves the current best on its side, drift
//             the whole ladder first (ob_drift_price_*) so it becomes
//             slot 0, then insert via ob_add_order_*(is_mine=0) — this is
//             anonymous third-party liquidity, not "the player"'s own
//             order, same as the market-maker/noise injection this engine
//             already supports. Falls outside the MAX_PRICE_LEVELS window
//             (or the target slot's queue is already full) -> dropped,
//             counted, and remembered so later references to this id are
//             silently ignored instead of miscounted as anomalies.
//   Type 2 -> ob_reduce_order_qty_any_*(ob, OrderID, Size)
//   Type 3 -> ob_cancel_order_any_*(ob, OrderID)
//   Type 4/5 -> LOBSTER logs these against the RESTING order being hit;
//             the aggressor isn't separately logged. Consecutive 4/5 rows
//             sharing the same timestamp and Direction are one aggressor
//             sweep — summed and applied as a single
//             ob_market_buy_opt/sell_opt(..., is_player=0) call (the
//             direction opposite the resting side), same is_player=0/
//             is_mine=0 "noise-vs-noise" fill path the fill-direction
//             bugfix (commit 030721f) touched, so this replay doubles as
//             a real-data regression check of that fix. The ground-truth
//             comparison is skipped for every row inside a sweep except
//             the last (LOBSTER's book only reflects the FULL sweep once
//             all its rows are applied; ours does too, once flushed).
//   Type 7 -> logged and skipped (trading halt, not a book event).
//
// Drives baseline and opt through the identical translated stream in
// parallel, memcmp-comparing them at every synced row exactly like the
// existing differential harness — extends that net to real data for free.
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
    // Plain-C11 comma split (no strtok_r -- this codebase builds with
    // -std=c11, which hides POSIX-only declarations like strtok_r).
    char *p = line;
    while (n < 4 * LOB_MAX_FILE_LEVELS) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) break; // no digits here -- end of the line's numbers
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

// A LOBSTER-filled level's price is a sentinel (no real order at that
// depth) once the real book runs out before this file's configured level
// count. This tool hasn't been run against a real sample yet to confirm
// the exact sentinel value LOBSTER uses on this account's sandbox (the
// site's docs are behind a JS SPA this tool couldn't fetch) -- treat
// anything non-positive or implausibly large as "no real data here"
// rather than assume a specific sentinel constant. Adjust if the
// downloaded sample's own README states a different convention.
static int is_sentinel_price(long price) {
    return price <= 0 || price > 100000000000L;
}

// Reconstructs a dense per-tick view from LOBSTER's sparse listed levels
// on one side and compares it against this engine's fixed per-tick slots.
// See file header for why this can't just compare column i to slot i
// directly. Returns 1 if everything checkable matched, 0 with `why` set
// on the first mismatch.
static int compare_side(const L3PriceLevel *engine_side, const long *lob_price,
                         const long *lob_size, int lob_n, long tick_size, int is_bid,
                         char *why, size_t whylen) {
    if (lob_n == 0 || is_sentinel_price(lob_price[0])) return 1; // nothing to check this row
    long lob_best = lob_price[0];
    long engine_best = (long)engine_side[0].price * tick_size;
    if (engine_best != lob_best) {
        snprintf(why, whylen, "%s best price mismatch: engine=%ld lobster=%ld",
                 is_bid ? "bid" : "ask", engine_best, lob_best);
        return 0;
    }

    long tick_qty[MAX_PRICE_LEVELS];
    for (int t = 0; t < MAX_PRICE_LEVELS; t++) tick_qty[t] = -1; // -1 = not yet known
    int known_up_to = -1;
    for (int k = 0; k < lob_n; k++) {
        if (is_sentinel_price(lob_price[k])) break;
        long ticks = is_bid ? (lob_best - lob_price[k]) / tick_size
                             : (lob_price[k] - lob_best) / tick_size;
        if (ticks < 0) break; // malformed/out-of-order row -- stop trusting this side
        if (ticks >= MAX_PRICE_LEVELS) {
            for (int t = known_up_to + 1; t < MAX_PRICE_LEVELS; t++) tick_qty[t] = 0;
            known_up_to = MAX_PRICE_LEVELS - 1;
            break;
        }
        // every tick strictly between the last populated level and this
        // one is genuinely empty -- LOBSTER only lists a level when at
        // least one order rests there.
        for (int t = known_up_to + 1; t < (int)ticks; t++) tick_qty[t] = 0;
        tick_qty[ticks] = lob_size[k];
        known_up_to = (int)ticks;
    }
    for (int t = 0; t <= known_up_to && t < MAX_PRICE_LEVELS; t++) {
        if (tick_qty[t] < 0) continue; // unreachable given the loop above, kept defensively
        if ((long)engine_side[t].total_qty != tick_qty[t]) {
            snprintf(why, whylen, "%s tick %d qty mismatch: engine=%d lobster=%ld",
                     is_bid ? "bid" : "ask", t, engine_side[t].total_qty, tick_qty[t]);
            return 0;
        }
    }
    return 1;
}

typedef struct {
    long total_messages;
    long type_count[8]; // indexed 1..7
    long out_of_window;
    long queue_full;
    long compared_rows;
    long mismatches;
    long baseline_opt_divergences;
} Stats;

static void print_stats(const Stats *s) {
    printf("\n---- lobster_replay summary (MAX_PRICE_LEVELS=%d) ----\n", MAX_PRICE_LEVELS);
    printf("total messages:            %ld\n", s->total_messages);
    printf("  type 1 (submission):     %ld\n", s->type_count[1]);
    printf("  type 2 (partial cancel): %ld\n", s->type_count[2]);
    printf("  type 3 (deletion):       %ld\n", s->type_count[3]);
    printf("  type 4 (exec visible):   %ld\n", s->type_count[4]);
    printf("  type 5 (exec hidden):    %ld\n", s->type_count[5]);
    printf("  type 7 (halt):           %ld\n", s->type_count[7]);
    printf("dropped, out-of-window:    %ld\n", s->out_of_window);
    printf("dropped, queue full:       %ld\n", s->queue_full);
    printf("ground-truth comparisons:  %ld\n", s->compared_rows);
    printf("ground-truth mismatches:   %ld\n", s->mismatches);
    printf("baseline/opt divergences:  %ld\n", s->baseline_opt_divergences);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <message.csv> <orderbook.csv> [tick_size=100] [max_mismatches=5]\n", argv[0]);
        return 2;
    }
    long tick_size = (argc > 3) ? strtol(argv[3], NULL, 10) : 100; // LOBSTER units; 100 = $0.01
    int max_mismatches = (argc > 4) ? (int)strtol(argv[4], NULL, 10) : 5;

    FILE *msg_f = fopen(argv[1], "r");
    FILE *book_f = fopen(argv[2], "r");
    if (!msg_f || !book_f) {
        fprintf(stderr, "could not open input files\n");
        return 2;
    }

    L3OrderBook ob_base, ob_opt;
    EngineAccount acc_base, acc_opt;
    // base_price is irrelevant here -- the first Type 1 submission that
    // improves on it will immediately drift the ladder to the real
    // market's actual best, exactly like a fresh venue opening.
    ob_init_baseline(&ob_base, &acc_base, 1);
    ob_init_opt(&ob_opt, &acc_opt, 1);
    // Clear the synthetic default liquidity ob_init_* seeds every level
    // with -- real replay should start from an empty book, not
    // main.c's placeholder market-maker quotes.
    memset(&ob_base, 0, sizeof(ob_base));
    memset(&ob_opt, 0, sizeof(ob_opt));
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        ob_base.bids[i].price = -i;    // same formula ob_init_* uses (base_price - 1 - i) at base_price=1
        ob_base.asks[i].price = 2 + i; // same formula ob_init_* uses (base_price + 1 + i) at base_price=1
        ob_opt.bids[i].price  = ob_base.bids[i].price;
        ob_opt.asks[i].price  = ob_base.asks[i].price;
    }

    DroppedSet dropped;
    dropped_init(&dropped);
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
        case 1: {
            L3PriceLevel *bestlvl_opt = is_bid ? &ob_opt.bids[0] : &ob_opt.asks[0];
            long best = bestlvl_opt->price * tick_size;
            long ticks_from_best = is_bid ? (best - cur.price) / tick_size
                                           : (cur.price - best) / tick_size;
            if (ticks_from_best < 0) {
                long delta_ticks = -ticks_from_best;
                int delta = (int)(is_bid ? delta_ticks : -delta_ticks);
                int ok_b = ob_drift_price_baseline(&ob_base, delta);
                int ok_o = ob_drift_price_opt(&ob_opt, delta);
                if (!ok_b || !ok_o) {
                    // Would push a level below MIN_PRICE -- can't represent
                    // this improvement; drop the order like any other
                    // out-of-window case rather than desync the ladder.
                    dropped_add(&dropped, (int32_t)cur.order_id);
                    stats.out_of_window++;
                    break;
                }
                ticks_from_best = 0;
            }
            if (ticks_from_best >= MAX_PRICE_LEVELS) {
                dropped_add(&dropped, (int32_t)cur.order_id);
                stats.out_of_window++;
                break;
            }
            int level = (int)ticks_from_best;
            L3Order o = {(int)cur.order_id, (int)cur.size, 0, /*is_mine=*/0, 0};
            L3PriceLevel *lb = is_bid ? &ob_base.bids[level] : &ob_base.asks[level];
            L3PriceLevel *lo = is_bid ? &ob_opt.bids[level]  : &ob_opt.asks[level];
            int ok_b = ob_add_order_baseline(lb, o);
            int ok_o = ob_add_order_opt(lo, o);
            if (!ok_b || !ok_o) {
                dropped_add(&dropped, (int32_t)cur.order_id);
                stats.queue_full++;
            }
            ob_update_total_qty_baseline(&ob_base); // opt already incremental inside ob_add_order_opt
            break;
        }
        case 2:
            if (!dropped_contains(&dropped, (int32_t)cur.order_id)) {
                ob_reduce_order_qty_any_baseline(&ob_base, (int)cur.order_id, (int)cur.size);
                ob_reduce_order_qty_any_opt(&ob_opt, (int)cur.order_id, (int)cur.size);
            }
            break;
        case 3:
            if (!dropped_contains(&dropped, (int32_t)cur.order_id)) {
                ob_cancel_order_any_baseline(&ob_base, (int)cur.order_id);
                ob_cancel_order_any_opt(&ob_opt, (int)cur.order_id);
            }
            break;
        case 4:
        case 5: {
            int continues = pending_active && have_next &&
                             (next.type == 4 || next.type == 5) &&
                             next.time_ns == cur.time_ns && next.direction == cur.direction;
            if (!pending_active) {
                pending_active = 1;
                pending_direction = cur.direction;
                pending_qty = 0;
            }
            pending_qty += cur.size;
            // Also check whether the row we just started matches what's
            // still to come (handles the very first row of a sweep).
            int this_is_last = !(have_next && (next.type == 4 || next.type == 5) &&
                                  next.time_ns == cur.time_ns && next.direction == cur.direction);
            (void)continues;
            if (!this_is_last) {
                do_compare = 0; // engine hasn't caught up to this row yet
            } else {
                if (pending_direction == 1) { // resting side was a bid -> aggressor sold
                    ob_market_sell_baseline(&ob_base, &acc_base, (int)pending_qty, /*is_player=*/0);
                    ob_market_sell_opt(&ob_opt, &acc_opt, (int)pending_qty, /*is_player=*/0);
                } else { // resting side was an ask -> aggressor bought
                    ob_market_buy_baseline(&ob_base, &acc_base, (int)pending_qty, /*is_player=*/0);
                    ob_market_buy_opt(&ob_opt, &acc_opt, (int)pending_qty, /*is_player=*/0);
                }
                pending_active = 0;
                pending_qty = 0;
            }
            break;
        }
        case 7:
        default:
            do_compare = 0; // halt marker or unrecognized type -- not a book event
            break;
        }

        // Ghost compaction so total_qty/order_count-derived reads stay
        // representative, same housekeeping every other driver in this
        // codebase does between operations.
        for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
            ob_clean_ghosts_baseline(&ob_base.bids[i]);
            ob_clean_ghosts_opt(&ob_opt.bids[i]);
            ob_clean_ghosts_baseline(&ob_base.asks[i]);
            ob_clean_ghosts_opt(&ob_opt.asks[i]);
        }
        ob_update_total_qty_baseline(&ob_base);

        if (memcmp(&ob_base, &ob_opt, sizeof(ob_base)) != 0) {
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
            int bid_ok = compare_side(ob_opt.bids, row.bid_price, row.bid_size, row.n_levels,
                                       tick_size, 1, why, sizeof(why));
            int ask_ok = bid_ok ? compare_side(ob_opt.asks, row.ask_price, row.ask_size, row.n_levels,
                                                tick_size, 0, why, sizeof(why))
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
    return (stats.mismatches == 0 && stats.baseline_opt_divergences == 0) ? 0 : 1;
}
