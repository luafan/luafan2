/*
 * bytearray.c — LuaFan v2 dynamic byte buffer implementation.
 * See bytearray.h for the v2-over-v1 changes.
 */
#include "bytearray.h"

#include <stdlib.h>
#include <string.h>

/* Amortized growth factor: new_cap = max(need, cap * 3/2). Integer math only. */
#define BA_MIN_CAP 16u
/* Sanity ceiling for a single buffer (1 TiB). Any request beyond this is
 * treated as a caller bug / hostile length and rejected before touching the
 * allocator, so we never hand realloc() an absurd size (which ASan aborts on,
 * and which would otherwise be a soft-DoS on stock allocators). */
#define BA_MAX_CAP ((size_t)1 << 40)

static bool ba_mul_add_overflow(size_t a, size_t b, size_t *out) {
    /* compute a + b with overflow check */
    if (a > (size_t)-1 - b) return true;
    *out = a + b;
    return false;
}

/* Grow capacity so that total >= needed. Overflow- and ceiling-safe. */
static bool ba_grow(BYTEARRAY *ba, size_t needed) {
    if (ba->total >= needed) return true;
    if (ba->wrapbuffer) return false;       /* borrowed buffers never reallocate */
    if (needed > BA_MAX_CAP) return false;  /* reject absurd sizes up front */

    size_t cap = ba->total ? ba->total : BA_MIN_CAP;
    /* cap = cap * 3 / 2 until >= needed, with overflow guard */
    while (cap < needed) {
        size_t half = cap >> 1;
        size_t next;
        if (ba_mul_add_overflow(cap, half, &next) || next > BA_MAX_CAP) {
            /* geometric growth overflowed / hit ceiling; use exact needed */
            cap = needed;
            break;
        }
        cap = next;
    }
    if (cap < needed) cap = needed;

    uint8_t *nb = (uint8_t *)realloc(ba->buffer, cap);
    if (!nb) return false;
    ba->buffer = nb;
    ba->total = cap;
    return true;
}

bool bytearray_alloc(BYTEARRAY *ba, size_t length) {
    if (!ba) return false;
    memset(ba, 0, sizeof(*ba));
    if (length == 0) length = BA_MIN_CAP;
    ba->buffer = (uint8_t *)malloc(length);
    if (!ba->buffer) return false;
    ba->total = length;
    ba->length = 0;
    ba->offset = 0;
    ba->wrapbuffer = false;
    ba->reading = false;
    return true;
}

bool bytearray_wrap_buffer(BYTEARRAY *ba, uint8_t *buff, size_t length) {
    if (!ba || (!buff && length)) return false;
    memset(ba, 0, sizeof(*ba));
    ba->buffer = buff;
    ba->total = length;
    ba->length = length;
    ba->offset = 0;
    ba->wrapbuffer = true;
    ba->reading = true;
    return true;
}

bool bytearray_dealloc(BYTEARRAY *ba) {
    if (!ba) return false;
    if (!ba->wrapbuffer) free(ba->buffer);
    /* zero every field so a stale struct cannot be reused accidentally */
    ba->buffer = NULL;
    ba->total = ba->length = ba->offset = 0;
    ba->wrapbuffer = false;
    ba->reading = false;
    return true;
}

bool bytearray_read_ready(BYTEARRAY *ba) {
    if (!ba) return false;
    ba->reading = true;
    ba->offset = 0;
    return true;
}

bool bytearray_write_ready(BYTEARRAY *ba) {
    if (!ba) return false;
    if (ba->wrapbuffer) return false;
    ba->reading = false;
    return true;
}

bool bytearray_reserve(BYTEARRAY *ba, size_t additional) {
    if (!ba) return false;
    size_t needed;
    if (ba_mul_add_overflow(ba->length, additional, &needed)) return false;
    return ba_grow(ba, needed);
}

bool bytearray_writebuffer(BYTEARRAY *ba, const void *buff, size_t length) {
    if (!ba) return false;
    if (length == 0) return true;
    size_t needed;
    if (ba_mul_add_overflow(ba->length, length, &needed)) return false;
    if (!ba_grow(ba, needed)) return false;
    if (buff) memcpy(ba->buffer + ba->length, buff, length);
    else      memset(ba->buffer + ba->length, 0, length);
    ba->length += length;
    return true;
}

bool bytearray_write8(BYTEARRAY *ba, uint8_t u8) {
    return bytearray_writebuffer(ba, &u8, 1);
}
bool bytearray_write16(BYTEARRAY *ba, uint16_t u16) {
    /* little-endian on-wire, portable regardless of host endianness */
    uint8_t b[2] = { (uint8_t)(u16 & 0xff), (uint8_t)((u16 >> 8) & 0xff) };
    return bytearray_writebuffer(ba, b, 2);
}
bool bytearray_write32(BYTEARRAY *ba, uint32_t u32) {
    uint8_t b[4] = { (uint8_t)(u32 & 0xff), (uint8_t)((u32 >> 8) & 0xff),
                     (uint8_t)((u32 >> 16) & 0xff), (uint8_t)((u32 >> 24) & 0xff) };
    return bytearray_writebuffer(ba, b, 4);
}
bool bytearray_write64(BYTEARRAY *ba, uint64_t u64) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)((u64 >> (8 * i)) & 0xff);
    return bytearray_writebuffer(ba, b, 8);
}

size_t bytearray_read_available(const BYTEARRAY *ba) {
    if (!ba) return 0;
    if (ba->offset > ba->length) return 0;
    return ba->length - ba->offset;
}

bool bytearray_readbuffer(BYTEARRAY *ba, void **buff, size_t length) {
    if (!ba) return false;
    if (bytearray_read_available(ba) < length) return false;
    if (buff) *buff = ba->buffer + ba->offset;
    ba->offset += length;
    return true;
}

bool bytearray_read8(BYTEARRAY *ba, uint8_t *u8) {
    void *p;
    if (!bytearray_readbuffer(ba, &p, 1)) return false;
    if (u8) *u8 = ((uint8_t *)p)[0];
    return true;
}
bool bytearray_read16(BYTEARRAY *ba, uint16_t *u16) {
    void *p;
    if (!bytearray_readbuffer(ba, &p, 2)) return false;
    uint8_t *b = (uint8_t *)p;
    if (u16) *u16 = (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
    return true;
}
bool bytearray_read32(BYTEARRAY *ba, uint32_t *u32) {
    void *p;
    if (!bytearray_readbuffer(ba, &p, 4)) return false;
    uint8_t *b = (uint8_t *)p;
    if (u32) *u32 = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                    ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}
bool bytearray_read64(BYTEARRAY *ba, uint64_t *u64) {
    void *p;
    if (!bytearray_readbuffer(ba, &p, 8)) return false;
    uint8_t *b = (uint8_t *)p;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)b[i]) << (8 * i);
    if (u64) *u64 = v;
    return true;
}
