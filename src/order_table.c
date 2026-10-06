#include "order_table.h"
#include <stdlib.h>

int order_table_grow(order_table *t);

static inline size_t slot_for(uint64_t ref, size_t cap);
static inline size_t modulo(size_t v, size_t cap);
static inline int power_of_two(size_t n);

int order_table_init(order_table *t, size_t capacity)
{
    if (!power_of_two(capacity))
        return 0;

    t->slots = calloc(capacity, sizeof(order));
    if (!t->slots)
        return 0;

    t->capacity = capacity;
    t->count = 0;
    return 1;
}

/* order_table_insert: returns slot for order */
order *order_table_insert(order_table *t, uint64_t ref)
{
    if ((t->count + 1) * 10 > 7 * t->capacity)
        if (!order_table_grow(t)) return NULL;

    size_t i = slot_for(ref, t->capacity);
    while (t->slots[i].order_ref != 0)
        i = modulo(i + 1, t->capacity);

    t->slots[i].order_ref = ref;
    t->count++;
    return &(t->slots[i]);
}

/* order_table_find: retrieves an order by its reference */
order *order_table_find(order_table *t, uint64_t ref)
{
    size_t i = slot_for(ref, t->capacity);

    while (t->slots[i].order_ref != 0)
    {
        if (t->slots[i].order_ref == ref)
            return &t->slots[i];

        i = modulo(i + 1, t->capacity);
    }

    return NULL;
}

void order_table_remove(order_table *t, uint64_t ref)
{
    size_t i = slot_for(ref, t->capacity);

    while (t->slots[i].order_ref != ref)
    {
        if (t->slots[i].order_ref == 0)
            break;
        i = modulo(i + 1, t->capacity);
    }

    if (t->slots[i].order_ref == 0)
        return;

    size_t j = i;
    for (;;)
    {
        j = modulo(j + 1, t->capacity);
        if (t->slots[j].order_ref == 0)
            break;

        size_t home = slot_for(t->slots[j].order_ref, t->capacity);
        size_t probe_dist = modulo(j - home, t->capacity);
        size_t gap_dist = modulo(j - i, t->capacity);
        if (probe_dist >= gap_dist)
        {
            t->slots[i] = t->slots[j];
            i = j;
        }
    }

    t->slots[i].order_ref = 0;
    t->count--;
}

void order_table_free(order_table *t)
{
    free(t->slots);
}

int order_table_grow(order_table *t)
{
    order_table new_t;
    order *slot;
    size_t new_cap;

    if (!t) return 0;

    new_cap = t->capacity << 1;
    if (!order_table_init(&new_t, new_cap))
        return 0;

    for (size_t i = 0; i < t->capacity; i++)
        if (t->slots[i].order_ref != 0 && (slot = order_table_insert(&new_t, t->slots[i].order_ref)))
            *slot = t->slots[i];

    order_table_free(t);
    *t = new_t;
    return 1;
}

static inline size_t slot_for(uint64_t ref, size_t cap)
{
    return modulo((size_t)((ref * 0x9E3779B97F4A7C15ULL) >> 32), cap);
}

static inline size_t modulo(size_t n, size_t cap)
{
    /*
     * Works specifically for `cap` that is a power of 2.
     *
     * For a power of two, cap
     * cap = 100..00 and cap-1 = 011..11
     *
     * ANDing any number n with cap-1 keeps the lowests log(cap)-1 bits
     * and unsets the higher bits.
     */
    return n & (cap - 1);
}

static inline int power_of_two(size_t n)
{
    /*
     * Works specifically for `cap` that is a power of 2.
     * So that n = 100..00 and n-1 = 011..11
     * Therefore for any number n that is a power of 2, n & (n-1) = 0
     */
    return n != 0 && (n & (n - 1)) == 0;
}
