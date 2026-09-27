/*
 * test_bytearray.c — unit tests for src/util/bytearray.c (v2).
 * Covers: alloc/dealloc, append growth, integer types LE round-trip,
 * read/write mode switch, wrap buffer, read underflow, overflow guard.
 */
#include "test_framework.h"
#include "bytearray.h"

#include <string.h>

TEST_CASE(t_alloc_dealloc) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 0));
    TEST_ASSERT_NOT_NULL(ba.buffer);
    TEST_ASSERT_EQ(ba.length, 0);
    TEST_ASSERT_TRUE(bytearray_dealloc(&ba));
    TEST_ASSERT_EQ(ba.buffer == NULL, 1);
    TEST_ASSERT_EQ(ba.total, 0);
}

TEST_CASE(t_append_and_grow) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 4));
    const char *msg = "hello, luafan v2 dynamic buffer growth test";
    size_t n = strlen(msg);
    TEST_ASSERT_TRUE(bytearray_writebuffer(&ba, msg, n));
    TEST_ASSERT_EQ(ba.length, n);
    TEST_ASSERT_TRUE(ba.total >= n);
    TEST_ASSERT_MEM_EQ(ba.buffer, msg, n);
    bytearray_dealloc(&ba);
}

TEST_CASE(t_int_types_roundtrip) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 2));
    TEST_ASSERT_TRUE(bytearray_write8(&ba, 0xAB));
    TEST_ASSERT_TRUE(bytearray_write16(&ba, 0x1234));
    TEST_ASSERT_TRUE(bytearray_write32(&ba, 0xDEADBEEF));
    TEST_ASSERT_TRUE(bytearray_write64(&ba, 0x0102030405060708ULL));

    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));
    uint8_t u8; uint16_t u16; uint32_t u32; uint64_t u64;
    TEST_ASSERT_TRUE(bytearray_read8(&ba, &u8));   TEST_ASSERT_EQ(u8, 0xAB);
    TEST_ASSERT_TRUE(bytearray_read16(&ba, &u16)); TEST_ASSERT_EQ(u16, 0x1234);
    TEST_ASSERT_TRUE(bytearray_read32(&ba, &u32)); TEST_ASSERT_EQ(u32, 0xDEADBEEF);
    TEST_ASSERT_TRUE(bytearray_read64(&ba, &u64)); TEST_ASSERT_EQ(u64 == 0x0102030405060708ULL, 1);
    TEST_ASSERT_EQ(bytearray_read_available(&ba), 0);
    bytearray_dealloc(&ba);
}

TEST_CASE(t_read_underflow) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 8));
    TEST_ASSERT_TRUE(bytearray_write8(&ba, 1));
    TEST_ASSERT_TRUE(bytearray_read_ready(&ba));
    uint8_t u8; uint32_t u32;
    TEST_ASSERT_TRUE(bytearray_read8(&ba, &u8));
    /* nothing left: reading 4 must fail, not read OOB */
    TEST_ASSERT_FALSE(bytearray_read32(&ba, &u32));
    bytearray_dealloc(&ba);
}

TEST_CASE(t_wrap_buffer_no_free) {
    uint8_t storage[4] = { 0x10, 0x20, 0x30, 0x40 };
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_wrap_buffer(&ba, storage, sizeof(storage)));
    TEST_ASSERT_EQ(bytearray_read_available(&ba), 4);
    /* wrapped buffers cannot switch to write mode / grow */
    TEST_ASSERT_FALSE(bytearray_write_ready(&ba));
    TEST_ASSERT_FALSE(bytearray_writebuffer(&ba, "x", 1));
    uint8_t v;
    TEST_ASSERT_TRUE(bytearray_read8(&ba, &v)); TEST_ASSERT_EQ(v, 0x10);
    /* dealloc must NOT free the borrowed stack buffer (ASan would flag it) */
    TEST_ASSERT_TRUE(bytearray_dealloc(&ba));
}

TEST_CASE(t_overflow_guard) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 8));
    /* reserving near SIZE_MAX must fail cleanly (no wrap, no huge malloc) */
    TEST_ASSERT_FALSE(bytearray_reserve(&ba, (size_t)-1));
    /* buffer stays usable after the rejected reserve */
    TEST_ASSERT_TRUE(bytearray_write8(&ba, 0x7F));
    TEST_ASSERT_EQ(ba.length, 1);
    bytearray_dealloc(&ba);
}

TEST_CASE(t_write_read_write_switch) {
    BYTEARRAY ba;
    TEST_ASSERT_TRUE(bytearray_alloc(&ba, 4));
    bytearray_write32(&ba, 0xAABBCCDD);
    bytearray_read_ready(&ba);
    uint16_t half;
    TEST_ASSERT_TRUE(bytearray_read16(&ba, &half)); TEST_ASSERT_EQ(half, 0xCCDD);
    /* back to write mode, append continues after existing length */
    TEST_ASSERT_TRUE(bytearray_write_ready(&ba));
    TEST_ASSERT_TRUE(bytearray_write8(&ba, 0xEE));
    TEST_ASSERT_EQ(ba.length, 5);
    bytearray_dealloc(&ba);
}

static const test_case_t bytearray_cases[] = {
    {"alloc_dealloc",         t_alloc_dealloc},
    {"append_and_grow",       t_append_and_grow},
    {"int_types_roundtrip",   t_int_types_roundtrip},
    {"read_underflow",        t_read_underflow},
    {"wrap_buffer_no_free",   t_wrap_buffer_no_free},
    {"overflow_guard",        t_overflow_guard},
    {"write_read_write_switch", t_write_read_write_switch},
};

const test_suite_t bytearray_suite = {
    "bytearray", bytearray_cases,
    (int)(sizeof(bytearray_cases) / sizeof(bytearray_cases[0]))
};
