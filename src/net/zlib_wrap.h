/*
 * zlib_wrap.h — LuaFan v2 zlib deflate/inflate primitives.
 *
 * Compiled unconditionally; the zlib-dependent body is guarded by
 * FAN_WITH_ZLIB. When zlib is disabled fan.zlib.available() returns false
 * and the operational functions return nil,"not compiled in" from Lua.
 *
 * We expose the "raw deflate" wire format (no zlib header, no adler32) —
 * the same encoding used by permessage-deflate and by HTTP deflate content
 * encoding on the wire. gzip and cooked zlib framing can be layered on top
 * in Lua when needed; keeping the C layer minimal preserves "thin C" and
 * avoids extra options-plumbing.
 *
 * API on the Lua side (registered by fan_zlib_register into module @ -1):
 *   fan.zlib.available()  -> boolean  (also fan.zlib.enabled)
 *   fan.zlib.deflate_raw(data [, level])  -> string or nil,err
 *       level: 0..9, default Z_DEFAULT_COMPRESSION (-1)
 *   fan.zlib.inflate_raw(data)            -> string or nil,err
 *
 * These primitives own no state: each call is a self-contained round-trip
 * over a temporary z_stream. That matches "no unbounded static growth" and
 * is fine for handshake payloads / one-shot messages; stateful streaming
 * (permessage-deflate context takeover) will layer on top later.
 */
#ifndef FAN2_NET_ZLIB_H
#define FAN2_NET_ZLIB_H

#include <lua.h>
#include <stddef.h>

/* Whether zlib is compiled in. */
int fan_zlib_available(void);

/* Register the fan.zlib table on the module at stack top (-1). */
void fan_zlib_register(lua_State *L);

/* ---- Internal C API (used by websocket.c permessage-deflate path) ----
 *
 * Both functions:
 *   - Return a malloc'd buffer via *out (caller must free()).
 *   - Return 0 on success, -1 on failure; on failure *out is NULL and
 *     *errmsg is a static string suitable for a Lua error message.
 *   - Do not touch the Lua stack.
 *
 * These wrap the same raw-deflate / raw-inflate primitives that
 * fan.zlib.deflate_raw / fan.zlib.inflate_raw expose to Lua, so both
 * entry points share code and behaviour (hard 64 MiB decompress cap,
 * sync-flush emit, etc.).
 *
 * When compiled without FAN_WITH_ZLIB both fail with "not compiled in".
 */
int fan_zlib_deflate_raw_c(const char *in, size_t inlen,
                           int level, int sync,
                           char **out, size_t *outlen,
                           const char **errmsg);
int fan_zlib_inflate_raw_c(const char *in, size_t inlen,
                           char **out, size_t *outlen,
                           const char **errmsg);

#endif /* FAN2_NET_ZLIB_H */
