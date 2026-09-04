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

## Files

```
board/main.c                 original bare-metal program (RISC-V/DE1-SoC only)
include/orderbook_engine.h   public API + data structures for the extracted engine
src/orderbook_engine.c       baseline + optimized implementations, limit order lifecycle, risk limit
tests/test_correctness.c     200k-tick replay + capacity/lifecycle/stress/risk-limit tests
bench/benchmark.c            baseline-vs-optimized timing: mean, percentiles, depth sweep
tools/book_trace.c           runs a scenario against the opt engine, dumps per-tick JSON
tools/render_trace.py        wraps the JSON trace into a self-contained HTML replay
tools/visualizer_template.html  the replay page itself (ladder + inventory/PnL/depth charts)
include/spsc_ring.h / src/spsc_ring.c   lock-free single-producer/single-consumer queue
tests/test_spsc_ring.c       FIFO/capacity boundary tests + a real 2M-item multithreaded stress test
bench/threaded_bench.c       measures whether splitting receive/match onto two threads helps or hurts
scripts/format_depth_sweep.awk  tabulates `make depth-sweep` output
Makefile                     test / bench / asan / tsan / cppcheck / depth-sweep / visualize / threaded-bench / clean targets
.github/workflows/ci.yml     runs test / bench / asan / tsan / cppcheck on every push/PR
```

- `board/main.c` — the original bare-metal program, with one real bug fixed
  (see below). This still targets the DE1-SoC and won't compile with a
  normal x86 gcc (it uses RISC-V-specific interrupt attributes and raw
  hardware addresses on purpose). Kept as a historical/reference artifact,
  not part of the buildable project.
- `include/orderbook_engine.h` / `src/orderbook_engine.c` — the order book
  data structures and matching logic extracted into a platform-independent
  form, with two implementations side by side:
  - `*_baseline` — a faithful port of the logic in `main.c`
  - `*_opt` — an optimized version with the same external behavior
  - plus a resting player limit-order lifecycle (`ob_place_limit_*`,
    `ob_cancel_order_*`) that completes a feature `main.c` only half-built
    (see below), a pre-trade position risk limit
    (`ob_market_*_risk_checked_*`), and an O(1) cancel-by-id index
    (`OrderIndex`, `ob_place_limit_*_opt_indexed`,
    `ob_cancel_order_opt_indexed`) — all new in the extracted engine, not
    ports of anything in `main.c`.
- `tests/test_correctness.c` — seven things, in order: (1) replays 200,000
  ticks of synthetic market-maker + player activity against both engines
  and asserts identical account state and book state after every tick —
  this has to pass before any benchmark number means anything; (2) a
  capacity regression test guarding the buffer-overflow bug described
  below; (3) a deterministic test of the limit order lifecycle (place,
  reserve, invalid rejection, cancel, refund, double-cancel rejection);
  (4) a 100,000-iteration randomized stress test (fixed-seed PRNG, not
  `rand()`, for reproducibility across platforms) mixing market orders,
  placements, and cancels, diffing full state after every operation; (5)
  risk-limit clipping arithmetic in isolation; (6) a 20,000-iteration
  randomized run asserting the position limit invariant holds through
  real matching on both engines; (7) the O(1) index's fast path, its
  bounded fallback when a compaction stales the cache, and a 50,000-
  iteration randomized run proving it's behaviorally identical to the
  plain linear-scan cancel at every step.
- `bench/benchmark.c` — measures baseline vs optimized under identical
  workload using `clock_gettime(CLOCK_MONOTONIC)`, reporting the mean,
  the p50/p90/p99/p99.9 latency distribution per tick, (via
  `make depth-sweep`) how the gap between baseline and optimized scales
  as book depth grows past the board's real value, and a dedicated
  linear-scan-vs-indexed cancel comparison at a fixed worst-case position.
