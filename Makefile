CXX      ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c++17 -Iinclude
SAN_CXX  ?= clang++
SAN_CXXFLAGS = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Iinclude -std=c++17

# Everything here is C++17 (board/main.c, the original bare-metal RISC-V
# program, isn't built by this Makefile at all). The engine is compiled
# once into one object file and reused by every target -- there's no
# per-depth macro to recompile it for anymore (see README). spsc_ring.h
# is header-only.
ENGINE_SRC    = src/orderbook_engine.cpp
TEST_SRC      = tests/test_correctness.cpp
RING_TEST_SRC = tests/test_spsc_ring.cpp
BENCH_SRC     = bench/benchmark.cpp
TRACE_SRC     = tools/book_trace.cpp
THREAD_SRC    = bench/threaded_bench.cpp
LOBSTER_SRC   = tools/lobster_replay.cpp
LOBSTER_BENCH_SRC = bench/lobster_bench.cpp

# Default to the committed synthetic fixture (see
# tests/fixtures/lobster_sample/README.md) so `make lobster-test` works
# out of the box with no download. Override for real data, e.g.:
#   make lobster-test LOBSTER_MSG=data/lobster/GOOG_message.csv \
#     LOBSTER_BOOK=data/lobster/GOOG_orderbook.csv
# (No tick-size/level-depth knobs needed anymore -- prices go straight
# into the engine as-is now, see tools/lobster_replay.cpp's header.)
LOBSTER_MSG  ?= tests/fixtures/lobster_sample/message.csv
LOBSTER_BOOK ?= tests/fixtures/lobster_sample/orderbook.csv

TSAN_CXX   ?= clang++
TSAN_FLAGS = -O1 -g -fsanitize=thread -Wall -Wextra -Iinclude -std=c++17 -pthread

BUILD_DIR = build

.PHONY: all test bench asan clean cppcheck depth-sweep visualize threaded-bench tsan lobster-test lobster-bench

all: $(BUILD_DIR)/test_correctness $(BUILD_DIR)/benchmark

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/orderbook_engine.o: $(ENGINE_SRC) include/orderbook_engine.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $(ENGINE_SRC)

$(BUILD_DIR)/test_correctness: $(TEST_SRC) $(BUILD_DIR)/orderbook_engine.o include/orderbook_engine.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_SRC) $(BUILD_DIR)/orderbook_engine.o

$(BUILD_DIR)/test_spsc_ring: $(RING_TEST_SRC) include/spsc_ring.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(RING_TEST_SRC)

$(BUILD_DIR)/benchmark: $(BENCH_SRC) $(BUILD_DIR)/orderbook_engine.o include/orderbook_engine.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(BENCH_SRC) $(BUILD_DIR)/orderbook_engine.o

# Correctness first, always — a benchmark number from an engine that
# hasn't passed the replay+regression tests isn't worth reporting.
test: $(BUILD_DIR)/test_correctness $(BUILD_DIR)/test_spsc_ring
	./$(BUILD_DIR)/test_correctness
	./$(BUILD_DIR)/test_spsc_ring

bench: test $(BUILD_DIR)/benchmark
	./$(BUILD_DIR)/benchmark

# Uses clang++, not $(CXX): gcc's ASan/UBSan runtime libraries are not
# installed on every machine this was developed on, clang's are.
asan: | $(BUILD_DIR)
	$(SAN_CXX) $(SAN_CXXFLAGS) -o $(BUILD_DIR)/test_correctness_asan $(TEST_SRC) $(ENGINE_SRC)
	./$(BUILD_DIR)/test_correctness_asan
	$(SAN_CXX) $(SAN_CXXFLAGS) -pthread -o $(BUILD_DIR)/test_spsc_ring_asan $(RING_TEST_SRC)
	./$(BUILD_DIR)/test_spsc_ring_asan

# ASan/UBSan catch memory-safety bugs, not data races — spsc_ring.h's
# entire risk surface IS a data race (on head/tail between two real
# threads), so it needs ThreadSanitizer specifically, not just more of the
# same sanitizer already used for test_correctness.cpp.
tsan: | $(BUILD_DIR)
	$(TSAN_CXX) $(TSAN_FLAGS) -o $(BUILD_DIR)/test_spsc_ring_tsan $(RING_TEST_SRC)
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
	$(CXX) $(CXXFLAGS) -o $@ $(TRACE_SRC) $(BUILD_DIR)/orderbook_engine.o

# Tests whether splitting "receive" and "match" onto two threads (spsc_ring.h)
# actually protects the matching thread from receive-side jitter, or is
# just complexity for nothing — see bench/threaded_bench.cpp for the full
# rationale and what this specifically does/doesn't claim. Needs -pthread
# (std::thread, not part of the base CXXFLAGS since nothing else here needs it).
threaded-bench: $(BUILD_DIR)/threaded_bench
	./$(BUILD_DIR)/threaded_bench

$(BUILD_DIR)/threaded_bench: $(THREAD_SRC) $(BUILD_DIR)/orderbook_engine.o include/orderbook_engine.h include/spsc_ring.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(THREAD_SRC) $(BUILD_DIR)/orderbook_engine.o

# Replays a real (or, by default, the committed synthetic fixture's)
# LOBSTER message stream through the engine and cross-checks the result
# against LOBSTER's own reconstructed orderbook snapshots -- external
# ground truth, not the baseline-vs-opt self-consistency tests above.
lobster-test: $(BUILD_DIR)/lobster_replay
	./$(BUILD_DIR)/lobster_replay $(LOBSTER_MSG) $(LOBSTER_BOOK)

$(BUILD_DIR)/lobster_replay: $(LOBSTER_SRC) tools/lobster_format.h $(BUILD_DIR)/orderbook_engine.o include/orderbook_engine.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(LOBSTER_SRC) $(BUILD_DIR)/orderbook_engine.o

# Correctness first, always -- same discipline `bench` already applies to
# the synthetic benchmark: a latency number from a replay that hasn't
# passed ground-truth validation isn't worth reporting.
lobster-bench: lobster-test $(BUILD_DIR)/lobster_bench
	./$(BUILD_DIR)/lobster_bench $(LOBSTER_MSG)

$(BUILD_DIR)/lobster_bench: $(LOBSTER_BENCH_SRC) tools/lobster_format.h $(BUILD_DIR)/orderbook_engine.o include/orderbook_engine.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Itools -o $@ $(LOBSTER_BENCH_SRC) $(BUILD_DIR)/orderbook_engine.o

cppcheck:
	cppcheck --enable=warning,style,performance,portability \
		--suppress=missingIncludeSystem --error-exitcode=1 \
		--language=c++ --std=c++17 -Iinclude -Itools src/ bench/ tests/ tools/

clean:
	rm -rf $(BUILD_DIR)
