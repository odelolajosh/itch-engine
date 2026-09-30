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
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "itch.h"

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <itch_file>\n", argv[0]);
        return 2;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); return 1; }
    size_t size = (size_t)st.st_size;
    if (size == 0) { fprintf(stderr, "empty file\n"); return 1; }

    const uint8_t *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) { perror("mmap"); return 1; }
    posix_madvise((void *)data, size, POSIX_MADV_SEQUENTIAL);  /* hint: linear scan */

    uint64_t counts[256] = {0};
    uint64_t total = 0, bad_len = 0, unknown = 0;

    uint64_t bad_side = 0, ts_backwards = 0, ts_out_of_day = 0;
    uint64_t zero_shares = 0, zero_price = 0, high_price = 0;
    uint64_t last_ts = 0;
    uint32_t min_price = UINT32_MAX, max_price = 0;
    const uint64_t NS_PER_DAY = 86400ULL * 1000000000ULL;

    double t0 = now_sec();
    size_t off = 0;
    while (off + 2 <= size) {
        uint16_t len = be16(data + off);
        if (len == 0 || off + 2 + len > size) {
            fprintf(stderr, "truncated or corrupt frame at offset %zu\n", off);
            break;
        }
        const uint8_t *msg = data + off + 2;
        uint8_t type = msg[0];

        if (ITCH_MSG_LEN[type] == 0)        unknown++;
        else if (ITCH_MSG_LEN[type] != len) bad_len++;
        counts[type]++;
        total++;

        if (ITCH_MSG_LEN[type] == len) {
            itch_header h;
            itch_decode_header(msg, &h);
            if (h.timestamp_ns < last_ts)      ts_backwards++;
            if (h.timestamp_ns >= NS_PER_DAY)  ts_out_of_day++;
            last_ts = h.timestamp_ns;

            switch (type)
            {
              case 'A':
              case 'F': {
                  itch_add_order a;
                  itch_decode_add_order(msg, &a);
                  if (a.side != 'B' && a.side != 'S') bad_side++;
                  if (a.shares == 0) zero_shares++;
                  if (a.price == 0)  zero_price++;
                  if (a.price < min_price) min_price = a.price;
                  if (a.price > max_price) max_price = a.price;
                  if (a.price >= 100000000) {
                      high_price++;
                      if (high_price <= 5)
                          printf("high: %.8s  %c  %u shares @ $%.4f\n", a.stock,
                              a.side, a.shares, a.price / 10000.0);
                  }
                  break;
              }
              case 'E': {
                  itch_order_executed e;
                  itch_decode_order_executed(msg, &e);
                  if (e.executed_shares == 0) zero_shares++;
                  break;
              }
              case 'X': {
                  itch_order_cancel x;
                  itch_decode_order_cancel(msg, &x);
                  if (x.cancelled_shares == 0) zero_shares++;
                  break;
              }
              case 'D': {
                  itch_order_delete d;
                  itch_decode_order_delete(msg, &d);
                  break;
              }
              case 'U': {
                  itch_order_replace u;
                  itch_decode_order_replace(msg, &u);
                  if (u.shares == 0) zero_shares++;
                  if (u.price == 0)  zero_price++;
                  break;
              }
              default: {
                  break;   /* other message types: not needed yet */
              }
            }
        }

        off += 2 + (size_t)len;
    }
    double elapsed = now_sec() - t0;

    printf("file:        %s (%.2f GB)\n", argv[1], (double)size / 1e9);
    printf("messages:    %llu\n", (unsigned long long)total);
    printf("bad length:  %llu\n", (unsigned long long)bad_len);
    printf("unknown:     %llu\n", (unsigned long long)unknown);
    printf("elapsed:     %.3f s\n", elapsed);
    if (elapsed > 0)
        printf("throughput:  %.1f M msgs/s\n", (double)total / elapsed / 1e6);

    printf("\nper type:\n");
    for (int t = 0; t < 256; t++)
        if (counts[t])
            printf("  %c  %12llu\n", t, (unsigned long long)counts[t]);

    printf("\nsanity checks (all should be 0):\n");
    printf("  bad side:            %llu\n", (unsigned long long)bad_side);
    printf("  timestamp backwards: %llu\n", (unsigned long long)ts_backwards);
    printf("  timestamp > 24h:     %llu\n", (unsigned long long)ts_out_of_day);
    printf("  zero shares:         %llu\n", (unsigned long long)zero_shares);
    printf("  zero price:          %llu\n", (unsigned long long)zero_price);
    if (max_price > 0)
        printf("add-order price range: $%.4f to $%.4f\n",
               min_price / 10000.0, max_price / 10000.0);

    printf("add orders >= $10,000: %llu\n", (unsigned long long)high_price);

    munmap((void *)data, size);
    close(fd);
    return 0;
}
