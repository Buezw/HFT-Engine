CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra -Wpedantic -std=c11 -Iinclude
SAN_CC   ?= clang
SAN_FLAGS = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Iinclude

SRC         = src/orderbook_engine.c
RING_SRC    = src/spsc_ring.c
TEST_SRC    = tests/test_correctness.c
RING_TEST_SRC = tests/test_spsc_ring.c
BENCH_SRC   = bench/benchmark.c
TRACE_SRC   = tools/book_trace.c
THREAD_SRC  = bench/threaded_bench.c
LOBSTER_SRC = tools/lobster_replay.c

# Real LOBSTER samples have a matching depth (5/10/50 populated levels);
# default to the committed synthetic fixture (see
# tests/fixtures/lobster_sample/README.md) so `make lobster-test` works
# out of the box with no download. Override all three for real data, e.g.:
#   make lobster-test LOBSTER_MSG=data/lobster/AMZN_message.csv \
#     LOBSTER_BOOK=data/lobster/AMZN_orderbook.csv LOBSTER_TICK=100 \
#     LOBSTER_LEVELS=10
LOBSTER_MSG    ?= tests/fixtures/lobster_sample/message.csv
LOBSTER_BOOK   ?= tests/fixtures/lobster_sample/orderbook.csv
LOBSTER_TICK   ?= 1
LOBSTER_LEVELS ?= 3

TSAN_CC    ?= clang
TSAN_FLAGS = -O1 -g -fsanitize=thread -Wall -Wextra -Iinclude -pthread

BUILD_DIR = build

DEPTHS = 10 25 50 100 200 400

.PHONY: all test bench asan clean cppcheck depth-sweep visualize threaded-bench tsan lobster-test lobster-bench

all: $(BUILD_DIR)/test_correctness $(BUILD_DIR)/benchmark

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_correctness: $(TEST_SRC) $(SRC) include/orderbook_engine.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(SRC)

$(BUILD_DIR)/test_spsc_ring: $(RING_TEST_SRC) $(RING_SRC) include/spsc_ring.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RING_TEST_SRC) $(RING_SRC)

$(BUILD_DIR)/benchmark: $(BENCH_SRC) $(SRC) include/orderbook_engine.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(BENCH_SRC) $(SRC)

# Correctness first, always — a benchmark number from an engine that
# hasn't passed the replay+regression tests isn't worth reporting.
test: $(BUILD_DIR)/test_correctness $(BUILD_DIR)/test_spsc_ring
	./$(BUILD_DIR)/test_correctness
	./$(BUILD_DIR)/test_spsc_ring

bench: test $(BUILD_DIR)/benchmark
	./$(BUILD_DIR)/benchmark

# Uses clang, not $(CC): gcc's ASan/UBSan runtime libraries are not
# installed on every machine this was developed on, clang's are.
asan: | $(BUILD_DIR)
	$(SAN_CC) $(SAN_FLAGS) -o $(BUILD_DIR)/test_correctness_asan $(TEST_SRC) $(SRC)
	./$(BUILD_DIR)/test_correctness_asan
	$(SAN_CC) $(SAN_FLAGS) -pthread -o $(BUILD_DIR)/test_spsc_ring_asan $(RING_TEST_SRC) $(RING_SRC)
	./$(BUILD_DIR)/test_spsc_ring_asan

# ASan/UBSan catch memory-safety bugs, not data races — spsc_ring.h's
# entire risk surface IS a data race (on head/tail between two real
# threads), so it needs ThreadSanitizer specifically, not just more of the
# same sanitizer already used for test_correctness.c.
tsan: | $(BUILD_DIR)
	$(TSAN_CC) $(TSAN_FLAGS) -o $(BUILD_DIR)/test_spsc_ring_tsan $(RING_TEST_SRC) $(RING_SRC)
	./$(BUILD_DIR)/test_spsc_ring_tsan

