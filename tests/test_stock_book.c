/*
 * Tests for stock_book: sorted price-level arrays (best price at the END),
 * aggregation per price, partial vs full reductions, level removal,
 * error reporting, growth, and a randomized comparison against a model.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "stock_book.h"

static int checks_run = 0;
#define CHECK(cond) do {                                                  \
    checks_run++;                                                         \
    if (!(cond)) {                                                        \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        exit(1);                                                          \
    }                                                                     \
} while (0)

/* Dollars to ITCH fixed-point (4 implied decimals): $150.00 -> 1500000 */
static uint32_t px(double dollars) { return (uint32_t)(dollars * 10000 + 0.5); }

static level_array *side_arr(book *b, char side) {
    return side == 'B' ? &b->bids : &b->asks;
}

/* Structural rules that must hold after EVERY operation:
 *  - len <= cap
 *  - bids strictly ascending, asks strictly descending (best at the end,
 *    no duplicate prices)
 *  - no level with 0 shares or 0 orders (empty levels must be removed)
 *  - best_bid / best_ask report the last element, or 0 when empty */
static void check_structure(book *b) {
    for (int s = 0; s < 2; s++) {
        char side = s ? 'S' : 'B';
        level_array *la = side_arr(b, side);
        CHECK(la->len <= la->cap);
        for (size_t i = 0; i < la->len; i++) {
            CHECK(la->data[i].shares > 0);
            CHECK(la->data[i].order_count > 0);
            if (i > 0) {
                if (side == 'B') CHECK(la->data[i - 1].price < la->data[i].price);
                else             CHECK(la->data[i - 1].price > la->data[i].price);
            }
        }
    }
    uint32_t p = 0;
    if (b->bids.len) { CHECK(book_best_bid(b, &p) == 1); CHECK(p == b->bids.data[b->bids.len - 1].price); }
    else             { CHECK(book_best_bid(b, &p) == 0); }
    if (b->asks.len) { CHECK(book_best_ask(b, &p) == 1); CHECK(p == b->asks.data[b->asks.len - 1].price); }
    else             { CHECK(book_best_ask(b, &p) == 0); }
}

/* Find the level at `price` on `side` (simple full scan; test-only). */
static level *get(book *b, char side, uint32_t price) {
    level_array *la = side_arr(b, side);
    for (size_t i = 0; i < la->len; i++)
        if (la->data[i].price == price) return &la->data[i];
    return NULL;
}

static void expect_level(book *b, char side, uint32_t price,
                         uint32_t shares, uint32_t orders) {
    level *lv = get(b, side, price);
    CHECK(lv != NULL);
    CHECK(lv->shares == shares);
    CHECK(lv->order_count == orders);
}

/* ---- Tests -------------------------------------------------------------- */

static void test_empty(void) {
    book b;
    CHECK(book_init(&b) == 1);
    uint32_t p = 12345;
    CHECK(book_best_bid(&b, &p) == 0);
    CHECK(book_best_ask(&b, &p) == 0);
    CHECK(p == 12345);                      /* untouched when empty */
    CHECK(b.bids.len == 0 && b.asks.len == 0);
    check_structure(&b);
    book_free(&b);
}

static void test_single_bid(void) {
    book b;
    CHECK(book_init(&b));
    book_add(&b, 'B', px(150.00), 100);
    expect_level(&b, 'B', px(150.00), 100, 1);
    uint32_t p;
    CHECK(book_best_bid(&b, &p) && p == px(150.00));
    CHECK(book_best_ask(&b, &p) == 0);      /* asks untouched */
    check_structure(&b);
    book_free(&b);
}

