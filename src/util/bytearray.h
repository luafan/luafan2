/*
 * bytearray.h — LuaFan v2 dynamic byte buffer.
 *
 * v2 changes over v1:
 *   - integer-overflow-safe capacity growth (returns 0 on overflow, never wraps)
 *   - standard <string.h> instead of non-standard <memory.h>
 *   - GROWTH_FACTOR actually used (1.5x amortized growth)
 *   - all public API declared here; no non-static symbols leaking from the .c
 */
#ifndef FAN2_BYTEARRAY_H
#define FAN2_BYTEARRAY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct BYTEARRAY {
    size_t   total;   /* allocated capacity of buffer */
    size_t   length;  /* number of valid bytes written (write cursor high-water) */
    size_t   offset;  /* read cursor */
    uint8_t *buffer;  /* owned heap buffer, or NULL when empty */
    bool     wrapbuffer; /* true => buffer is borrowed (not owned/freed) */
    bool     reading;    /* true => in read mode (length frozen) */
} BYTEARRAY;

/* Lifecycle */
bool bytearray_alloc(BYTEARRAY *ba, size_t length);
bool bytearray_wrap_buffer(BYTEARRAY *ba, uint8_t *buff, size_t length);
bool bytearray_dealloc(BYTEARRAY *ba);

/* Mode switch: writing -> reading (resets read offset to 0). */
bool bytearray_read_ready(BYTEARRAY *ba);
/* Mode switch: reading -> writing (append continues after existing length). */
bool bytearray_write_ready(BYTEARRAY *ba);

/* Write (append). Returns false on overflow / OOM. */
bool bytearray_writebuffer(BYTEARRAY *ba, const void *buff, size_t length);
bool bytearray_write8(BYTEARRAY *ba, uint8_t u8);
bool bytearray_write16(BYTEARRAY *ba, uint16_t u16);
bool bytearray_write32(BYTEARRAY *ba, uint32_t u32);
bool bytearray_write64(BYTEARRAY *ba, uint64_t u64);

/* Read (consume). Returns false when not enough bytes remain. */
bool bytearray_read8(BYTEARRAY *ba, uint8_t *u8);
bool bytearray_read16(BYTEARRAY *ba, uint16_t *u16);
bool bytearray_read32(BYTEARRAY *ba, uint32_t *u32);
bool bytearray_read64(BYTEARRAY *ba, uint64_t *u64);
/* Reads `length` bytes into *buff (a pointer into the internal buffer, no copy);
 * advances the read offset. buff may be NULL to skip. */
bool bytearray_readbuffer(BYTEARRAY *ba, void **buff, size_t length);

/* Introspection */
size_t bytearray_read_available(const BYTEARRAY *ba);

/* Ensure at least `additional` more bytes can be appended without reallocation.
 * Public for callers that batch writes. Returns false on overflow/OOM. */
bool bytearray_reserve(BYTEARRAY *ba, size_t additional);

#endif /* FAN2_BYTEARRAY_H */