# Compiles bench/benchmark.c at several MAX_ORDERS_PER_LVL values (10 is
# the board's real, VRAM/CPU-budget-constrained depth; the rest are
# hypothetical) and tabulates baseline-vs-optimized speedup by depth —
# turns the README's "the O(n) rescan matters more as book depth grows"
# claim into measured data instead of an assertion. Each depth needs its
# own binary: MAX_ORDERS_PER_LVL sizes an array embedded in L3PriceLevel,
# so it's a compile-time constant, not something one binary can vary.
depth-sweep: | $(BUILD_DIR)
	@rm -f $(BUILD_DIR)/depth_sweep_rows.txt
	@for d in $(DEPTHS); do \
		echo "building + running at depth $$d..." >&2; \
		$(CC) -O2 -Wall -Wextra -Iinclude -DMAX_ORDERS_PER_LVL=$$d \
			-o $(BUILD_DIR)/bench_depth$$d $(BENCH_SRC) $(SRC) || exit 1; \
		./$(BUILD_DIR)/bench_depth$$d | grep '^SWEEP_ROW' >> $(BUILD_DIR)/depth_sweep_rows.txt; \
	done
	@echo
	@awk -f scripts/format_depth_sweep.awk $(BUILD_DIR)/depth_sweep_rows.txt

# Runs a fixed 300-tick scenario against the opt engine (market noise +
# player limit orders/cancels/risk-checked orders), dumps one JSON object
# per tick, then wraps it into a self-contained, scrubbable HTML replay.
# No server needed — open the printed path directly in a browser.
visualize: $(BUILD_DIR)/book_trace | $(BUILD_DIR)
	./$(BUILD_DIR)/book_trace 300 > $(BUILD_DIR)/book_trace.jsonl
	python3 tools/render_trace.py $(BUILD_DIR)/book_trace.jsonl \
		tools/visualizer_template.html $(BUILD_DIR)/book_visualizer.html

$(BUILD_DIR)/book_trace: $(TRACE_SRC) $(SRC) include/orderbook_engine.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(TRACE_SRC) $(SRC)

# Tests whether splitting "receive" and "match" onto two threads (spsc_ring.h)
# actually protects the matching thread from receive-side jitter, or is
# just complexity for nothing — see bench/threaded_bench.c for the full
# rationale and what this specifically does/doesn't claim. Needs -pthread
# (POSIX threads, not part of the base CFLAGS since nothing else here needs it).
threaded-bench: $(BUILD_DIR)/threaded_bench
	./$(BUILD_DIR)/threaded_bench

$(BUILD_DIR)/threaded_bench: $(THREAD_SRC) $(SRC) $(RING_SRC) include/orderbook_engine.h include/spsc_ring.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(THREAD_SRC) $(SRC) $(RING_SRC)

# Replays a real (or, by default, the committed synthetic fixture's)
# LOBSTER message stream through the engine and cross-checks the result
# against LOBSTER's own reconstructed orderbook snapshots -- external
# ground truth, not the baseline-vs-opt self-consistency tests above.
# MAX_PRICE_LEVELS is compiled in at LOBSTER_LEVELS so the engine's fixed
# per-tick window matches how deep the input data actually is (see
# tools/lobster_replay.c's header for why this has to be a build-time
# choice, same reasoning as depth-sweep's MAX_ORDERS_PER_LVL).
lobster-test: $(BUILD_DIR)/lobster_replay
	./$(BUILD_DIR)/lobster_replay $(LOBSTER_MSG) $(LOBSTER_BOOK) $(LOBSTER_TICK)

$(BUILD_DIR)/lobster_replay: $(LOBSTER_SRC) tools/lobster_format.h $(SRC) include/orderbook_engine.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DMAX_PRICE_LEVELS=$(LOBSTER_LEVELS) -o $@ $(LOBSTER_SRC) $(SRC)

# Correctness first, always -- same discipline `bench` already applies to
# the synthetic benchmark: a latency number from a replay that hasn't
# passed ground-truth validation isn't worth reporting.
lobster-bench: lobster-test $(BUILD_DIR)/lobster_bench
	./$(BUILD_DIR)/lobster_bench $(LOBSTER_MSG) $(LOBSTER_TICK)

$(BUILD_DIR)/lobster_bench: bench/lobster_bench.c tools/lobster_format.h $(SRC) include/orderbook_engine.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DMAX_PRICE_LEVELS=$(LOBSTER_LEVELS) -Itools -o $@ bench/lobster_bench.c $(SRC)

cppcheck:
	cppcheck --enable=warning,style,performance,portability \
		--suppress=missingIncludeSystem --error-exitcode=1 \
		-Iinclude src/ bench/ tests/ tools/

clean:
	rm -rf $(BUILD_DIR)
