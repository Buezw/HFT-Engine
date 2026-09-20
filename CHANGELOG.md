# Changelog

Newest first. Commits have the full diffs; this is what changed and why.

## 2026-09-19 — Everything's C++ now

- tests/, bench/, tools/ went from C to C++17, and `extern "C"` is gone.
  Same `ob_*` names and signatures.
- `spsc_ring` is now a header-only `SpscRing` over `std::atomic`, same
  acquire/release pairing. `src/spsc_ring.c` is deleted.
- pthread became `std::thread` (affinity still uses
  `pthread_setaffinity_np`), `clock_gettime` became `steady_clock`, and
  malloc/qsort became vector/sort.
- LOBSTER's fixed-size `DroppedSet` became `std::unordered_set`, so
  there's no capacity left to overflow.
- `test_correctness`, `book_trace` and `lobster_replay` output is
  byte-identical before and after, on the fixture and on the full real
  days.

## 2026-09-19 — LOBSTER replay: two tool bugs, then the real problem

- Type 4 (execution) used to be replayed as a market order against our
  own best price. That's only right if the book is already perfect.
  One miss and it eats the wrong orders forever: AAPL order 56951361
  was fully executed at 10:07 and still resting at the close. It's a
  reduce-by-id now, same as Type 2.
- Same-timestamp sweeps were only compared on the last row, but LOBSTER
  syncs its book after every row. Now every row is compared.
- Neither fix moved the mismatch rate. The real cause was the data: a
  level-5 extract only has events touching the top 5 levels, and odds
  an order is never removed in the file go from ~4% (submitted at level
  1) to 38–46% (level 5). Those orders got cancelled while too deep to
  report. Replaying messages alone leaves them resting forever, so AAPL
  ended the day with 6,268 bids above the closing ask. An independent
  Python rebuild got the same counts, so it wasn't the engine.
- Fix: resync to the visible window after comparing each row. Result is
  0 mismatches on AAPL/GOOG/AMZN full days (510k rows). Since ground
  truth now feeds back in, the harness was mutation-tested: an
  off-by-one in Type 4 gives 7,329 mismatches, dropping 1/1000 Type 3s
  gives 53, Type 1 qty +1 gives 60,835.
- `DroppedSet` was 65,536 slots. A full AAPL day has 143k order ids and
  it was silently dropping inserts. It got bigger and aborts when full,
  and was later replaced outright (see above).

## 2026-09-16 — Resting-order double charge

- When someone else filled your resting limit order, the fill touched
  both legs, including the one `place_limit_*` had already reserved. So
  cash (buy) or inventory (sell) was charged twice. Now only the
  unreserved leg settles.
- Baseline and opt had the same bug, so the differential tests never
  noticed. There's a direct test for it now.

## 2026-09-15 — C++ rewrite: dynamic book

- Real data broke the fixed-array design. Once the top tracked level
  drained, the engine couldn't find the next real price because it had
  never recorded anything outside its window. Over the GOOG day, best
  bid was stuck at the $579.40 open while the market went to $565.12.
- Rewrote the engine: `std::map` per side, `std::deque` per level, no
  `MAX_PRICE_LEVELS` / `MAX_ORDERS_PER_LVL`. `L3OrderBook` became
  opaque, with accessors.
- Same GOOG day afterwards: best bid follows the market to $565.10.
- What went away because a dynamic book doesn't need it: price drift,
  ghost orders + `clean_ghosts`, `update_total_qty` (now kept
  incrementally everywhere), and the separate `OrderIndex` /
  `_opt_indexed` API (opt has the index built in).
- baseline vs opt is redefined. Both use the same book and the same
  matching. Only cancel/modify-by-id differs: full scan vs index. The
  plain tick loop now makes opt ~0.7x slower (index upkeep with nothing
  using it), while the depth sweep goes past 400x at 2k orders.
- `memcmp` equality became `ob_books_equal`, a semantic comparison.

## 2026-09-15 — LOBSTER real-data validation

- `lobster_replay` replays NASDAQ order flow through both engines and
  checks against LOBSTER's own reconstructed book. `lobster_bench` times
  the same calls under real flow.
- Added `ob_cancel_order_any_*` / `ob_reduce_order_qty_any_*`. Real
  cancels are almost always someone else's orders, and the `is_mine`-only
  functions couldn't touch those.
- This is what exposed the fixed-window problem above.

## 2026-09-05 — Price drift, fill-direction bug

- Added `ob_drift_price_*`. Watching the visualizer showed prices never
  moved at all, only quantities, so a market maker had no adverse
  selection to deal with. (Removed on 09-15: a fixed ladder can't drift
  bid and ask independently without corrupting one side.)
- Fill direction bug: noise flow filling your resting ask credited you
  as if you'd bought. The same sign was used for "player aggresses" and
  "someone fills player's resting order". Baseline and opt agreed on the
  wrong answer. Caught by a position-limit test in the downstream
  Market-Maker-Strategy repo, not here.
- It also explains the -168,805 inventory the 08-30 stress test
  reported. That number was partly this bug.

## 2026-09-04 — Cancel-replace, SPSC ring, capability map

- `ob_modify_qty_*` changes qty in place and keeps priority.
  `ob_modify_price_*` is cancel + re-place: new id, back of the queue.
  It places first and cancels second, so a rejected move leaves the
  original alone. The cost is that both reservations are held for a
  moment.
- Lock-free SPSC ring and `threaded_bench`. Splitting receive from match
  didn't help p50/p99. Pinning cores gave a ~12.5 ms max outlier on
  this shared box. Clean under TSan.
- `tools/capability_map.html`, a plain-language function map. Stale
  since the rewrite.

## 2026-09-03 — Cancel-by-id index, visualizer

- O(1) cancel via an open-addressing index caching (level, slot), with
  a one-level rescan fallback when a compaction had moved the order.
  Kept separate from the book structs because of the `memcmp` test and
  the benchmark's function-pointer contract. At depth 400: 1139 ns scan
  vs 22 ns indexed. (Folded into opt on 09-15.)
- `make visualize`: a 300-tick scenario dumped as JSON and rendered into
  a self-contained HTML replay.

## 2026-08-30 — Pre-trade risk limit

- The 100k stress test drove inventory to -168,805 with nothing
  stopping it. (Partly a different bug, see 09-05.)
- Added `ob_market_*_risk_checked_*`, a wrapper that clips qty to stay
  within `MAX_POSITION` (500). It clips rather than rejects. It's a
  wrapper so the matching functions stay untouched.

## 2026-08-29 — Initial extraction

- Pulled the matching core out of `board/main.c` into a testable
  engine, with a baseline (faithful port) and an opt version.
- Optimizations: `clean_ghosts` checks before compacting, and
  `total_qty` is maintained incrementally instead of re-summed every
  tick. 1.2–1.4x at board depth, ~2.5x at 40x depth.
- `main.c` bug: `vga_draw_rect_fast(x + w - 1, 1, h, color)` had 4 args
  with y hardcoded. Should be `(x + w - 1, y, 1, h, color)`.
- Extraction bug: `ob_add_order_opt` lost the caller-side capacity
  check `main.c` had, and wrote past `queue[]` when a level was full.
  200k ticks of replay never hit it; ASan did. The check now lives
  inside the function.
- Completed resting limit orders. `main.c` checks `is_mine` in 8 places
  but never sets it. Added `ob_place_limit_*` (reserves on placement)
  and `ob_cancel_order_*` (refunds).
- Added `make depth-sweep`, CI, and the MIT license.
