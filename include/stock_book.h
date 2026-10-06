#ifndef STOCK_BOOK_H
#define STOCK_BOOK_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t shares;
    uint32_t price;
    uint32_t order_count;
} level;

typedef struct {
    level  *data;
    size_t  len;
    size_t  cap;
} level_array;

typedef struct {
    char symbols[8];
    level_array bids;
    level_array asks;
} book;

int book_init(book *b);
void book_free(book *b);
void book_add(book *b, char side, uint32_t price, uint32_t shares);
int book_reduce(book *b, char side, uint32_t price, uint32_t shares, int order_gone);
int book_best_bid(const book *b, uint32_t *price);
int book_best_ask(const book *b, uint32_t *price);

#endif
