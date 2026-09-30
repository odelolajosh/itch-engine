#include <string.h>
#include "itch.h"

/* Message lengths from the ITCH 5.0 spec (excluding the 2-byte length prefix).
 * Double-check each against the official spec PDF as you implement decoders. */
const uint16_t ITCH_MSG_LEN[256] = {
    ['S'] = 12, /* System Event */
    ['R'] = 39, /* Stock Directory */
    ['H'] = 25, /* Stock Trading Action */
    ['Y'] = 20, /* Reg SHO Restriction */
    ['L'] = 26, /* Market Participant Position */
    ['V'] = 35, /* MWCB Decline Level */
    ['W'] = 12, /* MWCB Status */
    ['K'] = 28, /* IPO Quoting Period Update */
    ['J'] = 35, /* LULD Auction Collar */
    ['h'] = 21, /* Operational Halt */
    ['A'] = 36, /* Add Order */
    ['F'] = 40, /* Add Order with MPID */
    ['E'] = 31, /* Order Executed */
    ['C'] = 36, /* Order Executed with Price */
    ['X'] = 23, /* Order Cancel */
    ['D'] = 19, /* Order Delete */
    ['U'] = 35, /* Order Replace */
    ['P'] = 44, /* Trade (non-cross) */
    ['Q'] = 40, /* Cross Trade */
    ['B'] = 19, /* Broken Trade */
    ['I'] = 50, /* NOII */
    ['N'] = 20, /* RPII */
    ['O'] = 48, /* Direct Listing with Capital Raise */
};

/* Worked example: offsets 0 type, 1 locate, 3 tracking, 5 timestamp. */
void itch_decode_header(const uint8_t *p, itch_header *out) {
    out->type            = (char)p[0];
    out->stock_locate    = be16(p + 1);
    out->tracking_number = be16(p + 3);
    out->timestamp_ns    = be48(p + 5);
}

/* ---- TODO (milestone 1): implement the decoders below ------------------
 * Work out each field's byte offset from the spec. The body always starts
 * at offset 11, after the common header. Write a unit test for each one
 * in tests/test_decode.c BEFORE running it on the real file. */

void itch_decode_add_order(const uint8_t *p, itch_add_order *out) {
    itch_decode_header(p, &out->hdr);
    /* TODO: order_ref (8), side (1), shares (4), stock (8), price (4) */
    out->order_ref = be64(p + 11);
    out->side      = (char)p[19];
    out->shares    = be32(p + 20);
    memcpy(out->stock, p + 24, 8);
    out->price     = be32(p + 32);
}

void itch_decode_order_executed(const uint8_t *p, itch_order_executed *out) {
    itch_decode_header(p, &out->hdr);
    out->order_ref        = be64(p + 11);
    out->executed_shares  = be32(p + 19);
    out->match_number     = be64(p + 23);
}

void itch_decode_order_cancel(const uint8_t *p, itch_order_cancel *out) {
    itch_decode_header(p, &out->hdr);
    /* TODO: order_ref (8), cancelled_shares (4) */
    out->order_ref        = be64(p + 11);
    out->cancelled_shares = be32(p + 19);
}

void itch_decode_order_delete(const uint8_t *p, itch_order_delete *out) {
    itch_decode_header(p, &out->hdr);
    /* TODO: order_ref (8) */
    out->order_ref = be64(p + 11);
}

void itch_decode_order_replace(const uint8_t *p, itch_order_replace *out) {
    itch_decode_header(p, &out->hdr);
    /* TODO: original_order_ref (8), new_order_ref (8), shares (4), price (4) */
    out->original_order_ref = be64(p + 11);
    out->new_order_ref = be64(p + 19);
    out->shares = be32(p + 27);
    out->price = be32(p + 31);
}
