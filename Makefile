CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra -Wpedantic -std=c11 -Iinclude
SAN_CC   ?= clang
SAN_FLAGS = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Iinclude

SRC       = src/orderbook_engine.c
TEST_SRC  = tests/test_correctness.c
BENCH_SRC = bench/benchmark.c

BUILD_DIR = build

DEPTHS = 10 25 50 100 200 400

.PHONY: all test bench asan clean cppcheck depth-sweep

all: $(BUILD_DIR)/test_correctness $(BUILD_DIR)/benchmark

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_correctness: $(TEST_SRC) $(SRC) include/orderbook_engine.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(SRC)

$(BUILD_DIR)/benchmark: $(BENCH_SRC) $(SRC) include/orderbook_engine.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(BENCH_SRC) $(SRC)

# Correctness first, always — a benchmark number from an engine that
# hasn't passed the replay+regression tests isn't worth reporting.
test: $(BUILD_DIR)/test_correctness
	./$(BUILD_DIR)/test_correctness

bench: test $(BUILD_DIR)/benchmark
	./$(BUILD_DIR)/benchmark

# Uses clang, not $(CC): gcc's ASan/UBSan runtime libraries are not
# installed on every machine this was developed on, clang's are.
asan: | $(BUILD_DIR)
	$(SAN_CC) $(SAN_FLAGS) -o $(BUILD_DIR)/test_correctness_asan $(TEST_SRC) $(SRC)
	./$(BUILD_DIR)/test_correctness_asan

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

cppcheck:
	cppcheck --enable=warning,style,performance,portability \
		--suppress=missingIncludeSystem --error-exitcode=1 \
		-Iinclude src/ bench/ tests/

clean:
	rm -rf $(BUILD_DIR)
