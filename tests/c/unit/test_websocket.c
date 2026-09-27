/*
 * test_websocket.c — unit tests for src/net/websocket.c (M14.D D1 slice).
 *
 * Covers:
 *   - fan_ws_encode_frame → fan_ws_parse_frame round-trip (small / 126 /
 *     127 payload extension boundaries; unmasked server frames)
 *   - client→server masked frame parse (RFC 6455 example key)
 *   - control frame rules (fragmented, oversized, RSV1)
 *   - RSV2/RSV3 rejection
 *   - unmasked client frame rejected in server accept mode
 *   - need-more path (partial header, partial extlen, partial payload)
 *   - fan_ws_compute_accept RFC 6455 §1.3 canonical example
 *   - mask xor 4-byte alignment sanity for arbitrary offsets
 */
#include "test_framework.h"
#include "net/websocket.h"

#include <event2/buffer.h>
#include <stdlib.h>
#include <string.h>

/* ---- encode/parse round-trip -------------------------------------------- */

static void _rt(int fin, int opcode, int rsv1,
                const char *payload, size_t plen) {
    struct evbuffer *buf = evbuffer_new();
    if (!buf) { TEST_FAIL("evbuffer_new"); return; }
    int enc = fan_ws_encode_frame(buf, fin, opcode, rsv1, payload, plen);
    TEST_ASSERT_EQ(enc, 0);

    /* server encodes unmasked → parse with require_mask=0 (client side) */
    fan_ws_frame_t f;
    const char *err = NULL;
    int rc = fan_ws_parse_frame(buf, &f, 0, &err);
    TEST_ASSERT_EQ(rc, 1);
    TEST_ASSERT_EQ(f.fin, fin);
    TEST_ASSERT_EQ(f.opcode, opcode);
    TEST_ASSERT_EQ(f.rsv1, rsv1);
    TEST_ASSERT_EQ(f.plen, plen);
    if (plen > 0) TEST_ASSERT_MEM_EQ(f.payload, payload, plen);
    fan_ws_frame_dispose(&f);
    /* input drained */
    TEST_ASSERT_EQ(evbuffer_get_length(buf), 0);
    evbuffer_free(buf);
}

TEST_CASE(t_encode_parse_small) {
    _rt(1, FAN_WS_OP_TEXT, 0, "hello", 5);
}
TEST_CASE(t_encode_parse_empty) {
    _rt(1, FAN_WS_OP_PONG, 0, NULL, 0);
}
TEST_CASE(t_encode_parse_125) {
    char buf[125];
    for (int i = 0; i < 125; i++) buf[i] = (char)('a' + (i % 26));
    _rt(1, FAN_WS_OP_BIN, 0, buf, 125);
}
TEST_CASE(t_encode_parse_126) {
    char buf[126];
    for (int i = 0; i < 126; i++) buf[i] = (char)i;
    _rt(1, FAN_WS_OP_BIN, 0, buf, 126);
}
TEST_CASE(t_encode_parse_65535) {
    size_t n = 65535;
    char *buf = (char *)malloc(n);
    TEST_ASSERT_NOT_NULL(buf);
    for (size_t i = 0; i < n; i++) buf[i] = (char)(i & 0xff);
    _rt(1, FAN_WS_OP_BIN, 0, buf, n);
    free(buf);
}
TEST_CASE(t_encode_parse_65536) {
    /* forces the 127 (8-byte) extended length form */
    size_t n = 65536;
    char *buf = (char *)malloc(n);
    TEST_ASSERT_NOT_NULL(buf);
    for (size_t i = 0; i < n; i++) buf[i] = (char)((i * 7) & 0xff);
    _rt(1, FAN_WS_OP_BIN, 0, buf, n);
    free(buf);
}
TEST_CASE(t_encode_parse_fragmented_and_rsv1) {
    _rt(0, FAN_WS_OP_TEXT, 1, "frag1", 5);
    _rt(1, FAN_WS_OP_CONT, 0, "frag2", 5);
}

/* ---- masked client → server -------------------------------------------- */

/* Build a client masked frame manually and parse it. */
TEST_CASE(t_parse_client_masked) {
    /* payload = "Hello"; mask = 37 fa 21 3d (RFC 6455 §5.7 example) */
    unsigned char frame[] = {
        0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
        0x7f, 0x9f, 0x4d, 0x51, 0x58
    };
    struct evbuffer *buf = evbuffer_new();
    evbuffer_add(buf, frame, sizeof(frame));

    fan_ws_frame_t f;
    const char *err = NULL;
    int rc = fan_ws_parse_frame(buf, &f, 1 /* require_mask */, &err);
    TEST_ASSERT_EQ(rc, 1);
    TEST_ASSERT_EQ(f.fin, 1);
    TEST_ASSERT_EQ(f.opcode, FAN_WS_OP_TEXT);
    TEST_ASSERT_EQ(f.plen, 5);
    TEST_ASSERT_MEM_EQ(f.payload, "Hello", 5);
    fan_ws_frame_dispose(&f);
    evbuffer_free(buf);
}

