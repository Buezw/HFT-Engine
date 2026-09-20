# Synthetic LOBSTER-format fixture

`message.csv`/`orderbook.csv` are **hand-built**, not real LOBSTER data --
this sandbox couldn't reach lobsterdata.com's downloads (JS-rendered SPA,
no browser automation available at the time). They follow LOBSTER's
documented columns (`Time,Type,OrderID,Size,Price,Direction` and per-level
`AskPrice,AskSize,BidPrice,BidSize`) with small round prices so every row
can be checked by hand.

What it covers, one message type at a time: a submission that improves the
best and one that doesn't, a partial cancel (Type 2), a deletion (Type 3),
a single-row execution (Type 4), a two-row same-timestamp sweep across two
price levels (compared row by row -- LOBSTER syncs its book per message,
not per sweep), a halt (Type 7, skipped entirely), and a Type 4 against an
id that was never submitted followed by a normal row. That last pair is
the regression for executions being applied as a market order against our
own best price: the old translation ate order 104's shares and then failed
the next row.

`orderbook.csv` uses the real LOBSTER layout -- populated levels first,
then dummy +/-9999999999 prices at size 0 -- so the first level of every
row actually gets checked. The pre-window order in the last two rows is a
simplification; a real file would show it resting from row 1.

This is a **regression smoke test for `tools/lobster_replay.cpp`**, i.e.
the parser/translator/comparator, the same role
`tests/test_correctness.cpp`'s synthetic workloads play for the engine.
It's not a substitute for real data. For that, put a real LOBSTER day in
`data/lobster/` and run:

```bash
make lobster-test LOBSTER_MSG=<path> LOBSTER_BOOK=<path>
```
