#ifndef ORDER_TABLE_H
#define ORDER_TABLE_H

#include <stdint.h>
#include <stddef.h>

typedef struct
{
    uint64_t order_ref;
    uint32_t price;
    uint32_t shares;
    uint16_t stock_locate;
    char     side;
} order;

typedef struct
{
    order  *slots;
    size_t  capacity; // always a power of two
    size_t  count;
} order_table;

int order_table_init(order_table *t, size_t capacity);
order *order_table_insert(order_table *t, uint64_t ref);
order *order_table_find(order_table *t, uint64_t ref);
void order_table_remove(order_table *t, uint64_t ref);
void order_table_free(order_table *t);
void order_table_prefetch(order_table *t, uint64_t ref);

#endif
