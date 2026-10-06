/*
 * Tests for order_table: open addressing, linear probing,
 * backward-shift deletion, and growth.
 *
 * Build: see the test_order_table target in the Makefile.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "order_table.h"

/* ---- Minimal test helpers ---------------------------------------------
 * CHECK is used instead of assert() so it is never compiled out and
 * reports the exact line that failed. */
static int checks_run = 0;
#define CHECK(cond) do {                                                  \
    checks_run++;                                                         \
    if (!(cond)) {                                                        \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        exit(1);                                                          \
    }                                                                     \
} while (0)

/* Must match slot_for() in src/order_table.c. Used only to build keys
 * that collide on purpose. If you change the hash, change it here too. */
static size_t test_slot_for(uint64_t ref, size_t cap) {
    return (size_t)((ref * 0x9E3779B97F4A7C15ULL) >> 32) & (cap - 1);
}

/* Find the n-th key (starting search at `from`) whose home slot is `slot`. */
static uint64_t key_with_home(size_t slot, size_t cap, uint64_t *from) {
    for (uint64_t k = *from; ; k++) {
        if (test_slot_for(k, cap) == slot) { *from = k + 1; return k; }
    }
}

/* Every field is derived from the key, so we can detect entries whose
 * data got lost or mixed up when shifted during delete or copied on grow. */
static void fill(order *o, uint64_t ref) {
    o->price        = (uint32_t)(ref * 3 + 1);
    o->shares       = (uint32_t)(ref + 7);
    o->stock_locate = (uint16_t)(ref % 9000);
    o->side         = (ref & 1) ? 'B' : 'S';
}
static bool fields_ok(const order *o, uint64_t ref) {
    return o->order_ref    == ref &&
           o->price        == (uint32_t)(ref * 3 + 1) &&
           o->shares       == (uint32_t)(ref + 7) &&
           o->stock_locate == (uint16_t)(ref % 9000) &&
           o->side         == ((ref & 1) ? 'B' : 'S');
}
static void put(order_table *t, uint64_t ref) {
    order *o = order_table_insert(t, ref);
    CHECK(o != NULL);
    CHECK(o->order_ref == ref);
    fill(o, ref);
}
static long slot_of(order_table *t, uint64_t ref) {
    order *o = order_table_find(t, ref);
    return o ? (long)(o - t->slots) : -1;
}

/* The core invariant of linear probing: walking from an entry's home slot
 * to where it actually sits never crosses an empty slot. If this breaks,
 * find() will miss the entry. Also checks count matches occupied slots. */
static void check_invariants(order_table *t) {
    size_t mask = t->capacity - 1, occupied = 0;
    CHECK(t->capacity > 0 && (t->capacity & mask) == 0);
    for (size_t j = 0; j < t->capacity; j++) {
        uint64_t ref = t->slots[j].order_ref;
        if (ref == 0) continue;
        occupied++;
        for (size_t i = test_slot_for(ref, t->capacity); i != j; i = (i + 1) & mask)
            CHECK(t->slots[i].order_ref != 0);          /* no gap on the path */
        CHECK(order_table_find(t, ref) == &t->slots[j]); /* findable, unique */
        CHECK(fields_ok(&t->slots[j], ref));             /* data intact */
    }
    CHECK(occupied == t->count);
    CHECK(t->count * 10 <= t->capacity * 7);             /* load <= 70% */
}

/* ---- Tests -------------------------------------------------------------- */

static void test_init(void) {
    order_table t;
    CHECK(order_table_init(&t, 0) == 0);
    CHECK(order_table_init(&t, 6) == 0);        /* not a power of two */
    CHECK(order_table_init(&t, 1000) == 0);
    CHECK(order_table_init(&t, 8) == 1);
    CHECK(t.capacity == 8 && t.count == 0);
    for (uint64_t k = 1; k <= 100; k++)
        CHECK(order_table_find(&t, k) == NULL);
    order_table_free(&t);
}

static void test_insert_find(void) {
    order_table t;
    CHECK(order_table_init(&t, 16));
    for (uint64_t k = 1; k <= 5; k++) put(&t, k);
    CHECK(t.count == 5);
    for (uint64_t k = 1; k <= 5; k++) {
        order *o = order_table_find(&t, k);
        CHECK(o != NULL && fields_ok(o, k));
    }
    CHECK(order_table_find(&t, 99) == NULL);
    check_invariants(&t);
    order_table_free(&t);
}

/* Three keys that all hash to the LAST slot must wrap around to 0 and 1. */
static void test_wraparound(void) {
    order_table t;
    const size_t cap = 16;
    CHECK(order_table_init(&t, cap));
    uint64_t from = 1;
    uint64_t a = key_with_home(cap - 1, cap, &from);
    uint64_t b = key_with_home(cap - 1, cap, &from);
    uint64_t c = key_with_home(cap - 1, cap, &from);
    put(&t, a); put(&t, b); put(&t, c);

    CHECK(slot_of(&t, a) == 15);
    CHECK(slot_of(&t, b) == 0);                 /* wrapped */
    CHECK(slot_of(&t, c) == 1);
    check_invariants(&t);

    order_table_remove(&t, b);                  /* delete across the wrap */
    CHECK(order_table_find(&t, b) == NULL);
    CHECK(slot_of(&t, c) == 0);                 /* shifted back over the wrap */
    check_invariants(&t);

    order_table_remove(&t, a);
    CHECK(slot_of(&t, c) == 15);                /* shifted back to its home */
    check_invariants(&t);
    order_table_free(&t);
}

