/*
 * codec/objectbuf.h — LuaFan v2 objectbuf codec (compact binary serialiser).
 * See objectbuf.c for the wire format. Registered as fan.objectbuf.
 */
#ifndef FAN2_CODEC_OBJECTBUF_H
#define FAN2_CODEC_OBJECTBUF_H
#include <lua.h>
void fan_objectbuf_register(lua_State *L);
#endif
