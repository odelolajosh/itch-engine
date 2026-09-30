#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "itch.h"

/* Build messages by hand, byte by byte, then check the decoder. */

static void test_header(void) {
    const uint8_t msg[11] = {
        'A',
        0x00, 0x2A,                         /* locate   = 42 */
        0x00, 0x07,                         /* tracking = 7  */
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00  /* timestamp = 256 ns */
    };
    itch_header h;
    itch_decode_header(msg, &h);
    assert(h.type == 'A');
    assert(h.stock_locate == 42);
    assert(h.tracking_number == 7);
    assert(h.timestamp_ns == 256);
}

static void test_be48_large(void) {
    const uint8_t ts[6] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    assert(be48(ts) == 0x123456789ABCULL);
}

static void test_add_order(void) {
    const uint8_t msg[36] = {
        0x41,                                     /* 'A' */
        0x00, 0x01,                               /* locate = 1 */
        0x00, 0x00,                               /* tracking */
        0x1f, 0x1a, 0xce, 0xd9, 0xf0, 0x00,       /* 9:30:00am */
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x05,  /* order ref = 5 */
        0x42,                                     /* 'B' */
        0x00, 0x00, 0x00, 0x64,                   /* shares = 100 */
        'A','A','P','L',' ',' ',' ',' ',          /* stock */
        0x00, 0x16, 0xe3, 0x60                    /* price = 1500000 */
    };
    itch_add_order o;
    itch_decode_add_order(msg, &o);
    assert(o.hdr.timestamp_ns == 34200000000000ULL);
    assert(o.order_ref == 5);
    assert(o.side == 'B');
    assert(o.shares == 100);
    assert(memcmp(o.stock, "AAPL    ", 8) == 0);
    assert(o.price == 1500000);
}

/* TODO: test_add_order: build a 36-byte 'A' message with a known order_ref,
 * side 'B', shares 100, stock "AAPL    ", price 1502500 ($150.25). */
/* TODO: one test per remaining decoder. */

int main(void) {
    test_header();
    test_be48_large();
    test_add_order();
    printf("all tests passed\n");
    return 0;
}
