CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS = -Iinclude

SRC = src/decode.c src/order_table.c src/stock_book.c

.PHONY: all test asan clean

all: itch_stats

itch_stats: src/main.c $(SRC) include/itch.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/main.c $(SRC)

test_decode: tests/test_decode.c $(SRC) include/itch.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_decode.c $(SRC)

test_order_table: tests/test_order_table.c src/order_table.c include/order_table.h
	$(CC) $(CPPFLAGS) -O1 -g -Wall -Wextra -std=c11 -fsanitize=address,undefined -o $@ tests/test_order_table.c src/order_table.c

test_stock_book: tests/test_stock_book.c src/stock_book.c include/stock_book.h
	$(CC) $(CPPFLAGS) -O1 -g -Wall -Wextra -std=c11 -fsanitize=address,undefined -o $@ tests/test_stock_book.c src/stock_book.c

test: test_decode
	./test_decode

# Build with AddressSanitizer + UBSan to catch out-of-bounds reads early.
asan: CFLAGS = -O1 -g -Wall -Wextra -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean itch_stats test_decode
	./test_decode

clean:
	rm -f itch_stats test_decode