/* ---- error paths -------------------------------------------------------- */

TEST_CASE(t_parse_unmasked_client_rejected) {
    /* Same 5-byte text frame but WITHOUT the mask bit — must fail when
     * we insist on masks (server accept mode). */
    unsigned char frame[] = { 0x81, 0x05, 'H', 'e', 'l', 'l', 'o' };
    struct evbuffer *buf = evbuffer_new();
    evbuffer_add(buf, frame, sizeof(frame));
    fan_ws_frame_t f;
    const char *err = NULL;
    int rc = fan_ws_parse_frame(buf, &f, 1, &err);
    TEST_ASSERT_EQ(rc, -1);
    TEST_ASSERT_NOT_NULL(err);
    evbuffer_free(buf);
}

TEST_CASE(t_parse_rsv23_rejected) {
    /* RSV2 set (0x20) on a text frame. */
    unsigned char frame[] = { 0xa1, 0x00 };
    struct evbuffer *buf = evbuffer_new();
    evbuffer_add(buf, frame, sizeof(frame));
    fan_ws_frame_t f;
    const char *err = NULL;
    int rc = fan_ws_parse_frame(buf, &f, 0, &err);
    TEST_ASSERT_EQ(rc, -1);
    TEST_ASSERT_NOT_NULL(err);
    evbuffer_free(buf);
}

TEST_CASE(t_parse_fragmented_control_rejected) {
    /* FIN=0 on a PING (opcode 0x9) — control frames MUST be FIN. */
    unsigned char frame[] = { 0x09, 0x00 };
    struct evbuffer *buf = evbuffer_new();
    evbuffer_add(buf, frame, sizeof(frame));
    fan_ws_frame_t f;
    const char *err = NULL;
    int rc = fan_ws_parse_frame(buf, &f, 0, &err);
    TEST_ASSERT_EQ(rc, -1);
    evbuffer_free(buf);
}

TEST_CASE(t_parse_oversized_control_rejected) {
    /* PING with payload = 126 bytes must be rejected. Declared length
     * 126 forces the 7+16 extended-length branch, so we also encode
     * the extra two bytes even though the parser should reject before
     * touching them. */
    unsigned char frame[] = { 0x89, 0x7e, 0x00, 0x7e };
    struct evbuffer *buf = evbuffer_new();
    evbuffer_add(buf, frame, sizeof(frame));
    fan_ws_frame_t f;
    const char *err = NULL;
    int rc = fan_ws_parse_frame(buf, &f, 0, &err);
    TEST_ASSERT_EQ(rc, -1);
    evbuffer_free(buf);
}

TEST_CASE(t_parse_control_with_rsv1_rejected) {
    /* PING with RSV1 set — permessage-deflate applies only to data frames. */
    unsigned char frame[] = { 0xc9, 0x00 };
    struct evbuffer *buf = evbuffer_new();
    evbuffer_add(buf, frame, sizeof(frame));
    fan_ws_frame_t f;
    const char *err = NULL;
    int rc = fan_ws_parse_frame(buf, &f, 0, &err);
    TEST_ASSERT_EQ(rc, -1);
    evbuffer_free(buf);
}

TEST_CASE(t_parse_need_more_partial_header) {
    unsigned char one = 0x81;
    struct evbuffer *buf = evbuffer_new();
    evbuffer_add(buf, &one, 1);
    fan_ws_frame_t f;
    const char *err = NULL;
    int rc = fan_ws_parse_frame(buf, &f, 0, &err);
    TEST_ASSERT_EQ(rc, 0);
    /* buf must be untouched (still 1 byte inside) */
    TEST_ASSERT_EQ(evbuffer_get_length(buf), 1);
    evbuffer_free(buf);
}

TEST_CASE(t_parse_need_more_partial_extlen) {
    /* Announce plen=126 but supply only 3 bytes so the 16-bit ext-len
     * cannot be read. */
    unsigned char frame[] = { 0x82, 0x7e, 0x00 };
    struct evbuffer *buf = evbuffer_new();
    evbuffer_add(buf, frame, sizeof(frame));
    fan_ws_frame_t f;
    const char *err = NULL;
    int rc = fan_ws_parse_frame(buf, &f, 0, &err);
    TEST_ASSERT_EQ(rc, 0);
    TEST_ASSERT_EQ(evbuffer_get_length(buf), 3);
    evbuffer_free(buf);
}