/* Levels inserted out of order must end up sorted, best at the end. */
static void test_sorting(void) {
    book b;
    CHECK(book_init(&b));
    book_add(&b, 'B', px(150.00), 10);
    book_add(&b, 'B', px(149.90), 10);
    book_add(&b, 'B', px(150.10), 10);      /* new best */
    book_add(&b, 'B', px(149.95), 10);      /* lands in the middle */
    CHECK(b.bids.len == 4);
    CHECK(b.bids.data[0].price == px(149.90));
    CHECK(b.bids.data[1].price == px(149.95));
    CHECK(b.bids.data[2].price == px(150.00));
    CHECK(b.bids.data[3].price == px(150.10));

    book_add(&b, 'S', px(150.30), 10);
    book_add(&b, 'S', px(150.20), 10);      /* new best */
    book_add(&b, 'S', px(150.50), 10);      /* far from best: goes first */
    book_add(&b, 'S', px(150.25), 10);      /* middle */
    CHECK(b.asks.len == 4);
    CHECK(b.asks.data[0].price == px(150.50));
    CHECK(b.asks.data[1].price == px(150.30));
    CHECK(b.asks.data[2].price == px(150.25));
    CHECK(b.asks.data[3].price == px(150.20));

    uint32_t p;
    CHECK(book_best_bid(&b, &p) && p == px(150.10));
    CHECK(book_best_ask(&b, &p) && p == px(150.20));
    check_structure(&b);
    book_free(&b);
}

/* Orders at the same price share one level: shares add, count increments. */
static void test_aggregation(void) {
    book b;
    CHECK(book_init(&b));
    book_add(&b, 'B', px(150.00), 100);
    book_add(&b, 'B', px(150.00), 200);
    book_add(&b, 'B', px(150.00), 50);
    CHECK(b.bids.len == 1);
    expect_level(&b, 'B', px(150.00), 350, 3);
    /* Same price on the OTHER side is a different level entirely. */
    book_add(&b, 'S', px(150.00), 70);
    expect_level(&b, 'S', px(150.00), 70, 1);
    expect_level(&b, 'B', px(150.00), 350, 3);
    check_structure(&b);
    book_free(&b);
}

/* The illustration's sequence: partial cancel, full execution, delete. */
static void test_partial_and_full(void) {
    book b;
    CHECK(book_init(&b));
    book_add(&b, 'B', px(150.00), 100);     /* order #1 */
    book_add(&b, 'B', px(150.00), 200);     /* order #2 */
    book_add(&b, 'B', px(149.95), 70);      /* order #5 */

    /* X #2 cancel 50: partial, order stays -> count unchanged */
    CHECK(book_reduce(&b, 'B', px(150.00), 50, 0) == 1);
    expect_level(&b, 'B', px(150.00), 250, 2);

    /* E #1 executed 100: order #1 gone -> count drops */
    CHECK(book_reduce(&b, 'B', px(150.00), 100, 1) == 1);
    expect_level(&b, 'B', px(150.00), 150, 1);
    check_structure(&b);

    /* D #2 (its remaining 150): level empties and must be removed */
    CHECK(book_reduce(&b, 'B', px(150.00), 150, 1) == 1);
    CHECK(get(&b, 'B', px(150.00)) == NULL);
    CHECK(b.bids.len == 1);

    /* Best bid falls back to the next level */
    uint32_t p;
    CHECK(book_best_bid(&b, &p) && p == px(149.95));
    check_structure(&b);
    book_free(&b);
}

/* Removing the first, middle, and last (best) levels keeps order intact. */
static void test_remove_positions(void) {
    book b;
    CHECK(book_init(&b));
    for (int i = 0; i < 5; i++)
        book_add(&b, 'S', px(100.00 + i), 10);   /* asks 100..104 */
    /* array: 104 103 102 101 100 (best 100 at the end) */

    CHECK(book_reduce(&b, 'S', px(102.00), 10, 1) == 1);   /* middle */
    CHECK(book_reduce(&b, 'S', px(104.00), 10, 1) == 1);   /* first  */
    CHECK(book_reduce(&b, 'S', px(100.00), 10, 1) == 1);   /* best   */
    CHECK(b.asks.len == 2);
    CHECK(b.asks.data[0].price == px(103.00));
    CHECK(b.asks.data[1].price == px(101.00));
    uint32_t p;
    CHECK(book_best_ask(&b, &p) && p == px(101.00));
    check_structure(&b);

    CHECK(book_reduce(&b, 'S', px(101.00), 10, 1) == 1);
    CHECK(book_reduce(&b, 'S', px(103.00), 10, 1) == 1);
    CHECK(b.asks.len == 0);
    CHECK(book_best_ask(&b, &p) == 0);
    check_structure(&b);
    book_free(&b);
}

