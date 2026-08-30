# HFT Order Book Engine — Optimization & Benchmark

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

- `main.c` — the original bare-metal program, with one real bug fixed
  (see below). This still targets the DE1-SoC and won't compile with a
  normal x86 gcc (it uses RISC-V-specific interrupt attributes and raw
  hardware addresses on purpose).
- `orderbook_engine.h` / `orderbook_engine.c` — the order book data
  structures and matching logic extracted into a platform-independent
  form, with two implementations side by side:
  - `*_baseline` — a faithful port of the logic in `main.c`
  - `*_opt` — an optimized version with the same external behavior
- `test_correctness.c` — replays 200,000 ticks of synthetic market-maker +
  player activity against both engines and asserts identical account state
  and book state after every tick. This has to pass before any benchmark
  number means anything.
- `benchmark.c` — measures baseline vs optimized under identical workload
  using `clock_gettime(CLOCK_MONOTONIC)`.

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
[Full tick loop]
  baseline : ~150-160 ns/tick
  optimized: ~120-125 ns/tick
  speedup  : ~1.2-1.3x

[clean_ghosts in isolation]
  baseline : ~12-13 ns/call
  optimized: ~10-11 ns/call
  speedup  : ~1.15-1.22x

[update_total_qty full rescan, isolated, book at max depth]
  baseline : ~40 ns/call, called once per tick
  optimized: removed from hot path entirely (0 ns/tick)
```

**Important caveat, stated plainly rather than glossed over:** these
absolute nanosecond numbers are from an x86_64 dev machine, not the
DE1-SoC's RISC-V core `main.c` actually runs on — different ISA, different
cache hierarchy, different clock speed. The *relative* speedup is the
meaningful takeaway, since both variants ran the same instructions (modulo
the actual algorithmic difference) on the same machine in the same run.

**The honest limitation of this result:** the book depth in this project is
intentionally small (`MAX_PRICE_LEVELS=3`, `MAX_ORDERS_PER_LVL=10` — a
deliberate design choice to fit VRAM/CPU budget on the FPGA board), so a
per-level O(n) scan is only ever a scan over ~10 elements. That's why the
speedup is a modest 1.2-1.3x rather than an order of magnitude: removing an
O(n) rescan matters a lot more as book depth grows, and matters less when n
is already small. This is exactly the kind of trade-off worth stating
explicitly in an interview — not every "obviously correct" optimization
produces a dramatic number at small scale, and knowing *why* a benchmark
came out modest is more valuable than the number itself.

## How to run it yourself

```bash
gcc -O2 -Wall -Wextra -o test_correctness test_correctness.c orderbook_engine.c
./test_correctness      # should print PASS

gcc -O2 -Wall -Wextra -o benchmark benchmark.c orderbook_engine.c
./benchmark              # prints timing comparison
```

## How to talk about this project in an interview

A useful narrative arc, in order:

1. **What it is**: a bare-metal limit order book + matching engine on an
   FPGA board, with real constraints (no malloc, no OS, interrupt-driven
   timing, direct framebuffer writes) — not a toy Python script.
2. **What you found**: a real bug (wrong argument count/hardcoded
   coordinate from a copy-paste), and a real inefficiency (a full rescan
   happening every tick when the information needed was already available
   incrementally).
3. **What you did about the inefficiency**: designed an incremental-update
   version, and — critically — **did not trust it until it passed a
   correctness harness that replays real workload and diffs state
   tick-by-tick against the original.**
4. **What you measured, and how you talk about the number**: a real,
   modest 1.2-1.3x, plus the ability to explain *why* it's modest (small
   book depth) rather than just reporting an inflated number. Being able to
   say "here's the benchmark, here's its limitation, here's what would
   change the result at larger scale" is a stronger signal than a bigger
   number would be on its own.

This is a much stronger story than "I built a project" — it demonstrates
the actual discipline (verify before trusting, measure before claiming,
state limitations honestly) that low-latency/quant infra interviews are
specifically trying to probe for.
