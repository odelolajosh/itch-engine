#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stock_book.h"

#ifdef SCAN_STATS
/* [0] = 0 unit (work or depth), [b] = 2^(b-1) .. 2^b - 1 unit */
static uint64_t scan_hist_work[65], scan_hist_depth[65];
static uint64_t scan_calls, scan_work_total, scan_depth_total;
#endif

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

#define LINEAR_PROBE 8   /* from the histogram: ~77% of searches end within 7 steps */

static inline int better(uint32_t level_price, uint32_t price, char side)
{
    return side == 'B' ? level_price > price : level_price < price;
}

#ifdef SCAN_STATS
#define SCAN_STEP() (scan_work++)
#else
#define SCAN_STEP() ((void)0)
#endif

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
    size_t len  = la->len;
    size_t i = len;
    size_t stop = len > LINEAR_PROBE ? len - LINEAR_PROBE : 0;
    int found = 0;
#ifdef SCAN_STATS
    size_t scan_work = 0;
#endif

    // Fast path
    while (i > stop && (SCAN_STEP(), better(la->data[i - 1].price, price, side)))
        i--;

    // Binary search for the slow path
    if (i == stop && i > 0)
    {
        size_t lo = 0, hi = stop;
        while (lo < hi)
        {
            size_t mid = lo + (hi - lo) / 2;
            SCAN_STEP();
            if (better(la->data[mid].price, price, side))
                hi = mid;
            else
                lo = mid + 1;
        }
        i = lo;
    }

    // finds the exact price level
    if (i > 0 && la->data[i - 1].price == price)
    {
        *pos = --i;
        found = 1;
    }
    else
        *pos = i;

#ifdef SCAN_STATS
    size_t depth = len - i;
    scan_calls++;
    scan_depth_total += depth;
    scan_work_total  += scan_work;
    scan_hist_depth[depth ? 64 - __builtin_clzll(depth) : 0]++;
    scan_hist_work [scan_work ? 64 - __builtin_clzll(scan_work) : 0]++;
#endif

    return found;
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

#ifdef SCAN_STATS
#include <stdio.h>

/* Bucket b holds values in [lo, hi]. b == 0 is exactly 0; b == 64 tops out at UINT64_MAX. */
static void scan_bucket_range(int b, unsigned long long *lo, unsigned long long *hi)
{
    *lo = b ? 1ULL << (b - 1) : 0;
    *hi = b == 0 ? 0 : b == 64 ? ~0ULL : (1ULL << b) - 1;
}

/* Upper bound of the bucket containing the q-th quantile (approximate, bucket resolution). */
static unsigned long long scan_percentile(const uint64_t *hist, uint64_t n, double q)
{
    uint64_t target = (uint64_t)(q * (double)n);
    uint64_t cum = 0;
    unsigned long long lo, hi = 0;

    if (target == 0)
        target = 1;
    for (int b = 0; b < 65; b++) {
        cum += hist[b];
        if (cum >= target) {
            scan_bucket_range(b, &lo, &hi);
            return hi;
        }
    }
    return hi;
}

void book_print_scan_stats(FILE *out)
{
    static const struct { const char *name; double q; } pct[] = {
        { "p50", 0.50 }, { "p90", 0.90 }, { "p99", 0.99 }, { "p99.9", 0.999 },
    };

    fprintf(out, "\n== Level search ==\n");
    fprintf(out, "  calls                  %llu\n", (unsigned long long)scan_calls);
    if (scan_calls == 0)
        return;

    fprintf(out, "  linear probe           last %d levels, then binary search\n", LINEAR_PROBE);
    fprintf(out, "  depth = levels from best to target; work = levels actually compared\n\n");

    fprintf(out, "  %-22s %10s  %10s\n", "", "depth", "work");
    fprintf(out, "  %-22s %10.2f  %10.2f\n", "avg steps",
            (double)scan_depth_total / scan_calls,
            (double)scan_work_total  / scan_calls);
    for (size_t k = 0; k < sizeof pct / sizeof pct[0]; k++) {
        char label[32];
        snprintf(label, sizeof label, "%s steps <=", pct[k].name);
        fprintf(out, "  %-22s %10llu  %10llu\n", label,
                scan_percentile(scan_hist_depth, scan_calls, pct[k].q),
                scan_percentile(scan_hist_work,  scan_calls, pct[k].q));
    }

    int last = 0;
    for (int b = 0; b < 65; b++)
        if (scan_hist_depth[b] || scan_hist_work[b])
            last = b;

    fprintf(out, "\n  %-13s  %12s %6s %6s   %12s %6s %6s\n",
            "steps", "depth", "%", "cum%", "work", "%", "cum%");

    uint64_t cum_depth = 0, cum_work = 0;
    for (int b = 0; b <= last; b++) {
        unsigned long long lo, hi;
        char label[48];

        scan_bucket_range(b, &lo, &hi);
        if (lo == hi)
            snprintf(label, sizeof label, "%llu", lo);
        else
            snprintf(label, sizeof label, "%llu-%llu", lo, hi);

        cum_depth += scan_hist_depth[b];
        cum_work  += scan_hist_work[b];

        fprintf(out, "  %-13s  %12llu %5.1f%% %5.1f%%   %12llu %5.1f%% %5.1f%%\n",
                label,
                (unsigned long long)scan_hist_depth[b],
                100.0 * scan_hist_depth[b] / scan_calls,
                100.0 * cum_depth / scan_calls,
                (unsigned long long)scan_hist_work[b],
                100.0 * scan_hist_work[b] / scan_calls,
                100.0 * cum_work / scan_calls);
    }
}
#endif