- `include/spsc_ring.h` / `src/spsc_ring.c` — a lock-free single-producer/
  single-consumer ring buffer, used by `bench/threaded_bench.c` to hand
  messages from a "receiver" thread to the (still single-threaded)
  matching thread. `tests/test_spsc_ring.c` covers FIFO order and the
  exact capacity boundary, plus a real 2,000,000-item two-thread stress
  test, clean under both ASan and — since a lock-free queue's actual risk
  is a data race, not a memory-safety bug — ThreadSanitizer specifically
  (`make tsan`). See "Threaded ingestion" below for what this is for and
  what got measured.

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

## Results (x86_64 dev machine, see caveat below)

```
[Full tick loop, mean]
  baseline : ~150-185 ns/tick
  optimized: ~120-150 ns/tick
  speedup  : ~1.2-1.3x

[Full tick loop, tail latency — p50 / p99]
  baseline : p50 ~166 ns, p99 ~221 ns
  optimized: p50 ~134 ns, p99 ~178 ns
  speedup  : ~1.2x at both p50 and p99 (see note below on why this
             consistency matters)

[clean_ghosts in isolation]
  baseline : ~11-13 ns/call
  optimized: ~10-11 ns/call
  speedup  : ~1.08-1.22x

[update_total_qty full rescan, isolated, book at max depth]
  baseline : ~29-40 ns/call, called once per tick
  optimized: removed from hot path entirely (0 ns/tick)
```

Run-to-run variance on a shared, non-realtime dev machine is real — hence
the ranges above rather than a single number. `make bench` prints exact
figures for the run you actually did; don't quote a number you haven't
personally reproduced.

**Why the p50/p99 comparison, not just the mean:** a mean can hide a
regime where the optimized version is faster on typical ticks but has a
worse tail (e.g. from extra branching or cache pressure introduced by the
"optimization" itself) — exactly the failure mode that matters most for
code with a hard per-tick budget, and exactly the kind of thing a single
average silently launders. Here they move together (~1.2x at both p50 and
p99), which is itself a finding worth stating: the optimization doesn't
just win on average, it wins uniformly across the distribution, so there's
no tail-latency regression hiding behind a better mean.

**Important caveat, stated plainly rather than glossed over:** these
absolute nanosecond numbers are from an x86_64 dev machine, not the
DE1-SoC's RISC-V core `main.c` actually runs on — different ISA, different
cache hierarchy, different clock speed. The *relative* speedup is the
meaningful takeaway, since both variants ran the same instructions (modulo
the actual algorithmic difference) on the same machine in the same run.

**The honest limitation of this result — and the measurement that backs
it up:** the book depth in this project is intentionally small
(`MAX_PRICE_LEVELS=3`, `MAX_ORDERS_PER_LVL=10` — a deliberate design
choice to fit VRAM/CPU budget on the FPGA board), so a per-level O(n) scan
is only ever a scan over ~10 elements. That's the whole reason the speedup
above is a modest 1.2-1.4x rather than an order of magnitude. That claim
used to just be asserted; `make depth-sweep` (see below) now measures it
by recompiling the benchmark at several depths via `-DMAX_ORDERS_PER_LVL`
and tabulating the baseline-vs-optimized gap at each:

```
depth     mean_ns_base    mean_ns_opt    mean_x     p50_x     p99_x rescan_ns/call
-----     ------------    -----------    ------     -----     ----- --------------
10               208.0          150.9     1.38x     1.39x     1.48x           29.9
25               283.5          202.5     1.40x     1.43x     1.41x           88.7
50               421.5          285.0     1.48x     1.52x     1.47x          148.6
100             1024.4          462.4     2.22x     2.27x     2.23x          524.4
200             1871.6          783.9     2.39x     2.42x     2.39x          985.6
400             3545.3         1422.9     2.49x     2.51x     2.50x         1963.6
```

At the board's real depth (10), the optimization is worth 1.38x. At 40x
that depth, it's worth 2.49x — nearly double the speedup, tracking the
`O(n)` rescan cost (`rescan_ns/call`) scaling roughly linearly with `n`
(10→400 is 40x depth, ~29.9ns→~1964ns is ~66x rescan cost — slightly
superlinear, plausibly cache effects once a level's queue no longer fits
comfortably in a few cache lines, not investigated further here). This is
exactly the kind of trade-off worth stating explicitly in an interview —
not every "obviously correct" optimization produces a dramatic number at
small scale, a benchmark that only ran at the shipped configuration
wouldn't have shown *why* it's modest, and now there's a table instead of
an assertion.

