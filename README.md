# HFT Order Book Engine — Optimization & Benchmark

CI workflow: [`.github/workflows/ci.yml`](.github/workflows/ci.yml) (runs
`make test`, `make bench`, `make asan`, `make cppcheck` on every push/PR —
badge omitted until this is pushed to a repo GitHub can render status for).
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
src/orderbook_engine.c       baseline + optimized implementations, limit order lifecycle
tests/test_correctness.c     200k-tick replay + capacity/lifecycle/randomized-stress tests
bench/benchmark.c            baseline-vs-optimized timing: mean, percentiles, depth sweep
scripts/format_depth_sweep.awk  tabulates `make depth-sweep` output
Makefile                     test / bench / asan / cppcheck / depth-sweep / clean targets
.github/workflows/ci.yml     runs test / bench / asan / cppcheck on every push/PR
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
    (see below) — new in the extracted engine, not a port of anything in
    `main.c`.
- `tests/test_correctness.c` — four things, in order: (1) replays 200,000
  ticks of synthetic market-maker + player activity against both engines
  and asserts identical account state and book state after every tick —
  this has to pass before any benchmark number means anything; (2) a
  capacity regression test guarding the buffer-overflow bug described
  below; (3) a deterministic test of the limit order lifecycle (place,
  reserve, invalid rejection, cancel, refund, double-cancel rejection);
  (4) a 100,000-iteration randomized stress test (fixed-seed PRNG, not
  `rand()`, for reproducibility across platforms) mixing market orders,
  placements, and cancels, diffing full state after every operation.
- `bench/benchmark.c` — measures baseline vs optimized under identical
  workload using `clock_gettime(CLOCK_MONOTONIC)`, reporting the mean,
  the p50/p90/p99/p99.9 latency distribution per tick, and (via
  `make depth-sweep`) how the gap between baseline and optimized scales
  as book depth grows past the board's real value.

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
  cache-line reads — not worth an `order_id -> location` index without
  evidence (profiling) that this is ever hot. Stated explicitly rather
  than silently over-engineered: a real production engine handling
  unbounded depth would want that index; this one doesn't need it yet.

Tested two ways: a deterministic test walks through placement,
reservation, invalid-input rejection, cancel, exact refund, and
double-cancel rejection; a 100,000-iteration randomized stress test
(fixed-seed `xorshift32`, not `rand()`, so the exact same sequence
reproduces on any platform) mixes market orders, placements, and cancels
— including cancelling ids that were never issued or were already
filled — and diffs full baseline-vs-opt state after *every single
operation*, not periodically. Both clean under `make asan`.

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

## How to run it yourself

```bash
make test         # build + run the replay, capacity, lifecycle, and stress tests
make bench        # runs `make test` first, then builds + runs the benchmark
make asan         # rebuild the tests with clang -fsanitize=address,undefined and run them
make cppcheck     # static analysis over src/, bench/, tests/
make depth-sweep  # ~35s: the table in the Results section above, regenerated live
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

This is a much stronger story than "I built a project" — it demonstrates
the actual discipline (verify before trusting, measure before claiming,
distinguish "tests pass" from "code is safe," state limitations honestly)
that low-latency/quant infra interviews are specifically trying to probe
for.
