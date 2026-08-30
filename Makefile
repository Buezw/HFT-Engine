CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra -Wpedantic -std=c11 -Iinclude
SAN_CC   ?= clang
SAN_FLAGS = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Iinclude

SRC       = src/orderbook_engine.c
TEST_SRC  = tests/test_correctness.c
BENCH_SRC = bench/benchmark.c

BUILD_DIR = build

.PHONY: all test bench asan clean cppcheck

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

cppcheck:
	cppcheck --enable=warning,style,performance,portability \
		--suppress=missingIncludeSystem --error-exitcode=1 \
		-Iinclude src/ bench/ tests/

clean:
	rm -rf $(BUILD_DIR)
