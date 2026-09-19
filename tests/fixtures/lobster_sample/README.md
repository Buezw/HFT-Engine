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
price levels (each row compared on its own -- LOBSTER's book is synced
per message, not per sweep), a trading halt (Type 7, must be skipped
entirely), and a Type 4 against an id we never saw submitted followed by
a normal row. That last pair is the regression for executions being
applied as a market order on our best price: the old translation ate
order 104's shares for it and failed the next row.

`orderbook.csv` was rewritten in real LOBSTER layout (populated levels
first, dummy +/-9999999999 prices with size 0 after), so every row's
first level actually gets checked. The pre-window order in the last two
rows is a simplification -- a real file would show it in the book from
row 1.

This is a **regression smoke test for `tools/lobster_replay.cpp` itself** —
it proves the parser/translator/comparator logic is correct, the same role
`tests/test_correctness.cpp`'s synthetic workloads play for the core engine.
It is not a substitute for validating against real market data. Run
`make lobster-test LOBSTER_MSG=<path> LOBSTER_BOOK=<path> LOBSTER_TICK=100`
once a real LOBSTER sample is downloaded (see README.md's LOBSTER section).
