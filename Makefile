CC       ?= gcc
CXX      ?= g++
CFLAGS   ?= -O2 -Wall -Wextra -Wpedantic -std=c11 -Iinclude
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c++17 -Iinclude
SAN_CC   ?= clang
SAN_CXX  ?= clang++
SAN_FLAGS   = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Iinclude
SAN_CXXFLAGS = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Iinclude -std=c++17

# The engine is C++ now (src/orderbook_engine.cpp); everything else here
# stays plain C and links against it. There's no per-depth macro to
# recompile it for anymore (see README) -- one object file, reused by
# every target, same as any other mixed C/C++ project.
ENGINE_SRC    = src/orderbook_engine.cpp
RING_SRC      = src/spsc_ring.c
TEST_SRC      = tests/test_correctness.c
RING_TEST_SRC = tests/test_spsc_ring.c
BENCH_SRC     = bench/benchmark.c
TRACE_SRC     = tools/book_trace.c
THREAD_SRC    = bench/threaded_bench.c
LOBSTER_SRC   = tools/lobster_replay.c

# Default to the committed synthetic fixture (see
# tests/fixtures/lobster_sample/README.md) so `make lobster-test` works
# out of the box with no download. Override for real data, e.g.:
#   make lobster-test LOBSTER_MSG=data/lobster/GOOG_message.csv \
#     LOBSTER_BOOK=data/lobster/GOOG_orderbook.csv
# (No tick-size/level-depth knobs needed anymore -- prices go straight
# into the engine as-is now, see tools/lobster_replay.c's header.)
LOBSTER_MSG  ?= tests/fixtures/lobster_sample/message.csv
LOBSTER_BOOK ?= tests/fixtures/lobster_sample/orderbook.csv

TSAN_CC    ?= clang
TSAN_FLAGS = -O1 -g -fsanitize=thread -Wall -Wextra -Iinclude -pthread

BUILD_DIR = build

.PHONY: all test bench asan clean cppcheck depth-sweep visualize threaded-bench tsan lobster-test lobster-bench

all: $(BUILD_DIR)/test_correctness $(BUILD_DIR)/benchmark

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/orderbook_engine.o: $(ENGINE_SRC) include/orderbook_engine.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $(ENGINE_SRC)

$(BUILD_DIR)/test_correctness: $(TEST_SRC) $(BUILD_DIR)/orderbook_engine.o include/orderbook_engine.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_correctness.o $(TEST_SRC)
	$(CXX) -o $@ $(BUILD_DIR)/test_correctness.o $(BUILD_DIR)/orderbook_engine.o

$(BUILD_DIR)/test_spsc_ring: $(RING_TEST_SRC) $(RING_SRC) include/spsc_ring.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RING_TEST_SRC) $(RING_SRC)

$(BUILD_DIR)/benchmark: $(BENCH_SRC) $(BUILD_DIR)/orderbook_engine.o include/orderbook_engine.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/benchmark.o $(BENCH_SRC)
	$(CXX) -o $@ $(BUILD_DIR)/benchmark.o $(BUILD_DIR)/orderbook_engine.o

# Correctness first, always — a benchmark number from an engine that
# hasn't passed the replay+regression tests isn't worth reporting.
test: $(BUILD_DIR)/test_correctness $(BUILD_DIR)/test_spsc_ring
	./$(BUILD_DIR)/test_correctness
	./$(BUILD_DIR)/test_spsc_ring

bench: test $(BUILD_DIR)/benchmark
	./$(BUILD_DIR)/benchmark

# Uses clang, not $(CC)/$(CXX): gcc's ASan/UBSan runtime libraries are not
# installed on every machine this was developed on, clang's are.
asan: | $(BUILD_DIR)
	$(SAN_CXX) $(SAN_CXXFLAGS) -c -o $(BUILD_DIR)/orderbook_engine_asan.o $(ENGINE_SRC)
	$(SAN_CC) $(SAN_FLAGS) -c -o $(BUILD_DIR)/test_correctness_asan.o $(TEST_SRC)
	$(SAN_CXX) -fsanitize=address,undefined -o $(BUILD_DIR)/test_correctness_asan \
		$(BUILD_DIR)/test_correctness_asan.o $(BUILD_DIR)/orderbook_engine_asan.o
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