/* Inconsistent reductions must return 0 and leave the book unchanged. */
static void test_errors(void) {
    book b;
    CHECK(book_init(&b));
    book_add(&b, 'B', px(150.00), 100);

    /* Level does not exist */
    CHECK(book_reduce(&b, 'B', px(149.00), 10, 0) == 0);
    /* Right price, wrong side */
    CHECK(book_reduce(&b, 'S', px(150.00), 10, 0) == 0);
    /* More shares than the level holds: would go below zero */
    CHECK(book_reduce(&b, 'B', px(150.00), 101, 1) == 0);

    expect_level(&b, 'B', px(150.00), 100, 1);   /* nothing changed */
    CHECK(b.bids.len == 1 && b.asks.len == 0);
    check_structure(&b);
    book_free(&b);
}

/* Many distinct levels force the arrays to grow past their initial size. */
static void test_growth(void) {
    book b;
    CHECK(book_init(&b));
    const int n = 5000;
    /* Insert in a scrambled order: i * 7919 mod n visits every value once. */
    for (int i = 0; i < n; i++) {
        int k = (int)(((long)i * 7919) % n);
        book_add(&b, 'B', 1000000 + (uint32_t)k * 100, (uint32_t)k + 1);
        book_add(&b, 'S', 2000000 + (uint32_t)k * 100, (uint32_t)k + 1);
    }
    CHECK(b.bids.len == (size_t)n && b.asks.len == (size_t)n);
    for (int k = 0; k < n; k++) {
        expect_level(&b, 'B', 1000000 + (uint32_t)k * 100, (uint32_t)k + 1, 1);
        expect_level(&b, 'S', 2000000 + (uint32_t)k * 100, (uint32_t)k + 1, 1);
    }
    uint32_t p;
    CHECK(book_best_bid(&b, &p) && p == 1000000 + (uint32_t)(n - 1) * 100);
    CHECK(book_best_ask(&b, &p) && p == 2000000);
    check_structure(&b);
    book_free(&b);
}

/* Prices seen in real data: $0.0001 and the $199,999.99 far-away asks. */
static void test_extreme_prices(void) {
    book b;
    CHECK(book_init(&b));
    book_add(&b, 'B', px(35.00), 100);
    book_add(&b, 'B', 1, 5);                     /* $0.0001 */
    book_add(&b, 'S', px(35.05), 100);
    book_add(&b, 'S', 1999999900u, 100);         /* $199,999.99 */

    uint32_t p;
    CHECK(book_best_bid(&b, &p) && p == px(35.00));   /* extremes never best */
    CHECK(book_best_ask(&b, &p) && p == px(35.05));
    CHECK(b.bids.data[0].price == 1);
    CHECK(b.asks.data[0].price == 1999999900u);
    check_structure(&b);

    /* Once the normal levels go, the extremes become best */
    CHECK(book_reduce(&b, 'B', px(35.00), 100, 1) == 1);
    CHECK(book_reduce(&b, 'S', px(35.05), 100, 1) == 1);
    CHECK(book_best_bid(&b, &p) && p == 1);
    CHECK(book_best_ask(&b, &p) && p == 1999999900u);
    check_structure(&b);
    book_free(&b);
}

/* ---- Randomized comparison against a simple model -----------------------
 * We simulate real orders (so every reduction is valid), and keep the
 * expected shares and order count per (side, price) in plain arrays. */
static uint64_t rng_state;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

enum { PRICES = 64, MAX_LIVE = 3000 };
#define BASE 1500000u                 /* $150.00 */
#define TICK 100u                     /* $0.01   */

typedef struct { int side; int k; uint32_t shares; } sim_order;

