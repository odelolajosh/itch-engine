#ifndef ITCH_H
#define ITCH_H

#include <stdint.h>
#include <stddef.h>

/*
 * Nasdaq TotalView-ITCH 5.0.
 * All integer fields are big-endian (network byte order).
 * Prices are unsigned fixed-point with 4 implied decimal places.
 */

/* ---- Big-endian readers ------------------------------------------------ */
static inline uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static inline uint64_t be48(const uint8_t *p) {   /* ITCH timestamps are 6 bytes */
    return ((uint64_t)be16(p) << 32) | be32(p + 2);
}
static inline uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

/* Expected message length (bytes after the 2-byte frame prefix), by type.
 * 0 means "unknown type". Defined in decode.c. */
extern const uint16_t ITCH_MSG_LEN[256];

/* ---- Decoded message structs ------------------------------------------ */
/* Every message starts: type(1) locate(2) tracking(2) timestamp(6) = 11 bytes */
typedef struct {
    char     type;
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;      /* nanoseconds since midnight */
} itch_header;

typedef struct {                /* 'A' (36 bytes) and 'F' (40, + 4-byte MPID) */
    itch_header hdr;
    uint64_t order_ref;
    char     side;              /* 'B' buy, 'S' sell */
    uint32_t shares;
    char     stock[8];          /* space-padded, NOT NUL-terminated */
    uint32_t price;
} itch_add_order;

typedef struct {                /* 'E' (31 bytes) */
    itch_header hdr;
    uint64_t order_ref;
    uint32_t executed_shares;
    uint64_t match_number;
} itch_order_executed;

typedef struct {                /* 'X' (23 bytes) */
    itch_header hdr;
    uint64_t order_ref;
    uint32_t cancelled_shares;
} itch_order_cancel;

typedef struct {                /* 'D' (19 bytes) */
    itch_header hdr;
    uint64_t order_ref;
} itch_order_delete;

typedef struct {                /* 'U' (35 bytes) */
    itch_header hdr;
    uint64_t original_order_ref;
    uint64_t new_order_ref;
    uint32_t shares;
    uint32_t price;
} itch_order_replace;

/* ---- Decoders ----------------------------------------------------------
 * `p` points at the message type byte (just after the length prefix).
 * Caller guarantees the buffer holds at least ITCH_MSG_LEN[type] bytes. */
void itch_decode_header(const uint8_t *p, itch_header *out);
void itch_decode_add_order(const uint8_t *p, itch_add_order *out);
void itch_decode_order_executed(const uint8_t *p, itch_order_executed *out);
void itch_decode_order_cancel(const uint8_t *p, itch_order_cancel *out);
void itch_decode_order_delete(const uint8_t *p, itch_order_delete *out);
void itch_decode_order_replace(const uint8_t *p, itch_order_replace *out);

#endif /* ITCH_H */
