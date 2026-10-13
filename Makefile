CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS = -Iinclude

SRC  = src/decode.c src/order_table.c src/stock_book.c
HDRS = $(wildcard include/*.h)
SAN  = -O1 -g -Wall -Wextra -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer

BINS  = itch_stats itch_stats_scan itch_stats_prof
TESTS = test_decode test_order_table test_stock_book

.PHONY: all test asan stats prof clean

all: itch_stats

# Benchmark binary: the only one whose timings go in the README.
itch_stats: src/main.c $(SRC) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/main.c $(SRC)

# Instrumented build: level-search histogram. Never use its timings.
itch_stats_scan: src/main.c $(SRC) $(HDRS)
	$(CC) $(CPPFLAGS) -DSCAN_STATS $(CFLAGS) -o $@ src/main.c $(SRC)

# Profiling build: frame pointers so perf call graphs are correct.
itch_stats_prof: src/main.c $(SRC) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fno-omit-frame-pointer -o $@ src/main.c $(SRC)

stats: itch_stats_scan
prof: itch_stats_prof

test_decode: tests/test_decode.c $(SRC) $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_decode.c $(SRC)

test_order_table: tests/test_order_table.c src/order_table.c $(HDRS)
	$(CC) $(CPPFLAGS) $(SAN) -o $@ tests/test_order_table.c src/order_table.c

test_stock_book: tests/test_stock_book.c src/stock_book.c $(HDRS)
	$(CC) $(CPPFLAGS) $(SAN) -o $@ tests/test_stock_book.c src/stock_book.c

test: $(TESTS)
	./test_decode
	./test_order_table
	./test_stock_book

# Build with AddressSanitizer + UBSan to catch out-of-bounds reads early.
asan: CFLAGS = $(SAN)
asan: clean itch_stats test_decode
	./test_decode

clean:
	rm -f $(BINS) $(TESTS)
