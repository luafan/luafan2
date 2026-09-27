/*
 * codec/json.h — LuaFan v2 JSON codec (RFC 8259 encode/decode).
 * Registered as fan.json with encode/decode/array/object/null/is_array/is_object.
 */
#ifndef FAN2_CODEC_JSON_H
#define FAN2_CODEC_JSON_H
#include <lua.h>
void fan_json_register(lua_State *L);
#endif
