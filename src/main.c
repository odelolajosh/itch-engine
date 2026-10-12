/*
 * itch_stats: memory-map an uncompressed ITCH 5.0 file, walk every message,
 * validate framing, and report per-type counts and throughput.
 *
 * File format: repeated [2-byte big-endian length][message of that length].
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "itch.h"
#include "stock_book.h"
#include "order_table.h"

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#define MAX_LOCATE (1 << 16)
const uint64_t NS_PER_DAY = 86400ULL * 1000000000ULL;

typedef struct {
    order_table orders;
    book *books;

    uint64_t counts[256];
    uint64_t total, bad_len, unknown;

    uint64_t bad_side, ts_backwards, ts_out_of_day;
    uint64_t zero_shares, zero_price, high_price;
    uint64_t last_ts;
    uint32_t min_price, max_price;
    uint64_t peak_orders;
    uint64_t max_order_ref;

    uint64_t dup_directory, dup_order;
    uint64_t unknown_book, unknown_order;
    uint64_t book_errors;
} engine_t;

int engine_init(engine_t *engine)
{
    *engine = (engine_t){0};
    engine->min_price = UINT32_MAX;

    engine->books = calloc(MAX_LOCATE, sizeof(book));
    if (!engine->books)
    {
        fprintf(stderr, "unable to initialize books\n");
        return 0;
    }

    if (!order_table_init(&engine->orders, 1 << 20))
    {
        fprintf(stderr, "unable to initialize order table\n");
        free(engine->books);
        engine->books = NULL;
        return 0;
    }
    return 1;
}

void engine_free(engine_t *engine)
{
    if (engine->books) {
        for (size_t i = 0; i < MAX_LOCATE; i++)
            book_free(&engine->books[i]);
        free(engine->books);
        engine->books = NULL;
    }
    order_table_free(&engine->orders);
}

static void reduce_order(engine_t *engine, uint64_t order_ref,
    uint32_t shares, int delete_all);

static int add_order(engine_t *engine, uint16_t stock_locate,
    uint64_t order_ref, char side, uint32_t price, uint32_t shares);

static size_t print_report(const engine_t *e, const char *path,
                           size_t off, size_t limit, size_t file_size, double elapsed);

static uint64_t parse_size(const char *s);

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [--max-bytes N[K|M|G]] <itch_file>\n", prog);
}

int main(int argc, char **argv) {
    uint64_t max_bytes = UINT64_MAX; /* default: whole file */
    const char *path = NULL;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--max-bytes") == 0 && i + 1 < argc)
        {
            max_bytes = parse_size(argv[++i]);
            if (max_bytes == 0)
            {
                fprintf(stderr, "bad --max-bytes value\n");
                return 1;
            }
        }
        else if (argv[i][0] == '-')
        {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
        else if (path == NULL)
        {
            path = argv[i];
        }
        else
        {
            fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (!path)
    {
        usage(argv[0]);
        return 1;
    }

    int status = EXIT_SUCCESS;

    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); return 1; }

    size_t file_size = (size_t)st.st_size;
    if (file_size == 0) { fprintf(stderr, "empty file\n"); return 1; }

    size_t limit = (max_bytes < file_size) ? max_bytes : file_size;

    const uint8_t *data = mmap(NULL, limit, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) { perror("mmap"); return 1; }
    posix_madvise((void *)data, limit, POSIX_MADV_SEQUENTIAL); /* hint: linear scan */

    engine_t engine;
    if (!engine_init(&engine)) { munmap((void *)data, limit); close(fd); return 1; }

    double t0 = now_sec();
    size_t off = 0;
    while (off + 2 <= limit)
    {
        uint16_t len = be16(data + off);
        if (len == 0 || off + 2 + len > limit)
        {
            fprintf(stderr, "truncated or corrupt frame at offset %zu\n", off);
            break;
        }
        const uint8_t *msg = data + off + 2;
        uint8_t type = msg[0];

        if (ITCH_MSG_LEN[type] == 0)
            engine.unknown++;
        else if (ITCH_MSG_LEN[type] != len)
            engine.bad_len++;

        engine.counts[type]++;
        engine.total++;

        if (ITCH_MSG_LEN[type] == len)
        {
            itch_header h;
            itch_decode_header(msg, &h);

            if (h.timestamp_ns < engine.last_ts)
                engine.ts_backwards++;
            if (h.timestamp_ns >= NS_PER_DAY)
                engine.ts_out_of_day++;

            engine.last_ts = h.timestamp_ns;

            switch (type)
            {
                case 'A':
                case 'F':
                {
                    itch_add_order a;
                    itch_decode_add_order(msg, &a);

                    if (a.side != 'B' && a.side != 'S')
                        engine.bad_side++;
                    if (a.shares == 0)
                        engine.zero_shares++;
                    if (a.price == 0)
                        engine.zero_price++;
                    if (a.price < engine.min_price)
                        engine.min_price = a.price;
                    if (a.price > engine.max_price)
                        engine.max_price = a.price;
                    if (a.price >= 100000000) {
                        engine.high_price++;
                        if (engine.high_price <= 5)
                            printf("high: %.8s  %c  %u shares @ $%.4f\n", a.stock, a.side,
                            a.shares, a.price / 10000.0);
                    }

                    if (!add_order(&engine, a.hdr.stock_locate, a.order_ref,
                        a.side, a.price, a.shares))
                    {
                        status = EXIT_FAILURE;
                        goto cleanup;
                    }

                    break;
                }
                case 'E':
                case 'C':
                {
                    itch_order_executed e;
                    itch_decode_order_executed(msg, &e);

                    if (e.executed_shares == 0)
                        engine.zero_shares++;

                    reduce_order(&engine, e.order_ref, e.executed_shares, 0);
                    break;
                }
                case 'X':
                {
                    itch_order_cancel x;
                    itch_decode_order_cancel(msg, &x);

                    if (x.cancelled_shares == 0)
                        engine.zero_shares++;

                    reduce_order(&engine, x.order_ref, x.cancelled_shares, 0);
                    break;
                }
                case 'D':
                {
                    itch_order_delete d;
                    itch_decode_order_delete(msg, &d);
                    reduce_order(&engine, d.order_ref, 0, 1);
                    break;
                }
                case 'U':
                {
                    itch_order_replace u;
                    itch_decode_order_replace(msg, &u);

                    if (u.shares == 0)
                        engine.zero_shares++;
                    if (u.price == 0)
                        engine.zero_price++;

                    order *old = order_table_find(&engine.orders, u.original_order_ref);
                    if (!old) { engine.unknown_order++; break; }

                    uint16_t locate = old->stock_locate;
                    char side = old->side;

                    reduce_order(&engine, u.original_order_ref, 0, 1); // remove old order
                    if (!add_order(&engine, locate, u.new_order_ref, side, u.price, u.shares))
                    { // Out of memory
                        status = EXIT_FAILURE;
                        goto cleanup;
                    }

                    break;
                }
                case 'R':
                {
                    itch_stock_directory r;
                    itch_decode_stock_directory(msg, &r);

                    book *b = &engine.books[r.hdr.stock_locate];
                    if (b->bids.data != NULL) { engine.dup_directory++; break; }

                    if (!book_init(b))
                    {
                        fprintf(stderr, "out of memory initializing book %u\n",
                        r.hdr.stock_locate);
                        status = EXIT_FAILURE;
                        goto cleanup;
                    }
                    memcpy(b->symbols, r.stock, 8);
                    break;
                }
                default:
                {
                    break; /* other message types: not needed yet */
                }
            }
        }

        off += 2 + (size_t)len;
    }

    double elapsed = now_sec() - t0;

    if (print_report(&engine, argv[1], off, limit, file_size, elapsed) > 0)
        status = EXIT_FAILURE;

    cleanup:
        engine_free(&engine);
        munmap((void *)data, file_size);
        close(fd);
        return status;
}

