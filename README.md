# HFT Order Book Engine

An L3 limit order book, pulled out of a bare-metal RISC-V program
(`board/main.c`, DE1-SoC FPGA, no malloc, no OS) so it can be tested and
benchmarked on a normal machine. Started as a straight C port with the
board's fixed-size arrays; it's a dynamic C++17 book now.

How it got here -- bugs found, things rewritten, numbers that turned out
wrong -- is in [CHANGELOG.md](CHANGELOG.md). This file is just the
current state.

License: [MIT](LICENSE). CI: [`.github/workflows/ci.yml`](.github/workflows/ci.yml).

## Layout

```
board/main.c                  original board program (RISC-V only, not built)
include/orderbook_engine.h    public API
src/orderbook_engine.cpp      the engine
include/spsc_ring.h           header-only lock-free SPSC queue
tests/test_correctness.cpp    baseline-vs-opt differential tests + lifecycle/risk tests
tests/test_spsc_ring.cpp      ring FIFO/capacity + 2M-item two-thread stress
bench/benchmark.cpp           tick-loop latency percentiles, depth sweep
bench/threaded_bench.cpp      does splitting receive/match onto two threads help?
bench/lobster_bench.cpp       latency under real NASDAQ order flow
tools/lobster_replay.cpp      replays LOBSTER data, checks against its own book
tools/lobster_format.h        shared LOBSTER parsing
tools/book_trace.cpp + render_trace.py + visualizer_template.html
                              300-tick scenario -> self-contained HTML replay
tools/capability_map.html     plain-language API map (Chinese, no build step)
tests/fixtures/lobster_sample/  tiny hand-built LOBSTER fixture (what CI runs)
```

## Running it

```bash
make test            # correctness + spsc tests
make bench           # test, then tick-loop benchmark
make asan            # tests under clang++ ASan/UBSan
make tsan            # spsc test under TSan
make cppcheck
make depth-sweep     # ~40s, cancel/modify-by-id vs depth
make threaded-bench
make visualize       # -> build/book_visualizer.html
make lobster-test    # fixture by default
make lobster-bench
```

Real data: drop LOBSTER files in `data/lobster/` (gitignored) and pass
them in. The default `max_mismatches=5` is for the fixture; on a full
day, call `./build/lobster_replay <msg> <book> 100000` directly.

```bash
make lobster-test LOBSTER_MSG=data/lobster/TICKER_message.csv \
                  LOBSTER_BOOK=data/lobster/TICKER_orderbook.csv
```

CI runs test, bench, lobster-test, asan, tsan and cppcheck.
depth-sweep and lobster-bench are too slow/noisy to gate on.

## The engine

- Per side, a `std::map<price, PriceLevel>`. Each level has a
  `std::deque<Order>` in time priority. A level exists only while
  something rests at it. No depth cap.
- Every function comes in `_baseline` and `_opt`. Same book, same
  matching. The only difference: opt keeps an `order_id -> (side, price)`
  hash, so cancel/modify jumps to the right level instead of scanning
  the whole book. Tests run both and compare after every operation.
- `L3OrderBook` is opaque. Read it through `ob_num_levels`,
  `ob_level_price`, `ob_qty_at_price`, `ob_dump_levels` etc.

What you can do with it:

| | |
|---|---|
| `ob_market_buy/sell_*` | walk the book best-to-worst. `is_player=1` skips your own orders (no self-trade) |
| `ob_add_order_*` | rest a raw order, anyone's |
| `ob_place_limit_buy/sell_*` | rest your own order, reserving cash (buy) or inventory (sell) up front |
| `ob_cancel_order_*` | cancel your own order by id, refund the reservation |
| `ob_modify_qty_*` | change qty in place, keeps queue priority |
| `ob_modify_price_*` | cancel + re-place: new id, back of the queue. Places first, so a rejected move leaves the old order alone |
| `ob_cancel_order_any_*` / `ob_reduce_order_qty_any_*` | same thing for third-party orders, no refund (what LOBSTER replay uses) |
| `ob_market_*_risk_checked_*` | clips the qty so `|inventory| <= MAX_POSITION` (500) |

When someone else fills one of your resting orders, only the leg that
wasn't reserved gets settled. The reserved leg was already paid at
placement.

## What's verified

**`make test`** -- a 200k-tick synthetic replay plus randomized stress
runs (100k, 50k and 20k ops, fixed-seed xorshift), baseline vs opt
compared after every op. Also deterministic tests for the limit order
lifecycle, cancel-replace, risk clipping, third-party cancel/reduce, and
resting-fill settlement. All clean under ASan/UBSan.

This only proves baseline and opt agree with each other. It can't catch
a bug they share, and that has happened twice (see changelog).

**`make lobster-test` on real data** -- three full NASDAQ days (AAPL,
GOOG, AMZN, 2012-06-21, level 5): **0 mismatches** over 510k compared
rows, 0 rejected executions, 0 baseline/opt divergences.

What this doesn't tell you:
- LOBSTER level-5 files only carry events inside the top 5 levels. Orders
  cancelled while deeper than that never show up, so the replay resyncs
  to the visible window after each row. Each check is really "one
  message applied to a book that was right a row ago". It's
  mutation-tested, not taken on faith: an off-by-one in Type 4 gives
  7,329 mismatches, dropping 1 in 1000 Type 3s gives 53.
- It only exercises add / reduce-by-id / cancel-by-id. Nothing in it
  touches `ob_market_*`, the account, or the risk checks.

## Numbers

x86_64 shared dev box, so variance is real. Rerun it yourself before
quoting anything.

Cancel/modify by id, pinned at the worst-case position (`make depth-sweep`):

```
 depth  cancel_base_ns  cancel_opt_ns  speedup | modify_base_ns  modify_opt_ns  speedup
    10           361.2          266.2   1.36x  |         121.7           40.3    3.02x
   100          1002.9          133.6   7.51x  |         748.4           50.4   14.86x
   500          5358.7          157.0  34.14x  |        4728.1           57.9   81.60x
  2000         38848.4          196.6 197.64x  |       19601.7           75.8  258.49x
  5000        121409.1          288.1 421.47x  |      139681.6          168.4  829.44x
```

The baseline here scans the entire book, which is a pretty weak thing
to beat. Treat these as "the index works", not "this is fast".

Plain tick loop (add + match, no cancels): baseline ~150 ns, opt ~210
ns. Opt is slower because it maintains an index this workload never
uses.

Threaded ingestion (`make threaded-bench`): putting receive on its own
thread made no real difference to p50/p90/p99. Pinning threads to cores
made it worse: a consistent ~12.5 ms max outlier on a shared machine.
p99 didn't show it, only max did.

## Known gaps

- **Limit orders don't cross.** Placing a buy at or above the best ask
  just rests it, leaving a crossed book. Only `ob_market_*` matches.
- `ob_place_limit_buy_*` doesn't check cash (`modify_qty` does), so cash
  can go negative.
- `MAX_POSITION` only gates your own market orders. Fills on your
  resting limit orders can push inventory past it.
- `ob_market_*` return nothing, so there's no way to tell how much
  actually filled.
- Duplicate order ids aren't rejected. Opt's index gets overwritten and
  baseline/opt diverge.
- Performance-wise it's still std containers: node allocation per
  level, and opt still scans the deque linearly within a level.
