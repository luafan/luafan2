/*
 * codec/stream.h — LuaFan v2 stream codec (u/i 8/16/24/30/32 + string/bytes).
 * Fully compiled-in; exposed as fan.stream with fan.stream.new().
 *
 * U30 is a variable-length unsigned integer:
 *   1 byte  header 00xxxxxx      -> 6-bit  value  (0 .. 0x3F)
 *   2 bytes header 01xxxxxx + b1 -> 14-bit value  (0 .. 0x3FFF)
 *   3 bytes header 10xxxxxx + b1 + b2 -> 22-bit value  (0 .. 0x3FFFFF)
 *   5 bytes header 11xxxxxx + 4 big-endian bytes -> 32-bit value
 * The header's low 6 bits carry the high 6 bits of the value in forms 1..3
 * and are ignored in the 5-byte form (matches LuaFan v1's wire format).
 */
#ifndef FAN2_CODEC_STREAM_H
#define FAN2_CODEC_STREAM_H
#include <lua.h>
void fan_stream_register(lua_State *L);
#endif
