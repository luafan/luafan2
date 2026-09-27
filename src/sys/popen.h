/*
 * popen.h — LuaFan v2 subprocess management (M12).
 *
 * Lua API (fan.popen):
 *   fan.popen.spawn{
 *     command = "ls -l" | {"ls","-l"},
 *     onread          = function(data) end,           -- required
 *     onstderr        = function(data) end,           -- optional
 *     ondisconnected  = function(reason, exit) end,   -- optional
 *     capture_stderr  = true,                         -- default true
 *     env             = { KEY = "value", ... },       -- overrides env
 *     process_group   = false,                        -- close kills group
 *     pty             = false,                        -- allocate a pty
 *   } -> popen userdata | nil, err
 *
 *   popen:send(data)          -> bytes_written | nil, err
 *   popen:close_stdin()       -> true
 *   popen:set_winsize(rows,cols) -> true | nil, err   (pty=true only)
 *   popen:close()             -> true
 *   popen:getpid()            -> pid | nil
 *   popen:is_alive()          -> boolean
 *
 * SIGCHLD is not caught: we drive process teardown from the read-event
 * EOF, which is the same strategy v1 used and does not race with other
 * libraries that install SIGCHLD handlers.
 */
#ifndef FAN2_SYS_POPEN_H
#define FAN2_SYS_POPEN_H

#include <lua.h>

/* Register the fan.popen module on the fan table currently at the top
 * of L's stack. */
void fan_popen_register(lua_State *L);

#endif /* FAN2_SYS_POPEN_H */
