# Synthetic LOBSTER-format fixture

`message.csv`/`orderbook.csv` here are **hand-constructed**, not real LOBSTER
data — this sandbox couldn't reach lobsterdata.com's actual sample downloads
(the site is now a JS-rendered SPA whose real download endpoints aren't
reachable by static fetch, and no working browser automation was available
when this was built). They follow LOBSTER's documented column format
(`Time,Type,OrderID,Size,Price,Direction` / per-level `AskPrice,AskSize,
BidPrice,BidSize`) with `tick_size=1` for easy hand-verification, and every
`orderbook.csv` row was computed by hand from what `src/orderbook_engine.c`
should produce after the paired `message.csv` row, at `MAX_PRICE_LEVELS=3`.

Covers every message type this engine's LOBSTER adapter has to translate:
a submission that improves the best (triggers `ob_drift_price_*`), one that
doesn't, a partial cancel (Type 2), a full deletion (Type 3), a single-row
execution (Type 4), a two-row same-timestamp execution sweep spanning two
price levels, and a trading halt (Type 7, must be skipped entirely).

This is a **regression smoke test for `tools/lobster_replay.c` itself** —
it proves the parser/translator/comparator logic is correct, the same role
`tests/test_correctness.c`'s synthetic workloads play for the core engine.
It is not a substitute for validating against real market data. Run
`make lobster-test LOBSTER_MSG=<path> LOBSTER_BOOK=<path> LOBSTER_TICK=100`
once a real LOBSTER sample is downloaded (see README.md's LOBSTER section).
