/*
 * codec/stream.h — LuaFan v2 stream codec (u/i 8/16/24/32, u30, D64,
 * string/bytes) with v1 wire compatibility.
 *
 * U30 is a LEB128-style variable-length unsigned integer, byte-for-byte
 * compatible with LuaFan v1:
 *   each byte carries 7 payload bits in bits 0..6, and bit 7 (continuation)
 *   set means "another byte follows". Values are read in little-endian
 *   septet order. Range: 0 .. 0xFFFFFFFF (up to 5 bytes). Encoding widths:
 *     0        .. 0x7F         -> 1 byte
 *     0x80     .. 0x3FFF       -> 2 bytes
 *     0x4000   .. 0x1FFFFF     -> 3 bytes
 *     0x200000 .. 0xFFFFFFF    -> 4 bytes
 *     0x10000000 .. 0xFFFFFFFF -> 5 bytes
 *
 * D64 is IEEE-754 binary64, 8 bytes little-endian (matches v1 stream_ffi).
 */
#ifndef FAN2_CODEC_STREAM_H
#define FAN2_CODEC_STREAM_H
#include <lua.h>
void fan_stream_register(lua_State *L);
#endif
