# HFT Order Book Engine — Optimization & Benchmark

CI workflow: [`.github/workflows/ci.yml`](.github/workflows/ci.yml) (runs
`make test`, `make bench`, `make asan`, `make tsan`, `make cppcheck` on
every push/PR — badge omitted until this is pushed to a repo GitHub can
render status for).
License: [MIT](LICENSE).

## What this is

The original `main.c` is a bare-metal RISC-V program (targeting the DE1-SoC
FPGA board) that implements an interactive L3 limit order book simulator:
a matching engine, market-maker/event-injection logic, VGA framebuffer
rendering, and interrupt-driven timing/input — all with static memory only
(no malloc, no OS).

This directory adds a **testable, benchmarkable version of the matching
engine core**, so the performance-critical logic can be validated and
measured on a normal machine instead of only living inside a board-only
build.

The engine itself (`src/orderbook_engine.cpp`) is C++ now, not C — see
"C++ rewrite: a dynamic order book" below for why. Everything else
(`tests/`, `bench/`, `tools/`) stays plain C and links against it through
a C API.

## C++ rewrite: a dynamic order book

For a while this engine kept the bare-metal build's original constraint
even on the desktop port: `L3OrderBook` was a fixed-size array,
`MAX_PRICE_LEVELS` price slots per side, no malloc. Fine for testing
matching logic in isolation. Then real NASDAQ order flow
([LOBSTER](https://lobsterdata.com), see below) got run through it, and
the actual cost of that constraint showed up: once the top tracked price
level's liquidity fully drained, the engine had no way to discover the
real next-best price, because anything outside its narrow window was
never recorded at all. Over one real trading day the tracked best bid
got stuck at the opening price ($579.40) while the real market drifted
down to $565.12 — a $14+ divergence, and it wasn't a rounding error, it
was the engine having genuinely lost track of where the market was.

The fix is the one every real matching engine already uses: don't cap
how many price levels you track. `L3OrderBook` is now backed by
`std::map<price, level>` per side (a level exists exactly when at least
one order rests there — created on insert, erased when the last order
leaves) and `std::deque<Order>` per level, with no `MAX_PRICE_LEVELS` or
`MAX_ORDERS_PER_LVL` anywhere. `L3OrderBook` is opaque now (`ob_create`/
`ob_destroy`, read state through accessors like `ob_level_price`/
`ob_num_levels` instead of struct fields) so the public API in
`include/orderbook_engine.h` stays plain C — `tests/`, `bench/`, and
`tools/` didn't need to become C++ themselves, just update their call
sites.

**What this removed, because a dynamic book doesn't need it:**
- **Price drift** (`ob_drift_price_*`) — existed to relabel fixed slots
  as the market moved. A dynamic book just gets a new map key for a new
  best price; there's nothing to relabel. (This also fixed a subtler bug
  the drift design had: moving bid and ask together, the only option a
  fixed ladder had, corrupts whichever side isn't actually changing the
  moment you try to track them independently — found running real data
  through it, before the full rewrite. See git history for
  `ob_drift_bid_price_*`/`ob_drift_ask_price_*`, the intermediate fix
  that was superseded by removing drift entirely.)
- **Ghost orders + `ob_clean_ghosts_*`** — a filled/cancelled order used
  to get marked `state=2` and linger in its fixed array slot until a
  compaction pass ran. A `std::deque` just erases it immediately.
- **`ob_update_total_qty_*`** — total_qty is maintained incrementally at
  every mutation site now, for both baseline and opt; there's no
  "rescan later" fallback because there's nothing left to rescan.
- **The separate `OrderIndex`/`_opt_indexed` API** — opt keeps an
  `order_id -> price` index as a normal member now, not a second parallel
  data structure bolted on afterward.

**What baseline vs opt means now:** the old split was "faithful port of
main.c's fixed-array logic" vs "optimized." That distinction doesn't
carry over to a dynamic structure — there's no fixed-array version left
to be faithful to. The methodology stays (two independent
implementations, diffed after every operation via `ob_books_equal`, so
an optimization can never silently change behavior), just re-scoped to
what's actually still a live algorithmic choice: **both use the same
dynamic book**; baseline finds an order to cancel/modify by scanning
every order in the book, opt jumps straight to the right price level via
its index. Same idea as the old indexed-vs-linear split, just built into
one implementation instead of two parallel APIs — and now it actually
matters at realistic depth instead of capping out at a few hundred
orders. The depth-sweep speedup used to top out around 2.5x at
`MAX_ORDERS_PER_LVL=400`; run at real depth now (`make depth-sweep`), it
passes 400x by a few thousand orders and 800x by five thousand (see
Results below) — the old number was measuring the shape of the
optimization, not the actual gap.

Confirmed against the same real GOOG trading day that exposed the
original bug: the tracked best bid now follows the market down to
$565.10, two cents off the real $565.12 by end of day — the remaining
gap is the LOBSTER message file starting at market open rather than
from a genuinely empty book (see "LOBSTER real-market-data validation"
below for that data-completeness caveat, which is unrelated to and
predates this rewrite), not a tracking failure.

## Files

```
board/main.c                 original bare-metal program (RISC-V/DE1-SoC only)
include/orderbook_engine.h   public C API + opaque L3OrderBook, EngineAccount
src/orderbook_engine.cpp     C++ implementation: dynamic std::map/std::deque book, baseline + opt
tests/test_correctness.c     200k-tick replay + lifecycle/stress/risk-limit tests (plain C, links against the C++ engine)
bench/benchmark.c            baseline-vs-optimized timing: mean, percentiles, runtime depth sweep
tools/book_trace.c           runs a scenario against the opt engine, dumps per-tick JSON
tools/render_trace.py        wraps the JSON trace into a self-contained HTML replay
tools/visualizer_template.html  the replay page itself (ladder + inventory/PnL/depth charts)
tools/capability_map.html    static reference: every public function, plain-language, no code reading required
include/spsc_ring.h / src/spsc_ring.c   lock-free single-producer/single-consumer queue
tests/test_spsc_ring.c       FIFO/capacity boundary tests + a real 2M-item multithreaded stress test
bench/threaded_bench.c       measures whether splitting receive/match onto two threads helps or hurts
tools/lobster_format.h       shared LOBSTER message-format decoding (used by the two tools below)
tools/lobster_replay.c       replays real order flow, cross-checks book state against LOBSTER ground truth
bench/lobster_bench.c        baseline-vs-opt latency under real (not synthetic) order flow
tests/fixtures/lobster_sample/  hand-built LOBSTER-format fixture, regression-tests the replay tool itself
Makefile                     test / bench / asan / tsan / cppcheck / depth-sweep / visualize / threaded-bench / lobster-test / lobster-bench / clean targets
.github/workflows/ci.yml     runs test / bench / lobster-test / asan / tsan / cppcheck on every push/PR
```

- `board/main.c` — the original bare-metal program, with one real bug fixed
  (see below). This still targets the DE1-SoC and won't compile with a
  normal x86 gcc (it uses RISC-V-specific interrupt attributes and raw
  hardware addresses on purpose). Kept as a historical/reference artifact,
  not part of the buildable project.
- `include/orderbook_engine.h` / `src/orderbook_engine.cpp` — the order
  book data structures and matching logic, extracted into a
  platform-independent, dynamic-depth form (see "C++ rewrite" above),
  with two implementations side by side:
  - `*_baseline` — finds an order to cancel/modify by scanning every
    order in the book
  - `*_opt` — same book, same matching, but keeps an `order_id -> price`
    index for direct lookup instead of scanning
  - plus a resting player limit-order lifecycle (`ob_place_limit_*`,
    `ob_cancel_order_*`) that completes a feature `main.c` only half-built
    (see below), a pre-trade position risk limit
    (`ob_market_*_risk_checked_*`), cancel-replace order modification
    (`ob_modify_qty_*`, `ob_modify_price_*`), and third-party order
    cancel/qty-reduce (`ob_cancel_order_any_*`,
    `ob_reduce_order_qty_any_*`, for orders that aren't the player's own —
    see "LOBSTER real-market-data validation" below) — all new in the
    extracted engine, not ports of anything in `main.c`.
- `tests/test_correctness.c` — nine things, in order: (1) replays 200,000
  ticks of synthetic market-maker + player activity against both engines
  and asserts identical account state and book state after every tick —
  this has to pass before any benchmark number means anything; (2)
  confirms there's no fixed depth anymore: 500 orders at one price level,
  and a price far outside what the old fixed 3-slot window could ever
  represent, both just work; (3) a deterministic test of the limit order
  lifecycle (place, reserve, invalid rejection, cancel, refund,
  double-cancel rejection); (4) a 100,000-iteration randomized stress
  test (fixed-seed PRNG, not `rand()`, for reproducibility across
  platforms) mixing market orders, placements, and cancels, diffing full
  state after every operation; (5) risk-limit clipping arithmetic in
  isolation; (6) a 20,000-iteration randomized run asserting the position
  limit invariant holds through real matching on both engines; (7)
  cancel-replace's deterministic lifecycle; (8) a 50,000-iteration
  randomized run mixing modify_qty/modify_price into the existing
  market/limit/cancel workload; (9) third-party (`is_mine=0`) order
  cancel/qty-reduce — confirms the existing `is_mine`-gated functions
  correctly ignore such an order, and
  `ob_cancel_order_any_*`/`ob_reduce_order_qty_any_*` correctly find and
  mutate it (partial reduce, exact-zero-removal, reject-over-large,
  bogus-id).
- `bench/benchmark.c` — measures baseline vs optimized under identical
  workload using `clock_gettime(CLOCK_MONOTONIC)`, reporting the mean,
  the p50/p90/p99/p99.9 latency distribution per tick, and (via
  `--depth-sweep`/`make depth-sweep`) how the gap between baseline
  (linear scan) and opt (indexed) scales as book depth grows — a runtime
  loop now, not a recompile at different `-D` values, since there's no
  compile-time capacity left to vary.
- `include/spsc_ring.h` / `src/spsc_ring.c` — a lock-free single-producer/
  single-consumer ring buffer, used by `bench/threaded_bench.c` to hand
  messages from a "receiver" thread to the (still single-threaded)
  matching thread. `tests/test_spsc_ring.c` covers FIFO order and the
  exact capacity boundary, plus a real 2,000,000-item two-thread stress
  test, clean under both ASan and — since a lock-free queue's actual risk
  is a data race, not a memory-safety bug — ThreadSanitizer specifically
  (`make tsan`). See "Threaded ingestion" below for what this is for and
  what got measured.
- `tools/lobster_format.h` / `tools/lobster_replay.c` / `bench/lobster_bench.c`
  — replays a real LOBSTER order-flow stream through both engines,
  cross-checks the result against LOBSTER's own reconstructed order book
  (external ground truth, not a baseline-vs-opt self-check), and
  separately benchmarks the same translated calls under real (not
  synthetic) timing/size distributions. See "LOBSTER real-market-data
  validation" below.

## Bug fixed in `main.c`

`vga_draw_hollow_rect`'s right-edge call was:
```c
vga_draw_rect_fast(x + w - 1, 1, h, color);   // only 4 args, y hardcoded to 1
```
against a 5-argument signature `(x, y, w, h, color)`. Classic copy-paste
error from the line above it. Fixed to:
```c
vga_draw_rect_fast(x + w - 1, y, 1, h, color);
```

## A second bug — found and fixed after the extraction, not present in `main.c`

`main.c` never writes into a price level's fixed-size order queue
(`L3Order queue[MAX_ORDERS_PER_LVL]`) without first checking
`order_count < MAX_ORDERS_PER_LVL` at the call site — every insertion in
`process_tick` is guarded that way. When the matching logic was extracted
into `ob_add_order_opt`, that external guard didn't come with it: the
function wrote unconditionally to `lvl->queue[lvl->order_count++]`. A price
level that was already at capacity and received one more insert would
silently write past the end of the array, into whatever struct field
follows `queue[]` in `L3PriceLevel` — memory corruption, not necessarily a
crash, and the kind of bug a correctness harness can miss entirely if its
synthetic workload never happens to fill a level to exact capacity (the
200k-tick replay test didn't hit it).

This wasn't a hunch — it was reproduced directly: temporarily reverting the
fix and running the test binary under `clang -fsanitize=address,undefined`
gives a concrete `AddressSanitizer: stack-buffer-overflow` at the exact
insertion line, not a theoretical concern.

**Fix:** move the bounds check inside the function itself instead of
relying on every caller to remember it externally — `ob_add_order_opt` and
the new `ob_add_order_baseline` (added for a symmetric, safe API on both
sides) now return `1`/`0` for accept/reject and refuse to touch `queue[]`
or `total_qty` at all on rejection. `tests/test_correctness.c` has a
dedicated regression test that fills a level to exact capacity, attempts
one more insert, and asserts it's rejected with zero state mutation —
verified clean via `make asan` (clang; gcc's ASan/UBSan runtime was broken
on the dev machine this was built on, which is itself a reminder that
"the sanitizer build passed" is only as good as actually having a working
sanitizer toolchain — worth checking, not assuming).

The lesson generalizes past this one function: an invariant that's true
only because every caller happens to check it externally is not actually
an invariant — it's an accident waiting for the one caller that doesn't.
Encapsulating the check inside the function that owns the data made it
true unconditionally instead of true by convention.

## A half-built feature, completed: resting player limit orders

`main.c` only ever aggresses the book — `player_market_buy` /
`player_market_sell` eat resting liquidity immediately, there's no
function anywhere in the 919-line file that lets the player place a
resting limit order. And yet `L3Order.is_mine` is *checked* in eight
places: both market matchers skip `is_mine` orders explicitly ("don't eat
our own orders"), order rendering colors `is_mine` orders yellow, and
`player_cancel_all_orders` refunds cash (for bids) or inventory (for
asks) specifically for `is_mine` orders that are still resting. None of
that is reachable — `grep -n "is_mine = 1" main.c` returns nothing. The
refund logic in `player_cancel_all_orders` only makes sense if something
reserved that cash/inventory when the order was placed, and as shipped,
nothing does.

This isn't a bug to fix (nothing crashes; the dead branches are just
inert), but it's a real, half-implemented feature, and completing it in
the extracted engine — not `main.c`, which stays a verbatim historical
artifact — is what actually makes `is_mine`, the self-trade exclusion,
and the cancel-refund logic meaningful instead of dead code:

- `ob_place_limit_buy_*` / `ob_place_limit_sell_*` — reserve cash (buy) or
  inventory (sell) immediately at placement, same as a real limit order
  ties up capital the moment it rests in the book, then insert an
  `is_mine=1` order via the already bounds-checked `ob_add_order_*`.
  Returns the new order's id, or `-1` for an invalid level, non-positive
  qty, oversold inventory, or a full queue — nothing is reserved on
  failure.
- `ob_cancel_order_*` — cancel a single resting order by id, anywhere in
  the book, refunding exactly what was reserved. Deliberately an
  `O(orders)` linear scan across the whole book rather than an indexed
  lookup: book size is capped at `MAX_PRICE_LEVELS * MAX_ORDERS_PER_LVL *
  2` (60 orders at the board's real depth), so a scan is a handful of
  cache-line reads. This reasoning holds at the board's real depth, and
  `ob_cancel_order_*` is deliberately left exactly as-is — but it stops
  holding once depth is scaled the way `make depth-sweep` does (see "O(1)
  cancel-by-id index" below for the actual indexed alternative that was
  added once that stopped being hypothetical).

Tested two ways: a deterministic test walks through placement,
reservation, invalid-input rejection, cancel, exact refund, and
double-cancel rejection; a 100,000-iteration randomized stress test
(fixed-seed `xorshift32`, not `rand()`, so the exact same sequence
reproduces on any platform) mixes market orders, placements, and cancels
— including cancelling ids that were never issued or were already
filled — and diffs full baseline-vs-opt state after *every single
operation*, not periodically. Both clean under `make asan`.

## A gap the stress test exposed: no pre-trade risk limit

That 100,000-iteration randomized stress test above is doing something
useful beyond checking baseline vs opt agree: it's also just running the
*real* engine with no artificial restraint, and what it found is that
`player_market_sell` — faithfully ported from `main.c`, which has no
position floor at all — will drive `my_inventory` as far negative as the
random workload pushes it. In that specific test run, unconstrained,
100,000 operations left it at **-168,805**. Nothing in `main.c` or the
ported engine stops that; it's not a bug (the original game never claimed
to have risk controls), but it's exactly the kind of gap a real venue
would never ship with.

**Correction, added later:** re-running this exact test today gives a
final inventory near zero, not -168,805. That number wasn't purely
measuring "no risk limit" — part of what drove it that far was the fill-
direction bug documented in "A fill-direction bug..." below, which this
same stress test was *also* incidentally triggering (noise flow filling
this test's resting `is_mine` orders in the wrong direction, compounding
in the same direction as the unconstrained `is_player=1` selling). Fixing
that bug doesn't undo the underlying finding here — raw
`ob_market_buy_opt`/`ob_market_sell_opt` genuinely have no position floor,
by construction, independent of any bug — but it does mean this specific
number was measuring two things at once, not one. The clean, still-valid
proof that the risk limit actually works is `test_risk_limit_end_to_end`,
which asserts the bound holds under real matching, not an incidental
large number from an unrelated stress test.

Added `ob_market_buy_risk_checked_*` / `ob_market_sell_risk_checked_*` —
thin wrappers around the existing, *unmodified* `ob_market_buy_opt` /
`ob_market_sell_baseline` etc. — that clip the requested qty down to
whatever room remains under a `MAX_POSITION` limit (currently 500, either
direction) before it ever touches the book. Deliberately a wrapper, not a
change to the underlying functions: those have to stay a faithful,
unmodified port of `main.c` for the whole baseline-vs-opt diffing
methodology in this project to keep meaning what it's supposed to mean —
"the risk check is a new layer, not a redefinition of what baseline is"
is the same discipline as the limit-order lifecycle work above, applied
again here. It clips rather than rejects outright (0 submitted is a valid
outcome, not an error) — mirrors how real pre-trade size limiters
typically behave: reduce the order to what's allowed, don't just bounce
it. Only gates the player's own fills; the simulated market-maker/noise
side of the book isn't the desk's own risk and isn't limited.

Tested by directly setting `my_inventory` to known distances from the
boundary and checking the clipping arithmetic in isolation (so it doesn't
depend on the book having any particular amount of liquidity to fill),
and by a second 20,000-iteration randomized run — same style as the
stress test above, but this time asserting `|my_inventory| <=
MAX_POSITION` after every single risk-checked call, on both engines. It
holds.

## The optimization

Two things in the original hot path (`process_tick`, called on every timer
interrupt) do more work than necessary:

1. **`clean_ghosts`** runs a full two-pointer compaction over every price
   level's order queue every tick, even on the (common) ticks where nothing
   in that level actually needs removing.
   - Fix: scan once, read-only, to check if there's anything to remove at
     all. Only pay for the compaction pass when there actually is one.

2. **`update_total_qty`** recomputes the summed quantity for every price
   level by re-summing every order in it, from scratch, every single tick —
   even though every code path that changes an order's quantity (a fill, an
   insertion) already knows exactly how much `total_qty` changed by.
   - Fix: maintain `total_qty` incrementally at the point of mutation
     (`ob_market_buy_opt` / `ob_market_sell_opt` adjust it after each fill;
     the new `ob_add_order_opt` adjusts it on insertion). The full O(orders)
     rescan is removed from the per-tick hot path entirely.

Both were verified to produce byte-identical account and book state to the
original across 200,000 simulated ticks (`test_correctness.c`) before any
performance number was trusted.

## O(1) cancel-by-id index

**Superseded by the C++ rewrite** (see above): the separate `OrderIndex`/
`ob_place_limit_*_opt_indexed`/`ob_cancel_order_opt_indexed` API this
section describes doesn't exist anymore — `ob_cancel_order_opt` keeps
this same index natively now, no second parallel API needed, and the
`memcmp`/generic-function-pointer constraints below that shaped this
design don't apply to the dynamic book. Kept as history: the reasoning
here is exactly why baseline/opt differ the way they do today, just
implemented more directly once those two constraints went away with the
fixed-array struct they were about.

`ob_cancel_order_opt` (above) is an honest `O(orders-in-book)` scan,
justified by a 60-order book at the board's real depth. `make depth-sweep`
already exists to scale `MAX_ORDERS_PER_LVL` up to 400 to see how the other
two optimizations hold up as depth grows — at that depth a cancel walks up
to 2,400 order slots per call, and the "just scan it, it's cheap" reasoning
stops holding. This section is the O(1) alternative for that case.

Two hard constraints, already baked into this codebase, shaped the design:

1. `test_correctness.c` proves baseline == opt with a raw
   `memcmp(a, b, sizeof(L3OrderBook))`. Any extra per-order bookkeeping
   added to `L3Order` or `L3PriceLevel` (a self-index, a generation
   counter) would make that `memcmp` fail immediately, since baseline has
   no equivalent field to keep in sync. So the index (`OrderIndex`) lives
   in its own struct, populated by new wrapper functions
   (`ob_place_limit_*_opt_indexed`, `ob_cancel_order_opt_indexed`), never
   inside the order/level structs themselves.
2. `bench/benchmark.c` drives `ob_clean_ghosts_opt` through a generic
   `void (*)(L3PriceLevel *)` function pointer, identically to baseline's
   `clean_ghosts`, so the two stay directly comparable. Giving
   `ob_clean_ghosts_opt` an extra parameter to keep an index in sync
   during compaction would break that shared calling contract, so it's
   left untouched — which means a compaction can silently move an
   indexed order to a new slot within its level without the index
   knowing.

The design that fits both constraints: the index (open addressing, linear
probing, a flat 8,192-bucket static array — no malloc) caches `(level,
slot)` per order id. A lookup checks the cached slot first: **O(1)**, and
correct whenever no compaction has touched that level since the order was
placed or last found — the common case. If the cached slot doesn't match
(a compaction moved it), the fallback rescans only that **one level**
(bounded by `MAX_ORDERS_PER_LVL`), never the rest of the book. So it's
O(1) amortized with an O(single-level-depth) worst case — strictly better
than the unconditional whole-book scan in every case, exactly O(1) in the
common one.

Tested three ways, all against `ob_cancel_order_opt` as the reference
behavior: the fast path (place, cancel immediately, no compaction in
between — must hit the cache directly); the fallback (place, force a
compaction that shifts the order to a different slot, then cancel — must
still succeed via the bounded rescan, not just return "not found"); and a
50,000-iteration randomized run mixing market orders, indexed placements,
and indexed cancels, diffing full account+book state against the plain
linear-scan cancel after every single operation. All three pass under
`make asan`.

Measured, not just argued — `bench/benchmark.c` cancels an order pinned at
the worst-case position (last slot of the last ask level, the last place
`ob_cancel_order_opt`'s scan would ever reach) for both variants:

```
depth=10:   linear scan  47.3 ns/call   indexed  20.7 ns/call   2.29x
depth=400:  linear scan 1138.6 ns/call  indexed  22.4 ns/call  50.92x
```

The indexed lookup's cost doesn't move with depth (~21-22 ns/call at both
10 and 400); the linear scan's grows roughly with it (24x more depth →
~24x more time). That's the O(1)-vs-O(n) story made concrete instead of
just asserted.

## Cancel-replace (order modification)

**Note (post C++ rewrite):** `_opt_indexed` variants mentioned below no
longer exist as a separate API — `ob_modify_qty_opt`/`ob_modify_price_opt`
use the same index natively (see "C++ rewrite" above). Also, "level" is
"price" now — `ob_modify_price_*`'s second argument is a real price, not
an index into a fixed ladder. The behavioral story (in-place qty change
vs cancel-old+place-new for a price move, place-new-first ordering) is
unchanged.

Every existing way to change a resting order was cancel it, then place a
brand-new one — which is fine for actually leaving the book, but wrong for
the much more common case of a market maker adjusting a live quote: a
real venue distinguishes a pure quantity change (never loses queue
priority — nothing about *where* the order sits changed) from a price
change (almost always loses priority — the order is now competing at a
level it wasn't resting at before). Modeling both as "cancel + place" like
everything else here already did would have been silently wrong for the
quantity case and would have thrown away a real, testable distinction.

Two functions instead of one "modify", because the underlying behavior
genuinely isn't one thing:

- `ob_modify_qty_baseline` / `ob_modify_qty_opt` (+ `_opt_indexed`) —
  changes a resting order's quantity **in place**: same id, same
  `(level, slot)`, same queue position. An increase reserves the
  additional cash/inventory (rejected if it can't be covered); a decrease
  refunds the difference. Nothing is mutated on rejection, same
  discipline as every placement function above.
- `ob_modify_price_baseline` / `ob_modify_price_opt` (+ `_opt_indexed`) —
  moves an order to a different level. Modeled honestly as cancel-old +
  place-new rather than pretending it's an in-place move: the result gets
  a **new id** and goes to the back of the new level's queue, exactly
  like a real cancel-replace that changes price would. Calling it at the
  *same* level the order already rests at is a legal way to change qty
  while deliberately giving up priority — the difference from
  `ob_modify_qty_*` is exactly that trade-off, not a quirk.

**Ordering matters and is deliberate:** `ob_modify_price_*` tries the new
placement *first* and only cancels the old order if that succeeds. If the
new placement is rejected (bad level, invalid qty, insufficient
cash/inventory, full queue), the original order is left completely
untouched — never a state where neither the old nor a new order exists.
The stated cost of that safety: for a moment both the old and the new
order's reservations are held at once, so this can reject a move that a
venue netting collateral in real time would allow. Netting it instead
would mean modifying `ob_place_limit_*_opt` itself (breaking the
"wrapper, not a modification" rule the risk-limit and indexed-cancel
features already committed to) or reimplementing reservation logic a
third time — not worth it without evidence this ever actually blocks a
legitimate move at this book's scale.

The indexed variants share their O(1) lookup with `ob_cancel_order_opt_indexed`
through one refactored-out helper (`find_indexed_live_order`) rather than a
third copy of the same cache-hit/stale-fallback logic — the existing
indexed-cancel tests were rerun after that refactor to make sure it changed
nothing behaviorally, not just that it compiled.

Tested the same way as everything else here: a deterministic lifecycle
(increase, decrease, a rejected increase that leaves state untouched, a
price move that gets a fresh id and leaves the old order gone, a rejected
move that leaves the original live and unchanged) identical on baseline
and opt; a 50,000-iteration randomized run mixing modify_qty/modify_price
into the existing market/limit/cancel workload, diffing full state after
every single operation; and a second 50,000-iteration run proving the
indexed variants produce identical results to the plain whole-book-scan
versions. All clean under `make asan`.

Measured, not just argued, same methodology as the cancel benchmark above
— a resting order pinned at the worst-case position, modified repeatedly:

```
depth=10:   linear scan   47.5 ns/call   indexed  22.1 ns/call   2.15x
depth=400:  linear scan 1062.2 ns/call   indexed  22.4 ns/call  47.42x
```

Same shape as the cancel numbers, for the same reason (both share the
underlying scan-vs-hash lookup) — and it's specifically the operation a
market maker would call often (resizing/repricing a live quote), not the
one that's cancel-old+place-new either way.

## Results (x86_64 dev machine, see caveat below)

**Note (post C++ rewrite):** the numbers in this section changed shape,
not just value, because what baseline vs opt even measure changed (see
"C++ rewrite" above). The full-tick-loop numbers below now measure pure
add+match, which baseline and opt run through identical logic —
`ob_market_buy/sell_*` is the same function body for both now, so opt's
*only* extra cost there is maintaining an index it never gets to use in
this workload. The real story moved to cancel/modify-by-id at depth,
where that index is exactly what pays off:

```
[Full tick loop (add + match only), mean]
  baseline : ~150 ns/tick
  optimized: ~210 ns/tick
  speedup  : ~0.7x -- opt is SLOWER here, on purpose to be honest about:
             this workload never calls cancel/modify, so opt pays its
             index-maintenance cost with nothing to show for it. That's
             a real, correctly-measured result, not a bug -- see below.

[Cancel-by-id / modify-qty-by-id, baseline (scan) vs opt (indexed), by depth]
   depth  cancel_base_ns  cancel_opt_ns  speedup | modify_base_ns  modify_opt_ns  speedup
      10           361.2          266.2   1.36x  |         121.7           40.3    3.02x
      50           494.4          137.0   3.61x  |         371.6           43.7    8.51x
     100          1002.9          133.6   7.51x  |         748.4           50.4   14.86x
     500          5358.7          157.0  34.14x  |        4728.1           57.9   81.60x
    2000         38848.4          196.6 197.64x  |       19601.7           75.8  258.49x
    5000        121409.1          288.1 421.47x  |      139681.6          168.4  829.44x
```

Run-to-run variance on a shared, non-realtime dev machine is real. `make
bench` / `make depth-sweep` print exact figures for the run you actually
did; don't quote a number you haven't personally reproduced.

**Why opt is slower for the plain tick loop, stated plainly instead of
cherry-picking the workload that flatters it:** baseline and opt now run
the *identical* matching algorithm on the *identical* dynamic book — the
old "unoptimized fixed array vs optimized incremental bookkeeping"
distinction doesn't exist anymore (see "C++ rewrite"). The only thing
opt still does differently is maintain an `order_id -> price` index on
every insert/fill, in case a cancel or modify needs it later. A workload
that never cancels or modifies pays that cost for nothing — which is
exactly what the full-tick-loop number shows. This is the honest result,
not a disappointing one: it says plainly that opt's value is
*conditional* on actually doing id-based lookups, which the depth-sweep
table above demonstrates directly.

**The old limitation here doesn't exist anymore, and the numbers show
it:** this section used to explain why the speedup topped out around
1.2-2.5x — book depth was capped at `MAX_ORDERS_PER_LVL=10` (400 at the
deepest synthetic sweep), a deliberate FPGA-board memory constraint, so a
linear scan was only ever a scan over a handful of elements. With no
cap left, the depth-sweep table above runs a real linear scan against a
real index at depths up to 5,000 orders, and the gap is no longer subtle:
1.36x at depth 10 (same ballpark as before, for the same reason — a scan
over 10 elements is cheap either way), climbing past **400x** by 2,000
orders and **800x** on modify by 5,000. The board's real depth was never
going to produce a dramatic number; real order-book depth does.

## Trace visualizer

The only rendering this project ever had was `board/main.c`'s VGA
framebuffer — DE1-SoC only, not viewable without the physical board. The
desktop-testable engine had nothing: `printf("PASS")` and raw nanosecond
numbers, no way to actually see book state, fills, or account state change
over time. `make visualize` fixes that:

- `tools/book_trace.c` runs a fixed, reproducible 300-tick scenario (market
  noise liquidity + noise market orders, player limit placements/cancels,
  player risk-checked market orders — the same public API used everywhere
  else in this project, not a second implementation of anything) against
  `*_opt`, and prints one JSON object per tick to stdout: every price
  level, every order (id, qty, `is_mine`), and account state.
- `tools/render_trace.py` embeds that JSON into `tools/visualizer_template.html`,
  producing `build/book_visualizer.html` — self-contained, no server, just
  open it in a browser. Scrub or play through the 300 ticks; the ladder
  highlights `is_mine` orders (yellow), and side panel charts track
  inventory, PnL, and bid/ask depth over time.

**A finding this made obvious that wasn't obvious from code alone:** at
the time this was first built, price levels were set once in `ob_init_*`
and never moved again — nothing in the engine repriced a level. Watching
the replay, only *quantities* moved; the ladder's price column sat frozen
for all 300 ticks. That's invisible reading `ob_market_buy_opt` in
isolation (it only ever fills against whatever level price already is),
but it mattered a lot for building a market maker on top of this engine:
there was no fair-value drift, no adverse selection, nothing to hedge
against, and no notion of "the market moved against you" — the primary
risk a real market maker manages. **That gap motivated `ob_drift_price_*`**
(see below) at the time — since superseded by the C++ rewrite's dynamic
book, where price movement is a natural consequence of real order flow
(a level disappears when its last order leaves, a new one appears
wherever the next order names), not a primitive the engine needs to
provide separately.

## Price drift

**Superseded by the C++ rewrite** (see above) — `ob_drift_price_*` and
its later `ob_drift_bid_price_*`/`ob_drift_ask_price_*` split don't exist
anymore. A fixed-slot book needed an explicit primitive to relabel where
its slots sat as the market moved; a dynamic book doesn't, since a new
best price is just a new map key. Kept below as history: the finding
that motivated this (frozen prices → no adverse-selection risk) is still
true of what the fixed-array engine looked like at the time, and the
"honest limitation" paragraph is exactly the shape of bug that pushed
toward the eventual rewrite rather than another patch on the same model.

The finding above (frozen prices → no adverse-selection risk → inventory
skew has nothing real to defend against) came from actually watching the
engine run, and pointed at a real gap: worth fixing in the engine itself,
since price is a property of the book, not of whatever strategy sits on
top of it.

`ob_drift_price_baseline` / `ob_drift_price_opt` shifted every level's
price by the same `delta` (bids and asks together, so the spread and
level spacing `ob_init_*` established never changed — only where the
whole ladder sat). A primitive, not a policy: it didn't decide *when* or
*how much* to drift.

**Honest limitation, not glossed over — and the one that eventually
motivated removing this instead of patching it further:** the engine had
exactly `MAX_PRICE_LEVELS` fixed slots per side, unlike a real order book
where levels are created and destroyed as orders arrive at whatever price
they name. Drifting was therefore a relabeling of what those fixed slots'
price tags were, not a simulation of new levels appearing — which meant
an order resting in a level when it drifted got, in effect, repriced
along with it. Independently drifting bid and ask (attempted once, to
track real LOBSTER order flow — see git history for
`ob_drift_bid_price_*`/`ob_drift_ask_price_*`) turned out to corrupt
whichever side wasn't actually changing, since the two were fundamentally
glued together by the fixed-slot model. That's what finally motivated the
rewrite instead of another fix on the same foundation.

## A fill-direction bug — found by a downstream project's test, not this repo's own

`ob_market_buy_opt`/`ob_market_sell_opt` (and their `_baseline` twins)
update `my_inventory`/`my_cash` whenever `is_player || ord->is_mine` is
true — one condition meant to cover two genuinely different trades:

1. **The player's own aggressive fill** (`is_player=1`; self-trade
   exclusion already guarantees this never hits an `is_mine` order). A
   market buy here means the player bought: inventory up, cash down.
2. **Noise flow filling the player's own *resting* order**
   (`is_player=0`, `ord->is_mine=1`) — only reachable at all once the
   limit-order lifecycle existed, since `main.c` never set `is_mine=1`
   anywhere. If the resting order noise flow just filled was an **ask**,
   the player was the *seller* in that trade: inventory should go
   *down*, cash *up* — the opposite of case 1.

The code applied case 1's direction to both. Every baseline-vs-opt
equivalence test in this file kept passing the entire time, because
baseline and opt carried the identical error — those tests only ever
checked "do these two implementations agree with each other," which they
did, faithfully, on a shared mistake. What actually exposed it: a
*different* project (Market-Maker-Strategy, which vendors this engine as
a submodule) added a randomized test asserting `|inventory| <=
position_limit` under real noise-flow fills, and that test failed — not
because its own logic was wrong, but because the engine underneath it was
quietly crediting every one of its resting-order fills in the wrong
direction, which no test *inside this repo* was positioned to catch,
since none of them checked economic correctness, only mutual agreement.

**Fix:** both directions now compute `sign = is_player ? 1 : -1` and
apply it to the *existing* case-1 formula, so the well-tested
`is_player=1` path is untouched (same code, sign=1, zero behavioral
change) and the previously-backwards `is_player=0 && is_mine` path
flips to the correct direction. Applied identically to baseline and opt.

**What changed, honestly:** this does NOT affect the pre-trade risk-limit
wrappers (`ob_market_*_risk_checked_*`) — those only ever call the
matching functions with `is_player=1`, the path that was always correct.
It DOES change the final numbers `test_random_stress` and similar
randomized tests print (see the correction note in "A gap the stress test
exposed" above) — those tests mix noise flow with resting `is_mine`
orders, exactly the path this bug lived on. All of them still pass
(baseline and opt still agree, exactly as before — the fix is symmetric),
just with different, now-correct, absolute numbers.

Verified with the existing test suite re-run after the fix (`make test`,
`make asan`) — no new tests were needed specifically for this, since the
existing baseline-vs-opt randomized stress tests already exercise the
exact code path that changed; what mattered was confirming they still
pass identically after the fix, not adding new coverage for a fix to
code that was already being exercised.

## Capability map

`tools/capability_map.html` — a static, no-build-step reference page for
everyone (including future-me) who needs to know what this engine can do
without reading `orderbook_engine.h`: every public function grouped by
what it's for, in plain language, colored by baseline (blue) vs opt
(amber), plus a diagram of a resting order's actual lifecycle (place →
resting → `modify_qty` in place vs `modify_price`'s cancel-old+place-new
→ cancelled/filled) and a real depth-scaling chart of the indexed-vs-
linear-scan numbers (re-measured at 6 depths, not just the two points
quoted elsewhere in this README). Open it directly in a browser — no
`make` target, nothing to generate, it's just a file.

**Stale as of the C++ rewrite** (see above) — it still describes the
pre-rewrite API (`ob_drift_price_*`, `ob_cancel_order_opt_indexed`,
level-index placement) and hasn't been regenerated for the current
function list yet. Cross-check against `include/orderbook_engine.h`
until it's updated.

## LOBSTER real-market-data validation

Every correctness result above comes from one engine checked against
itself: `tests/test_correctness.c` replays synthetic order flow through
both `_baseline` and `_opt` and `memcmp`s the result. That proves the two
implementations agree; it can't catch a bug both of them share (which is
exactly what happened once already — see "A fill-direction bug" below).
The next step up in rigor is checking the engine against *external*
ground truth: a real exchange's own reconstructed order book, driven by
real order flow this engine never saw synthesized.

[LOBSTER](https://lobsterdata.com) publishes exactly that for NASDAQ: a
message file (every submission/cancel/delete/execution, timestamped to
the nanosecond) paired line-for-line with an orderbook file (the
independently reconstructed book snapshot after each message). `make
lobster-test` (`tools/lobster_replay.c`) replays the message stream
through both `_baseline` and `_opt`, translating each LOBSTER event into
calls on this engine's public API, and cross-checks the result against
the paired orderbook file after every synced event. `make lobster-bench`
(`bench/lobster_bench.c`, sharing the same message-format decoding via
`tools/lobster_format.h`) times the same translated calls instead of
checking them, to see whether real inter-arrival timing and order-size
distributions change the p99/p99.9 tail latency the synthetic
`depth-sweep` table above reports.

**Two real gaps this surfaced in the engine API**, both fixed with pure
additions (nothing already-tested changed):

- `ob_cancel_order_opt`/`ob_modify_qty_opt` only ever touch `is_mine==1`
  orders (they refund a cash/inventory reservation that only exists for
  the player's own resting orders). LOBSTER's cancels overwhelmingly
  target orders belonging to anonymous third parties, injected via
  `ob_add_order_*` the same way market-maker/noise liquidity always has
  been — and until now, nothing could cancel or reduce those by id.
  `ob_cancel_order_any_*`/`ob_reduce_order_qty_any_*` do exactly that,
  minus the `is_mine` filter and the refund, with their own differential
  test coverage.
- `MAX_PRICE_LEVELS` being hardcoded at all turned out to be a much
  bigger problem than expected: making it a build-time knob (an
  intermediate fix, since superseded) wasn't enough, because the real
  issue wasn't the *number* of tracked levels, it was that a fixed window
  can't discover a real price outside it once the level it's currently
  tracking drains dry. Running this same real GOOG data through the
  fixed-array engine is what actually motivated the full C++ rewrite
  documented above — see that section for the concrete before/after.

**Ground truth comparison is a direct lookup now, not a reconstruction:**
the old fixed-array engine only tracked a few price *ticks* around the
current best, always exactly one tick apart, while LOBSTER's orderbook
file lists *populated* price levels, which can be many ticks apart —
comparing "engine slot `i`" against "LOBSTER column `i`" would report
false mismatches whenever the real book had a gap, so `compare_side()`
used to reconstruct a dense per-tick view before comparing. Since the
rewrite, the engine only ever represents populated prices too (a level
exists exactly when an order rests there, same as LOBSTER's own
representation) — so `compare_side()` now just checks the engine's
tracked best against LOBSTER's reported best, then looks up
`ob_qty_at_price()` at each price LOBSTER actually reported. No
reconstruction needed because there's no representational gap left to
bridge.

**Status, with a real result**: `data/lobster/` holds one real trading
day (GOOG, 2012-06-21, level-5 depth, ~112,700 messages) run through
`make lobster-test`. It's what caught the fixed-window bug in the first
place — tracked best bid frozen at the $579.40 opening price all day
against a real market that closed near $565.12 — and it's what confirms
the rewrite fixed it: same file, same day, tracked best bid now follows
the market down to $565.10, two cents off by the close. The remaining
gap, and the bulk of the mismatches `make lobster-test` still reports on
this file, is a genuine, unfixable data-completeness limit: LOBSTER's
message file starts exactly at market open, not from an empty book — the
opening auction leaves real resting orders this replay never saw
submitted (`refs to pre-window orders` in the summary), and any book
comparison touching that residual liquidity will legitimately disagree
with ground truth for as long as it's outstanding. That's a property of
where the data window starts, not a bug in this engine or this replay.
`tests/fixtures/lobster_sample/` (a small hand-built fixture covering
every message type, see its own README) is what `make lobster-test` runs
by default and what's wired into CI, precisely because it doesn't have
this real-world gap — it's a regression test for the replay tool itself,
not a substitute for real-data validation.

```bash
make lobster-test  LOBSTER_MSG=data/lobster/TICKER_message.csv \
                    LOBSTER_BOOK=data/lobster/TICKER_orderbook.csv
make lobster-bench  LOBSTER_MSG=data/lobster/TICKER_message.csv
```
(No tick-size or depth flags needed anymore — LOBSTER prices go straight
into the engine, see "C++ rewrite" above. `data/lobster/` is gitignored,
real market data shouldn't be committed. The default `max_mismatches=5`
cap will trip almost immediately on real data because of the pre-window
gap above — pass a much larger value, e.g. `./build/lobster_replay
<msg> <book> 100000`, to run a full real day to completion instead of
aborting early.)

## Threaded ingestion: does splitting receive from matching actually help?

The engine's matching functions (`ob_market_*_opt` etc.) are, and stay,
single-threaded — that's not a limitation to fix, it's correct: one order
book is one sequential state machine, and price-time priority requires
processing events in order. Locking the *existing* single book and calling
its matching functions from multiple threads wouldn't parallelize
anything real; it would just make every operation wait for a lock with
nothing happening concurrently underneath it — strictly worse, not an
optimization.

The place multithreading legitimately fits: decoupling *receiving* a
message (in reality: a socket read, packet reassembly, parsing — work
with real, unpredictable tail latency that has nothing to do with the
matching logic) from *matching* it, so a slow/jittery receive doesn't
stall the matching thread directly. That only requires a
single-producer/single-consumer channel between exactly one receiver
thread and the one matching thread — no locks needed, since a lock-free
SPSC ring buffer is correct precisely because each of `head`/`tail` is
written by only one of the two threads, ever (`include/spsc_ring.h`).

**Correctness came first, same as everywhere else in this project.**
`tests/test_spsc_ring.c` proves FIFO ordering and the exact capacity
boundary single-threaded, then runs a genuine two-thread stress test: one
producer thread pushes 2,000,000 sequential integers, one consumer thread
pops and asserts every single one arrives, in order, with nothing lost or
duplicated. Critically, this was also run under **ThreadSanitizer**
(`make tsan`), not just ASan — ASan catches memory-safety bugs, not data
races, and a lock-free queue's entire risk surface *is* a potential data
race on `head`/`tail`. Clean under both, which is what actually justifies
trusting the `memory_order_acquire`/`memory_order_release` pairing in
`spsc_ring.c` rather than just asserting it's correct because the logic
looks textbook.

**Then it was measured, not assumed to help** (`bench/threaded_bench.c`,
`make threaded-bench`): a receiver thread injects randomized busy-spin
"jitter" before each message (standing in for unpredictable receive-side
cost) and pushes it into the ring; the matching thread pops and matches in
a tight spin loop (no blocking wait — a blocking syscall in the hot path
would defeat the entire point). The metric is the wall-clock gap between
the start of one matching call and the next: in a single-threaded
baseline that gap directly includes the jitter every time; if decoupling
works, the matching thread's own gaps should stop tracking it.

```
baseline    p50=1.7-1.8us  p90=3.5-4.4us  p99=9-12.5us   max=19-45us
unpinned    p50=1.7-1.8us  p90=3.3-4.8us  p99=6.7-12.6us max=21-81us
pinned      p50=2.0-2.3us  p90=4.1-4.8us  p99=11.4-12.2us max=~12.5-12.9ms (!)
```

(ranges across several runs; `pinned` forces the matching thread onto core
0 and the producer onto core 1 via `pthread_setaffinity_np`, to isolate
whether OS scheduler bouncing explains `unpinned`'s numbers.)

**The honest result: typical-case latency is a wash, and forced core
pinning made the tail dramatically worse, not better — the opposite of
the naive expectation.** `unpinned`'s p50/p90/p99 are essentially
indistinguishable from `baseline`'s on this workload (the simulated
jitter here is small enough, and this machine has 80 cores free enough,
that the OS scheduler already keeps both threads running without
forcing them to fight over one core). `pinned` is worse everywhere: a
consistent, extremely tight ~12.5-12.9ms outlier appeared in *every*
pinned run and never in `unpinned`/`baseline` — confirmed not a cgroup
CPU quota artifact (`cpu.cfs_quota_us` is `-1`, unlimited, on this
machine), but not root-caused further than that (this is a shared,
multi-tenant machine, not a box under this project's control — proper
root-causing would need `perf sched`, a NUMA topology check, and
likely a dedicated/isolated core, none of which are available here).

The takeaway that's actually defensible from this data: **pinning threads
to specific cores is a technique that assumes you control the whole
machine** (dedicated hardware, `isolcpus`, no other tenants) — it removes
the OS scheduler's freedom to route a thread around momentary contention,
and on a shared machine that freedom is doing real work you don't see
until you take it away. Blindly pinning to arbitrary core numbers on
infrastructure you don't fully control can make the tail dramatically
worse in a way your p99 monitoring might not even catch — only `max`
caught this, p99 looked fine. That's a more useful, more honest finding
than "threading helped" or "threading didn't help" would have been on
its own — it's a specific, measured statement about *when* a specific
technique backfires, with the data to back it up.

## How to run it yourself

```bash
make test         # build + run the replay, lifecycle, stress, and spsc_ring tests
make bench        # runs `make test` first, then builds + runs the benchmark
make asan         # rebuild the tests with clang(++) -fsanitize=address,undefined and run them
make tsan         # rebuild test_spsc_ring with clang -fsanitize=thread and run it
make cppcheck     # static analysis over the engine (C++17) plus bench/tests/tools (C)
make depth-sweep  # ~40s: the cancel/modify-by-id table in the Results section above, regenerated live
make visualize    # builds build/book_visualizer.html — open it in a browser
open tools/capability_map.html  # static reference, no build step, no code reading
make threaded-bench  # the receive/match threading comparison above, regenerated live
make lobster-test    # LOBSTER ground-truth replay (fixture by default, see LOBSTER_MSG/etc above)
make lobster-bench   # real-order-flow latency benchmark, same LOBSTER_* overrides
make clean        # remove build/
```

The engine is C++ now, so `test_correctness`/`benchmark`/every other
target links a C file against a C++-compiled object — the Makefile does
this as two compile steps (`gcc`/`cc` for the `.c` file, `g++` for
`orderbook_engine.cpp`) plus a `g++`-driven link so `libstdc++` resolves.
Building anything by hand outside the Makefile needs the same two steps:
```bash
g++ -O2 -Wall -Wextra -std=c++17 -Iinclude -c src/orderbook_engine.cpp -o orderbook_engine.o
gcc -O2 -Wall -Wextra -Iinclude -c tests/test_correctness.c -o test_correctness.o
g++ -o test_correctness test_correctness.o orderbook_engine.o
./test_correctness
```

CI (`.github/workflows/ci.yml`) runs `test`/`bench`/`lobster-test`/`asan`/
`cppcheck` on every push and pull request (`depth-sweep` is not in CI —
it's a deliberately slow, occasional-use target, not something that
should gate every commit; `lobster-bench` isn't either, for the same
reason benchmarks generally aren't CI gates).

## How to talk about this project in an interview

A useful narrative arc, in order:

1. **What it is**: a bare-metal limit order book + matching engine on an
   FPGA board, with real constraints (no malloc, no OS, interrupt-driven
   timing, direct framebuffer writes) — not a toy Python script.
2. **What you found in the original**: a real rendering bug (wrong
   argument count/hardcoded coordinate from a copy-paste), and a real
   inefficiency (a full rescan happening every tick when the information
   needed was already available incrementally).
3. **What you did about the inefficiency**: designed an incremental-update
   version, and — critically — **did not trust it until it passed a
   correctness harness that replays real workload and diffs state
   tick-by-tick against the original.**
4. **What you found while hardening the extraction itself**: the
   correctness harness passing is *not* the same as the code being safe —
   `ob_add_order_opt` had a real out-of-bounds write that 200,000 ticks of
   replay testing never triggered, because it depended on a level actually
   reaching exact capacity, which the synthetic workload never happened to
   do. You only caught it by (a) reasoning about the invariant the
   extraction silently dropped, and (b) confirming it under a sanitizer
   rather than trusting the reasoning alone. This is worth leading with in
   an interview: **a green test suite tells you the paths it exercised are
   correct, not that the code is correct** — sanitizers, bounds-checking
   internal to the function that owns the data, and reasoning about what
   invariants a refactor can silently break are a different, complementary
   layer of confidence, and knowing when you need that layer (not just
   "write more tests") is the actual signal.
5. **What you measured, and how you talk about the number**: a real,
   modest 1.2-1.4x at the board's actual depth, at both the mean *and* the
   tail (p50/p99 move together) — and then, rather than stopping at
   "here's why it's modest" as an unverified claim, **you measured the
   claim itself**: `make depth-sweep` recompiles the same benchmark at
   depths up to 40x the board's, and the speedup climbs to 2.5x, tracking
   the isolated rescan cost scaling with depth. Being able to say "here's
   the benchmark, here's its methodology (percentiles, not just mean),
   here's the limitation, and here's the swept data that confirms *why*
   it's a limitation instead of just asserting it" is a stronger signal
   than a bigger number would be on its own.
6. **What you noticed that wasn't a bug**: `main.c` checks
   `L3Order.is_mine` in eight places but never sets it — a real,
   half-implemented feature (resting player limit orders), not a crash or
   a wrong number. Recognizing "this code path is unreachable, and here's
   the grep that proves it" is a different skill than finding a crash,
   and completing it in the extracted engine (with its own place/cancel
   API, tested both deterministically and via 100k iterations of
   randomized fuzzing against a fixed-seed PRNG) is a chance to show you
   can extend a matching engine's actual domain logic — order lifecycle,
   not just "make the loop faster" — correctly.
7. **What the stress test found beyond baseline-vs-opt agreement**: the
   same 100k-iteration randomized test built to verify the optimization
   also, incidentally, revealed that unconstrained player selling drives
   `my_inventory` arbitrarily negative with nothing stopping it — because
   that's just what `main.c`'s logic does, faithfully ported (the exact
   number quoted for this at the time, -168,805, later turned out to be
   partly a different bug — see point 9 — worth mentioning in an interview
   as its own lesson: "the specific number I quoted changed after a later
   fix, here's why, and here's why the underlying finding still held"
   is a stronger answer than defending a stale number would have been).
   Adding a pre-trade position limit as a *wrapper* around the existing
   matching functions (not a modification to them) is a small, deliberate
   design choice worth explaining on its own: it keeps "baseline" meaning
   "an unmodified, verifiable port of the original," while still being
   able to demonstrate risk-control thinking — the layer a real venue
   would never ship without — on top of it. Being able to say why you
   *didn't* just add an `if` statement inside the existing function is as
   much the signal as the risk check itself.

8. **What you did once "it's cheap, don't bother indexing" stopped being
   true**: the original cancel was an honest, stated tradeoff — O(orders)
   is fine at a 60-order book. `make depth-sweep` already existed to test
   whether that kind of reasoning holds up as depth scales, so rather than
   leaving the tradeoff as a static claim, you built the O(1) index it
   implied was eventually needed — and did it under two real constraints
   the existing codebase had already committed to (a byte-level
   `memcmp` correctness harness, and a function-pointer-driven benchmark
   contract), not by rewriting either of them to make the new feature
   easier. Talking through *why* the index caches `(level, slot)` instead
   of a live pointer, and why the fallback is bounded to one level instead
   of requiring the index to stay perfectly in sync with a compaction
   routine it can't safely be wired into, is a concrete answer to "design
   me an order book" that most candidates only gesture at abstractly.

9. **What a downstream project's test found that this repo's own tests
   couldn't**: `ob_market_buy_opt`/`sell_opt` (and their baseline twins)
   applied the *same* accounting sign to two cases that need opposite
   signs — the player's own aggressive fill, and noise flow filling the
   player's own resting order (which only became reachable once the
   limit-order lifecycle existed). Baseline and opt shared the identical
   error, so every baseline-vs-opt equivalence test in this file kept
   passing throughout — they were only ever checking "do these two
   implementations agree with each other," not "is either of them
   economically correct." What actually surfaced it: a *new* test in the
   separate Market-Maker-Strategy project (a randomized position-limit
   check under real noise-flow fills — see that repo) failed in a way an
   internally-consistent-but-wrong engine has no way to catch on its own.
   The fix and the discovery story are both in "A fill-direction bug..."
   below. This is worth leading with as the single clearest illustration
   in this whole project of "green tests mean the paths you exercised
   agree with each other, not that they're right" — more so than the
   buffer-overflow bug above, because that one *did* get caught by a
   sanitizer eventually; this one could only ever have been caught by
   something checking real-world correctness, not internal consistency.

10. **What running real market data found that no synthetic test or
    external-reference check could have**: point 9's lesson was "internal
    consistency isn't correctness — you need an external reference."
    Replaying a real NASDAQ trading day (LOBSTER, GOOG, 2012-06-21)
    against this engine's own reconstructed order book *was* exactly that
    external reference, and it still took real data to expose the
    problem: the engine's fixed `MAX_PRICE_LEVELS` window meant that once
    the top tracked price fully drained, the engine had no way to
    discover the real next-best price — anything outside its narrow
    window was never recorded to begin with. By end of day the tracked
    best bid was frozen at the $579.40 open while the real market had
    moved to $565.12. No synthetic workload would ever surface this,
    because a synthetic generator only ever produces activity *within*
    whatever window you told it to use — the bug is specifically about
    activity *outside* the window the code can see, which by construction
    a hand-rolled test workload never generates. The fix wasn't a patch:
    it was rewriting the core in C++ around a dynamic `std::map`/
    `std::deque` book with no fixed depth at all — a bigger, riskier
    change than anything else in this project's history, verified the
    same way everything else here was (baseline/opt differential testing,
    ASan/UBSan, then the *same* real GOOG day rerun to confirm the
    tracked price now follows the market to within two cents by close).
    The honest framing for an interview: real-world validation doesn't
    just catch bugs synthetic tests miss, it can reveal that a whole
    design assumption — "a small fixed window is good enough" — was
    wrong, and knowing when a finding calls for a rewrite instead of
    another patch on the same foundation is itself a judgment call worth
    being able to defend.

This is a much stronger story than "I built a project" — it demonstrates
the actual discipline (verify before trusting, measure before claiming,
distinguish "tests pass" from "code is safe," state limitations honestly)
that low-latency/quant infra interviews are specifically trying to probe
for.