static void check_against_model(book *b, uint64_t m_sh[2][PRICES], uint32_t m_n[2][PRICES]) {
    check_structure(b);
    for (int s = 0; s < 2; s++) {
        char side = s ? 'S' : 'B';
        size_t expected_levels = 0;
        int best = -1;
        for (int k = 0; k < PRICES; k++) {
            uint32_t price = BASE + (uint32_t)k * TICK;
            level *lv = get(b, side, price);
            if (m_n[s][k] == 0) {
                CHECK(lv == NULL);           /* empty level was removed */
            } else {
                expected_levels++;
                CHECK(lv != NULL);
                CHECK(lv->shares == m_sh[s][k]);
                CHECK(lv->order_count == m_n[s][k]);
                if (side == 'B' ? (best < 0 || k > best) : (best < 0 || k < best))
                    best = k;
            }
        }
        CHECK(side_arr(b, side)->len == expected_levels);
        uint32_t p;
        int has = side == 'B' ? book_best_bid(b, &p) : book_best_ask(b, &p);
        if (best < 0) CHECK(has == 0);
        else          CHECK(has == 1 && p == BASE + (uint32_t)best * TICK);
    }
}

static void test_random_vs_model(uint64_t seed) {
    static sim_order live[MAX_LIVE];
    static uint64_t m_sh[2][PRICES];
    static uint32_t m_n[2][PRICES];
    memset(m_sh, 0, sizeof m_sh);
    memset(m_n, 0, sizeof m_n);
    size_t nlive = 0;
    rng_state = seed;

    book b;
    CHECK(book_init(&b));
    const int OPS = 200000;
    for (int op = 1; op <= OPS; op++) {
        bool do_add = nlive == 0 || (nlive < MAX_LIVE && rng() % 100 < 52);
        if (do_add) {
            sim_order o = { (int)(rng() % 2), (int)(rng() % PRICES),
                            (uint32_t)(1 + rng() % 500) };
            book_add(&b, o.side ? 'S' : 'B', BASE + (uint32_t)o.k * TICK, o.shares);
            m_sh[o.side][o.k] += o.shares;
            m_n[o.side][o.k] += 1;
            live[nlive++] = o;
        } else {
            size_t j = (size_t)(rng() % nlive);
            sim_order *o = &live[j];
            uint32_t q;
            int gone;
            if (o->shares > 1 && rng() % 2) {        /* partial reduction */
                q = 1 + (uint32_t)(rng() % (o->shares - 1));
                gone = 0;
            } else {                                  /* order leaves */
                q = o->shares;
                gone = 1;
            }
            CHECK(book_reduce(&b, o->side ? 'S' : 'B',
                              BASE + (uint32_t)o->k * TICK, q, gone) == 1);
            m_sh[o->side][o->k] -= q;
            if (gone) m_n[o->side][o->k] -= 1;
            o->shares -= q;
            if (gone) live[j] = live[--nlive];       /* swap-remove */
        }
        if (op % 499 == 0 || op == OPS)
            check_against_model(&b, m_sh, m_n);
    }

    /* Drain everything: the book must end completely empty. */
    while (nlive > 0) {
        sim_order *o = &live[nlive - 1];
        CHECK(book_reduce(&b, o->side ? 'S' : 'B',
                          BASE + (uint32_t)o->k * TICK, o->shares, 1) == 1);
        m_sh[o->side][o->k] -= o->shares;
        m_n[o->side][o->k] -= 1;
        nlive--;
    }
    check_against_model(&b, m_sh, m_n);
    CHECK(b.bids.len == 0 && b.asks.len == 0);
    book_free(&b);
}

int main(int argc, char **argv) {
    uint64_t seed = argc > 1 ? strtoull(argv[1], NULL, 10) : 42;
    if (seed == 0) seed = 42;

    test_empty();
    test_single_bid();
    test_sorting();
    test_aggregation();
    test_partial_and_full();
    test_remove_positions();
    test_errors();
    test_growth();
    test_extreme_prices();
    test_random_vs_model(seed);

    printf("all stock_book tests passed (%d checks, seed %llu)\n",
           checks_run, (unsigned long long)seed);
    return 0;
}