# Sweeps how many orders sit at one price level at runtime now, not by
# recompiling at different -DMAX_ORDERS_PER_LVL values — there's no such
# macro anymore now that the book is a dynamic std::map/std::deque
# structure with no fixed capacity (see README). One binary, one run.
depth-sweep: $(BUILD_DIR)/benchmark
	./$(BUILD_DIR)/benchmark --depth-sweep

# Runs a fixed 300-tick scenario against the opt engine (market noise +
# player limit orders/cancels/risk-checked orders), dumps one JSON object
# per tick, then wraps it into a self-contained, scrubbable HTML replay.
# No server needed — open the printed path directly in a browser.
visualize: $(BUILD_DIR)/book_trace | $(BUILD_DIR)
	./$(BUILD_DIR)/book_trace 300 > $(BUILD_DIR)/book_trace.jsonl
	python3 tools/render_trace.py $(BUILD_DIR)/book_trace.jsonl \
		tools/visualizer_template.html $(BUILD_DIR)/book_visualizer.html

$(BUILD_DIR)/book_trace: $(TRACE_SRC) $(BUILD_DIR)/orderbook_engine.o include/orderbook_engine.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/book_trace.o $(TRACE_SRC)
	$(CXX) -o $@ $(BUILD_DIR)/book_trace.o $(BUILD_DIR)/orderbook_engine.o

# Tests whether splitting "receive" and "match" onto two threads (spsc_ring.h)
# actually protects the matching thread from receive-side jitter, or is
# just complexity for nothing — see bench/threaded_bench.c for the full
# rationale and what this specifically does/doesn't claim. Needs -pthread
# (POSIX threads, not part of the base CFLAGS since nothing else here needs it).
threaded-bench: $(BUILD_DIR)/threaded_bench
	./$(BUILD_DIR)/threaded_bench

$(BUILD_DIR)/threaded_bench: $(THREAD_SRC) $(RING_SRC) $(BUILD_DIR)/orderbook_engine.o include/orderbook_engine.h include/spsc_ring.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -pthread -c -o $(BUILD_DIR)/threaded_bench.o $(THREAD_SRC)
	$(CC) $(CFLAGS) -pthread -c -o $(BUILD_DIR)/spsc_ring_thr.o $(RING_SRC)
	$(CXX) -pthread -o $@ $(BUILD_DIR)/threaded_bench.o $(BUILD_DIR)/spsc_ring_thr.o $(BUILD_DIR)/orderbook_engine.o

# Replays a real (or, by default, the committed synthetic fixture's)
# LOBSTER message stream through the engine and cross-checks the result
# against LOBSTER's own reconstructed orderbook snapshots -- external
# ground truth, not the baseline-vs-opt self-consistency tests above.
lobster-test: $(BUILD_DIR)/lobster_replay
	./$(BUILD_DIR)/lobster_replay $(LOBSTER_MSG) $(LOBSTER_BOOK)

$(BUILD_DIR)/lobster_replay: $(LOBSTER_SRC) tools/lobster_format.h $(BUILD_DIR)/orderbook_engine.o include/orderbook_engine.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/lobster_replay.o $(LOBSTER_SRC)
	$(CXX) -o $@ $(BUILD_DIR)/lobster_replay.o $(BUILD_DIR)/orderbook_engine.o

# Correctness first, always -- same discipline `bench` already applies to
# the synthetic benchmark: a latency number from a replay that hasn't
# passed ground-truth validation isn't worth reporting.
lobster-bench: lobster-test $(BUILD_DIR)/lobster_bench
	./$(BUILD_DIR)/lobster_bench $(LOBSTER_MSG)

$(BUILD_DIR)/lobster_bench: bench/lobster_bench.c tools/lobster_format.h $(BUILD_DIR)/orderbook_engine.o include/orderbook_engine.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Itools -c -o $(BUILD_DIR)/lobster_bench.o bench/lobster_bench.c
	$(CXX) -o $@ $(BUILD_DIR)/lobster_bench.o $(BUILD_DIR)/orderbook_engine.o

cppcheck:
	cppcheck --enable=warning,style,performance,portability \
		--suppress=missingIncludeSystem --error-exitcode=1 \
		-Iinclude --std=c++17 src/orderbook_engine.cpp
	cppcheck --enable=warning,style,performance,portability \
		--suppress=missingIncludeSystem --error-exitcode=1 \
		-Iinclude bench/ tests/ tools/ src/spsc_ring.c

clean:
	rm -rf $(BUILD_DIR)
