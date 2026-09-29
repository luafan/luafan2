/*
 * test_crypto.c — M12.1 fan.crypto C-level smoke tests.
 *
 * We can't easily invoke the Lua-facing bindings from pure C without a
 * live lua_State; instead we assert (a) the registrar links + advertises
 * FAN_WITH_OPENSSL correctly, and (b) push the crypto table into a
 * freshly-opened lua_State so a symbol-level regression (unresolved
 * externs, bogus registration) trips a hard failure before we even try
 * the Lua tests. GCM round-trip / MD5 KAT is fully covered from Lua by
 * test_crypto.lua; the C harness's job here is a load-bearing smoke
 * check, symmetric to test_tcp_clear_lua_state.c.
 */
#include "test_framework.h"
#include "crypto.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

TEST_CASE(t_available_matches_build) {
    /* At the C level we just verify fan_crypto_available() returns 1 in
     * the CI build (which sets FAN_WITH_OPENSSL=1) and 0 otherwise. */
#if FAN_WITH_OPENSSL
    TEST_ASSERT_EQ(fan_crypto_available(), 1);
#else
    TEST_ASSERT_EQ(fan_crypto_available(), 0);
#endif
}

TEST_CASE(t_register_installs_crypto_table) {
    lua_State *L = luaL_newstate();
    TEST_ASSERT_NOT_NULL(L);
    luaL_openlibs(L);

    /* fan_crypto_register expects a module table on stack top; the fan
     * module table would carry it as fan.crypto in production, but here
     * we hand it a bare table and verify the field lands on it. */
    lua_newtable(L);
    fan_crypto_register(L);
    /* At this point: stack top is our module table; it should now have a
     * "crypto" field, and that field should itself be a table with
     * `available`, `md5`, `md5_binary`, and `gcm` entries. */
    lua_getfield(L, -1, "crypto");
    TEST_ASSERT_EQ(lua_type(L, -1), LUA_TTABLE);

    lua_getfield(L, -1, "available");
    TEST_ASSERT_EQ(lua_type(L, -1), LUA_TFUNCTION);
    lua_pop(L, 1);

    lua_getfield(L, -1, "md5");
    TEST_ASSERT_EQ(lua_type(L, -1), LUA_TFUNCTION);
    lua_pop(L, 1);

    lua_getfield(L, -1, "md5_binary");
    TEST_ASSERT_EQ(lua_type(L, -1), LUA_TFUNCTION);
    lua_pop(L, 1);

    lua_getfield(L, -1, "gcm");
    TEST_ASSERT_EQ(lua_type(L, -1), LUA_TTABLE);
    /* fan.crypto.gcm.encrypt / decrypt must exist too (both under OpenSSL
     * builds and under the disabled stub) — Lua tests will exercise them. */
    lua_getfield(L, -1, "encrypt");
    TEST_ASSERT_EQ(lua_type(L, -1), LUA_TFUNCTION);
    lua_pop(L, 1);
    lua_getfield(L, -1, "decrypt");
    TEST_ASSERT_EQ(lua_type(L, -1), LUA_TFUNCTION);
    lua_pop(L, 1);

    lua_close(L);
}

TEST_CASE(t_md5_via_lua_kat) {
    /* Drive the md5 function through Lua so we exercise the exact code
     * path that fan.crypto.md5() takes, without spinning up the whole
     * fan module. KAT: MD5("abc") = 900150983cd24fb0d6963f7d28e17f72. */
    lua_State *L = luaL_newstate();
    TEST_ASSERT_NOT_NULL(L);
    luaL_openlibs(L);
    lua_newtable(L);
    fan_crypto_register(L);          /* stack: mod-with-crypto */
    lua_getfield(L, -1, "crypto");   /* stack: mod, crypto */
    lua_getfield(L, -1, "md5");
    lua_pushliteral(L, "abc");
    int rc = lua_pcall(L, 1, 1, 0);
    TEST_ASSERT_EQ(rc, 0);
#if FAN_WITH_OPENSSL
    size_t sz = 0;
    const char *hex = lua_tolstring(L, -1, &sz);
    TEST_ASSERT_NOT_NULL(hex);
    TEST_ASSERT_EQ(sz, 32);
    TEST_ASSERT_MEM_EQ(hex, "900150983cd24fb0d6963f7d28e17f72", 32);
#else
    /* When OpenSSL is off, md5 returns (nil, err). Top of stack should be
     * nil (single return replaced by pcall — we requested 1 return but
     * the stub pushed 2; lua_pcall trims to what we asked). */
    TEST_ASSERT_EQ(lua_isnil(L, -1), 1);
#endif
    lua_close(L);
}

static const test_case_t crypto_cases[] = {
    { "available_matches_build",     t_available_matches_build },
    { "register_installs_crypto_table", t_register_installs_crypto_table },
    { "md5_via_lua_kat",             t_md5_via_lua_kat },
};

const test_suite_t crypto_suite = {
    .name  = "crypto",
    .cases = crypto_cases,
    .count = (int)(sizeof(crypto_cases) / sizeof(crypto_cases[0])),
};