## Trace visualizer

The only rendering this project ever had was `board/main.c`'s VGA
framebuffer — DE1-SoC only, not viewable without the physical board. The
desktop-testable engine had nothing: `printf("PASS")` and raw nanosecond
numbers, no way to actually see book state, fills, or account state change
over time. `make visualize` fixes that:

- `tools/book_trace.c` runs a fixed, reproducible 300-tick scenario (market
  noise liquidity + noise market orders, player limit placements/cancels via
  the indexed lifecycle, player risk-checked market orders — the same
  public API used everywhere else in this project, not a second
  implementation of anything) against `*_opt`, and prints one JSON object
  per tick to stdout: every price level, every order (id, qty, `is_mine`,
  ghost state), and account state.
- `tools/render_trace.py` embeds that JSON into `tools/visualizer_template.html`,
  producing `build/book_visualizer.html` — self-contained, no server, just
  open it in a browser. Scrub or play through the 300 ticks; the ladder
  highlights `is_mine` orders (yellow) and ghost/pending-cleanup orders
  (dimmed), and side panel charts track inventory, PnL, and bid/ask depth
  over time.

**A finding this made obvious that wasn't obvious from code alone:** price
levels are set once in `ob_init_*` and never move again — nothing in this
engine reprices a level. Watching the replay, only *quantities* move; the
ladder's price column is frozen for the entire 300 ticks. That's invisible
reading `ob_market_buy_opt` in isolation (it only ever fills against
whatever `lvl->price` already is), but it matters a lot for building a
market maker on top of this engine: there's no fair-value drift, no
adverse selection, nothing to hedge against, and no notion of "the market
moved against you" — the primary risk a real market maker manages. Any
quoting/inventory-skew logic added next either needs its own price-walk
mechanism, or needs to be honest that it's optimizing spread capture against
a market that structurally cannot move.

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
make test         # build + run the replay, capacity, lifecycle, stress, and spsc_ring tests
make bench        # runs `make test` first, then builds + runs the benchmark
make asan         # rebuild the tests with clang -fsanitize=address,undefined and run them
make tsan         # rebuild test_spsc_ring with clang -fsanitize=thread and run it
make cppcheck     # static analysis over src/, bench/, tests/
make depth-sweep  # ~35s: the table in the Results section above, regenerated live
make visualize    # builds build/book_visualizer.html — open it in a browser
make threaded-bench  # the receive/match threading comparison above, regenerated live
make clean        # remove build/
```

`bench/benchmark.c` and `tests/test_correctness.c` can also be built
directly if you don't want to use the Makefile:
```bash
gcc -O2 -Wall -Wextra -Iinclude -o test_correctness tests/test_correctness.c src/orderbook_engine.c
./test_correctness

gcc -O2 -Wall -Wextra -Iinclude -o benchmark bench/benchmark.c src/orderbook_engine.c
./benchmark
```

CI (`.github/workflows/ci.yml`) runs `test`/`bench`/`asan`/`cppcheck` on
every push and pull request (`depth-sweep` is not in CI — it's a
deliberately slow, occasional-use target, not something that should gate
every commit).

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
   `my_inventory` to -168,805 with nothing stopping it — because that's
   just what `main.c`'s logic does, faithfully ported. Adding a pre-trade
   position limit as a *wrapper* around the existing matching functions
   (not a modification to them) is a small, deliberate design choice worth
   explaining on its own: it keeps "baseline" meaning "an unmodified,
   verifiable port of the original," while still being able to demonstrate
   risk-control thinking — the layer a real venue would never ship
   without — on top of it. Being able to say why you *didn't* just add an
   `if` statement inside the existing function is as much the signal as
   the risk check itself.

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

This is a much stronger story than "I built a project" — it demonstrates
the actual discipline (verify before trusting, measure before claiming,
distinguish "tests pass" from "code is safe," state limitations honestly)
that low-latency/quant infra interviews are specifically trying to probe
for.