/* The scenario from the design discussion:
 *   slot:  5  6  7  8
 *   keys:  a  b  c  d     (a, b, c home 5; d home 7)
 * Deleting b must move c back to 6 and d back to 7. */
static void test_remove_middle_of_run(void) {
    order_table t;
    const size_t cap = 16;
    CHECK(order_table_init(&t, cap));
    uint64_t from = 1;
    uint64_t a = key_with_home(5, cap, &from);
    uint64_t b = key_with_home(5, cap, &from);
    uint64_t c = key_with_home(5, cap, &from);
    from = 1;
    uint64_t d = key_with_home(7, cap, &from);
    put(&t, a); put(&t, b); put(&t, c); put(&t, d);
    CHECK(slot_of(&t, d) == 8);
    check_invariants(&t);

    order_table_remove(&t, b);
    CHECK(t.count == 3);
    CHECK(order_table_find(&t, b) == NULL);
    CHECK(slot_of(&t, a) == 5);
    CHECK(slot_of(&t, c) == 6);
    CHECK(slot_of(&t, d) == 7);
    CHECK(t.slots[8].order_ref == 0);
    check_invariants(&t);
    order_table_free(&t);
}

/* An entry already at its home slot must NOT be moved back before it. */
static void test_no_move_before_home(void) {
    order_table t;
    const size_t cap = 16;
    CHECK(order_table_init(&t, cap));
    uint64_t from = 1;
    uint64_t a = key_with_home(3, cap, &from);
    uint64_t b = key_with_home(3, cap, &from);  /* sits at 4 */
    from = 1;
    uint64_t c = key_with_home(5, cap, &from);  /* sits at its home, 5 */
    put(&t, a); put(&t, b); put(&t, c);
    order_table_remove(&t, b);
    CHECK(slot_of(&t, c) == 5);                 /* stays put */
    CHECK(t.slots[4].order_ref == 0);
    check_invariants(&t);
    order_table_free(&t);
}

static void test_remove_absent(void) {
    order_table t;
    CHECK(order_table_init(&t, 16));
    put(&t, 10); put(&t, 20);
    order_table_remove(&t, 999);                /* never inserted */
    CHECK(t.count == 2);
    order_table_remove(&t, 10);
    order_table_remove(&t, 10);                 /* already removed */
    CHECK(t.count == 1);
    CHECK(order_table_find(&t, 20) != NULL);
    check_invariants(&t);
    order_table_free(&t);
}

static void test_grow(void) {
    order_table t;
    CHECK(order_table_init(&t, 8));
    const uint64_t n = 100000;
    for (uint64_t k = 1; k <= n; k++) put(&t, k);
    CHECK(t.count == n);
    CHECK(t.capacity > 8);
    for (uint64_t k = 1; k <= n; k++) {
        order *o = order_table_find(&t, k);
        CHECK(o != NULL && fields_ok(o, k));
    }
    CHECK(order_table_find(&t, n + 1) == NULL);
    check_invariants(&t);
    order_table_free(&t);
}

/* Randomized differential test: apply random inserts and removes to both
 * the table and a trivially correct model (a bool array), and require
 * them to agree. A small key range forces many collisions and reuse. */
static uint64_t rng_state;
static uint64_t rng(void) {                       /* xorshift64 */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void test_random_vs_model(uint64_t seed) {
    enum { KEYS = 600, OPS = 300000, VERIFY_EVERY = 997 };
    static bool present[KEYS + 1];
    memset(present, 0, sizeof present);
    size_t model_count = 0;
    rng_state = seed;

    order_table t;
    CHECK(order_table_init(&t, 8));             /* start tiny: forces growth */

    for (int op = 1; op <= OPS; op++) {
        uint64_t k = 1 + rng() % KEYS;
        bool insert = (rng() % 100) < 55;
        if (insert && !present[k]) {
            put(&t, k);
            present[k] = true;
            model_count++;
        } else if (!insert) {
            order_table_remove(&t, k);
            if (present[k]) { present[k] = false; model_count--; }
        }
        CHECK(t.count == model_count);

        if (op % VERIFY_EVERY == 0 || op == OPS) {
            for (uint64_t q = 1; q <= KEYS; q++) {
                order *o = order_table_find(&t, q);
                if (present[q]) CHECK(o != NULL && fields_ok(o, q));
                else            CHECK(o == NULL);
            }
            check_invariants(&t);
        }
    }
    order_table_free(&t);
}

int main(int argc, char **argv) {
    uint64_t seed = argc > 1 ? strtoull(argv[1], NULL, 10) : 42;
    if (seed == 0) seed = 42;

    test_init();
    test_insert_find();
    test_wraparound();
    test_remove_middle_of_run();
    test_no_move_before_home();
    test_remove_absent();
    test_grow();
    test_random_vs_model(seed);

    printf("all order_table tests passed (%d checks, seed %llu)\n",
           checks_run, (unsigned long long)seed);
    return 0;
}
