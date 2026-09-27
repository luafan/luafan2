/*
 * luafan.h — public embedding boundary for LuaFan v2.
 *
 * Embedders must call fan_clear_lua_states() immediately before lua_close()
 * when the event loop may still own armed libevent callbacks. The callback
 * state is then cleared before fan_loop_cleanup() releases the event base.
 */
#ifndef LUAFAN2_LUAFAN_H
#define LUAFAN2_LUAFAN_H

#include <lua.h>

/* Open the built-in fan module table. */
int luaopen_fan(lua_State *L);

/* Clear all module-cached Lua main-thread pointers before lua_close(). */
void fan_clear_lua_states(void);

#endif /* LUAFAN2_LUAFAN_H */