static void reduce_order(engine_t *engine, uint64_t order_ref,
    uint32_t shares, int delete_all)
{
    order *o = order_table_find(&engine->orders, order_ref);
    if (!o) { engine->unknown_order++; return; }

    if (delete_all)
        shares = o->shares;
    if (shares > o->shares) { engine->book_errors++; return; }

    uint16_t locate = o->stock_locate;
    char side = o->side;
    uint32_t price  = o->price;

    book *b = &engine->books[locate];
    if (b->bids.data == NULL) { engine->unknown_book++; return; }

    o->shares -= shares;
    int order_gone = (o->shares == 0);

    if (!book_reduce(b, side, price, shares, order_gone))
        engine->book_errors++;

    if (order_gone)
        order_table_remove(&engine->orders, order_ref);
}

static int add_order(engine_t *engine, uint16_t stock_locate,
    uint64_t order_ref, char side, uint32_t price, uint32_t shares)
{
    book *b = &engine->books[stock_locate];
    if (b->bids.data == NULL) { engine->unknown_book++; return 1; }

    if (order_table_find(&engine->orders, order_ref))
    {
        engine->dup_order++;
        return 1;
    }

    order *o = order_table_insert(&engine->orders, order_ref);
    if (!o) {
        fprintf(stderr, "out of memory in order table\n");
        return 0;
    }

    o->stock_locate = stock_locate;
    o->side = side;
    o->price = price;
    o->shares = shares;

    if (engine->orders.count > engine->peak_orders)
        engine->peak_orders = engine->orders.count;

    if (order_ref > engine->max_order_ref)
        engine->max_order_ref = order_ref;

    book_add(b, side, price, shares);
    return 1;
}