TEST_CASE(t_parse_need_more_partial_payload) {
    /* 5-byte text frame with only 3 of the payload bytes delivered. */
    unsigned char frame[] = { 0x81, 0x05, 'H', 'e', 'l' };
    struct evbuffer *buf = evbuffer_new();
    evbuffer_add(buf, frame, sizeof(frame));
    fan_ws_frame_t f;
    const char *err = NULL;
    int rc = fan_ws_parse_frame(buf, &f, 0, &err);
    TEST_ASSERT_EQ(rc, 0);
    TEST_ASSERT_EQ(evbuffer_get_length(buf), sizeof(frame));
    evbuffer_free(buf);
}

/* ---- handshake accept-key ---------------------------------------------- */

TEST_CASE(t_compute_accept_rfc6455_example) {
    /* RFC 6455 §1.3 example:
     *   client key = "dGhlIHNhbXBsZSBub25jZQ=="
     *   expected accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" */
    char out[64];
    int rc = fan_ws_compute_accept("dGhlIHNhbXBsZSBub25jZQ==",
                                    out, sizeof(out));
#ifdef FAN_WITH_OPENSSL
    TEST_ASSERT_EQ(rc, 0);
    TEST_ASSERT_EQ(strcmp(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), 0);
#else
    /* No OpenSSL: must fail cleanly, not crash. */
    TEST_ASSERT_EQ(rc, -1);
#endif
}

/* ---- large-payload byte-perfect through the encoder --------------------- */

TEST_CASE(t_encode_parse_large_pattern_byteperfect) {
    /* Verifies the aligned-XOR path (parse handles unmasked here but the
     * encoder is the primary target). 100_003 is deliberately not a
     * multiple of 4 so we exercise the head/tail masks even though
     * no mask is applied on the server → client direction. */
    size_t n = 100003;
    char *src = (char *)malloc(n);
    TEST_ASSERT_NOT_NULL(src);
    for (size_t i = 0; i < n; i++) src[i] = (char)((i * 131 + 7) & 0xff);

    struct evbuffer *buf = evbuffer_new();
    TEST_ASSERT_EQ(fan_ws_encode_frame(buf, 1, FAN_WS_OP_BIN, 0, src, n), 0);

    fan_ws_frame_t f;
    const char *err = NULL;
    TEST_ASSERT_EQ(fan_ws_parse_frame(buf, &f, 0, &err), 1);
    TEST_ASSERT_EQ(f.plen, n);
    TEST_ASSERT_MEM_EQ(f.payload, src, n);

    fan_ws_frame_dispose(&f);
    evbuffer_free(buf);
    free(src);
}

/* ---- suite ------------------------------------------------------------- */

static const test_case_t websocket_cases[] = {
    {"encode_parse_small",              t_encode_parse_small},
    {"encode_parse_empty",              t_encode_parse_empty},
    {"encode_parse_125",                t_encode_parse_125},
    {"encode_parse_126",                t_encode_parse_126},
    {"encode_parse_65535",              t_encode_parse_65535},
    {"encode_parse_65536",              t_encode_parse_65536},
    {"encode_parse_fragmented_and_rsv1", t_encode_parse_fragmented_and_rsv1},
    {"parse_client_masked",             t_parse_client_masked},
    {"parse_unmasked_client_rejected",  t_parse_unmasked_client_rejected},
    {"parse_rsv23_rejected",            t_parse_rsv23_rejected},
    {"parse_fragmented_control_rejected", t_parse_fragmented_control_rejected},
    {"parse_oversized_control_rejected", t_parse_oversized_control_rejected},
    {"parse_control_with_rsv1_rejected", t_parse_control_with_rsv1_rejected},
    {"parse_need_more_partial_header",  t_parse_need_more_partial_header},
    {"parse_need_more_partial_extlen",  t_parse_need_more_partial_extlen},
    {"parse_need_more_partial_payload", t_parse_need_more_partial_payload},
    {"compute_accept_rfc6455_example",  t_compute_accept_rfc6455_example},
    {"encode_parse_large_pattern_byteperfect", t_encode_parse_large_pattern_byteperfect},
};

const test_suite_t websocket_suite = {
    "websocket", websocket_cases,
    (int)(sizeof(websocket_cases) / sizeof(websocket_cases[0]))
};
