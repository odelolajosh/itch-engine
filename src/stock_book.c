#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stock_book.h"

#define INIT_LEVEL_SIZE 8

int level_array_init(level_array *la)
{
    la->data = calloc(INIT_LEVEL_SIZE, sizeof(level));
    if (!la->data) return 0;

    la->len = 0;
    la->cap = INIT_LEVEL_SIZE;
    return 1;
}

void level_array_free(level_array *la)
{
    free(la->data);
    la->data = NULL;
    la->len = 0;
    la->cap = 0;
}

int level_array_grow(level_array *la)
{
    size_t new_cap = la->cap ? la->cap * 2 : INIT_LEVEL_SIZE;
    level *new_data = realloc(la->data, new_cap * sizeof(level));
    if (!new_data) return 0;

    la->data = new_data;
    la->cap = new_cap;
    return 1;
}

/* level_find: search for a level at `price`; returns 1 and `pos` if such price
 * level is found or 0 if not.
 *
 * Run a linear scan from the end of the array, since the levels are
 * sorted such that best bid and asks are at the end of the array.
 * This is based on the assumption that most activity will be around
 * the best bids and asks.
 *
 * Performance drops when activity price drifts far from the best levels.
 * Another way is to use binary search which gives a consistency time
 * complexity of O(logn). This could worth a try for benchmarking.
 */
static int level_find(level_array *la, uint32_t price, char side, size_t *pos)
{
    size_t i = la->len;

    while (i > 0 && (side == 'B' ? la->data[i - 1].price > price : la->data[i - 1].price < price))
        i--;

    // finds the exact price level
    if (i > 0 && la->data[i - 1].price == price) {
        *pos = i - 1;
        return 1;
    }

    *pos = i;
    return 0;
}

level *level_insert(level_array *la, uint32_t price, size_t pos)
{
    if (la->len + 1 > la->cap)
        level_array_grow(la);

    memmove(&la->data[pos + 1], &la->data[pos], (la->len - pos) * sizeof(level));

    la->data[pos].price = price;
    la->data[pos].shares = 0;
    la->data[pos].order_count = 0;
    la->len++;
    return &la->data[pos];
}

void level_remove(level_array *la, size_t pos)
{
    memmove(&la->data[pos], &la->data[pos + 1], (la->len - pos - 1) * sizeof(level));

    la->data[la->len - 1].price = 0;
    la->data[la->len - 1].shares = 0;
    la->data[la->len - 1].order_count = 0;
    la->len--;
}


int book_init(book *b)
{
    if(!level_array_init(&b->bids))
        return 0;

    if(!level_array_init(&b->asks))
    {
        level_array_free(&b->bids);
        return 0;
    }

    return 1;
}

void book_free(book *b)
{
    if (!b) return;

    level_array_free(&b->bids);
    level_array_free(&b->asks);
}

void book_add(book *b, char side, uint32_t price, uint32_t shares)
{
    level_array *la = (side == 'B') ? &b->bids : &b->asks;

    size_t pos;
    level *lv = NULL;

    if (level_find(la, price, side, &pos))
        lv = &la->data[pos];
    else if (!(lv = level_insert(la, price, pos)))
        return;

    lv->shares += shares;
    lv->order_count += 1;
}

/* returns 1 on success, 0 on an inconsistency */
int book_reduce(book *b, char side, uint32_t price, uint32_t shares, int order_gone)
{
    level_array *la = (side == 'B') ? &b->bids : &b->asks;

    size_t pos;
    if (!(level_find(la, price, side, &pos)))
        return 0;

    level *lv = &la->data[pos];

    if (lv->shares < shares || (order_gone && lv->order_count == 0))
        return 0;

    lv->shares -= shares;
    if (order_gone)
        lv->order_count -= 1;

    // check if share and order count disagree
    // if total shares is zero, order_count must be zero
    if ((lv->shares == 0) != (lv->order_count == 0))
        return 0;

    if (lv->shares == 0)
        level_remove(la, pos);

    return 1;
}

int book_best_bid(const book *b, uint32_t *price)
{
    if (b->bids.len == 0)
        return 0;
    *price = b->bids.data[b->bids.len - 1].price;
    return 1;
}

int book_best_ask(const book *b, uint32_t *price)
{
    if (b->asks.len == 0)
        return 0;
    *price = b->asks.data[b->asks.len - 1].price;
    return 1;
}