#define U64(x) ((unsigned long long)(x))

static const char *msg_name(int t)
{
    switch (t) {
    case 'S': return "System Event";
    case 'R': return "Stock Directory";
    case 'H': return "Trading Action";
    case 'Y': return "Reg SHO Restriction";
    case 'L': return "Participant Position";
    case 'V': return "MWCB Decline Level";
    case 'W': return "MWCB Status";
    case 'K': return "IPO Quoting Period";
    case 'J': return "LULD Auction Collar";
    case 'h': return "Operational Halt";
    case 'A': return "Add Order";
    case 'F': return "Add Order (MPID)";
    case 'E': return "Order Executed";
    case 'C': return "Executed with Price";
    case 'X': return "Order Cancel";
    case 'D': return "Order Delete";
    case 'U': return "Order Replace";
    case 'P': return "Trade (non-cross)";
    case 'Q': return "Cross Trade";
    case 'B': return "Broken Trade";
    case 'I': return "NOII";
    case 'N': return "RPII";
    case 'O': return "Direct Listing";
    default:  return "unknown";
    }
}

static size_t print_report(const engine_t *e, const char *path,
                           size_t off, size_t limit, size_t file_size, double elapsed)
{
    printf("\n== Run ==\n");
    printf("  %-22s %s (%.2f GB)\n", "file", path, (double)file_size / 1e9);
    printf("  bytes processed        %.2f GB of %.2f GB%s\n",
           off / 1e9, file_size / 1e9,
           limit < file_size ? " (capped by --max-bytes)" : "");
    printf("  %-22s %llu\n", "messages", U64(e->total));
    printf("  %-22s %.3f s\n", "elapsed", elapsed);
    if (elapsed > 0 && e->total > 0) {
        printf("  %-22s %.1f M msgs/s\n", "throughput",
               (double)e->total / elapsed / 1e6);
        printf("  %-22s %.1f ns\n", "time per message",
               elapsed * 1e9 / (double)e->total);
    }

    printf("\n== Message types ==\n");
    for (int t = 0; t < 256; t++) {
        if (!e->counts[t])
            continue;
        printf("  %c  %-22s %12llu  %5.1f%%\n", t, msg_name(t),
               U64(e->counts[t]), 100.0 * (double)e->counts[t] / (double)e->total);
    }

    const struct { const char *name; uint64_t value; } checks[] = {
        { "bad length",          e->bad_len },
        { "unknown type",        e->unknown },
        { "bad side",            e->bad_side },
        { "timestamp backwards", e->ts_backwards },
        { "timestamp > 24h",     e->ts_out_of_day },
        { "zero shares",         e->zero_shares },
        { "zero price",          e->zero_price },
        { "unknown order",       e->unknown_order },
        { "unknown book",        e->unknown_book },
        { "book errors",         e->book_errors },
        { "duplicate order",     e->dup_order },
        { "duplicate directory", e->dup_directory },
    };
    size_t n_checks = sizeof checks / sizeof checks[0];
    size_t failed = 0;

    printf("\n== Sanity checks (all should be 0) ==\n");
    for (size_t i = 0; i < n_checks; i++) {
        printf("  %-22s %12llu  %s\n", checks[i].name, U64(checks[i].value),
               checks[i].value ? "FAIL" : "ok");
        if (checks[i].value)
            failed++;
    }
    if (failed)
        printf("  -> %zu of %zu checks FAILED\n", failed, n_checks);
    else
        printf("  -> all %zu checks passed\n", n_checks);

    printf("\n== Order table ==\n");
    printf("  %-22s %llu\n", "peak live orders", U64(e->peak_orders));
    printf("  %-22s %llu\n", "live orders at end", U64(e->orders.count));
    printf("  %-22s %llu\n", "max order ref", U64(e->max_order_ref));
    printf("  %-22s %zu\n", "final capacity", e->orders.capacity);
    if (e->orders.capacity)
        printf("  %-22s %.1f%%\n", "peak / final capacity",
               100.0 * (double)e->peak_orders / (double)e->orders.capacity);

    printf("\n== Add-order prices ==\n");
    if (e->max_price > 0)
        printf("  %-22s $%.4f to $%.4f\n", "range",
               e->min_price / 10000.0, e->max_price / 10000.0);
    printf("  %-22s %llu\n", "orders >= $10,000", U64(e->high_price));

    return failed;
}

/* "4G" -> 4294967296, "500M" -> 524288000, "1000" -> 1000. Returns 0 on error. */
static uint64_t parse_size(const char *s)
{
    char *end;
    uint64_t n = strtoull(s, &end, 10);
    switch (*end) {
    case 'G': case 'g': n <<= 30; end++; break;
    case 'M': case 'm': n <<= 20; end++; break;
    case 'K': case 'k': n <<= 10; end++; break;
    }
    return (*end == '\0') ? n : 0;
}
