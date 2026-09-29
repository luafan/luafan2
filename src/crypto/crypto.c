/*
 * crypto/crypto.c — LuaFan v2 crypto helpers (M12.1 + M12.3).
 *
 * Gated by FAN_WITH_OPENSSL — when OpenSSL isn't compiled in the whole
 * table degrades to `available()=false` and every op returns
 * (nil, "openssl not compiled in"). Symmetric with fan.zlib.
 *
 * Contract lives in crypto.h; the GCM primitives live in crypto/gcm.c so
 * this file stays focused on the small MD5 helper + table wiring.
 */
#include "crypto.h"
#include "../platform.h"

#include <lauxlib.h>
#include <string.h>

#if FAN_WITH_OPENSSL
#  include <openssl/evp.h>
#endif

/* GCM registrar lives in crypto/gcm.c and pushes fan.crypto.gcm onto the
 * crypto table already at stack top (-1). We forward-declare instead of
 * pulling in a header per function for now. */
void fan_crypto_gcm_register(lua_State *L);

int fan_crypto_available(void) {
#if FAN_WITH_OPENSSL
    return 1;
#else
    return 0;
#endif
}

#if FAN_WITH_OPENSSL

/* Compute an MD5 digest via EVP so we're insulated from the classic
 * MD5_* API's OpenSSL 3 deprecation. Uses the one-shot digest APIs
 * (EVP_Q_digest) when available, falling back to the EVP_MD_CTX path
 * on OpenSSL 1.x. */
static int md5_raw(const unsigned char *data, size_t len,
                   unsigned char out[16], const char **errmsg) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    /* EVP_Q_digest takes `size_t *mdlen` on OpenSSL 3+ (glibc 8-byte),
     * NOT `unsigned int *` — the wrong type here silently writes an
     * 8-byte length into a 4-byte slot and trips -fstack-protector,
     * which is exactly the "stack smashing detected" abort we hit on
     * arm1's Ubuntu 22.04 CI image. */
    size_t outlen = 0;
    if (!EVP_Q_digest(NULL, "MD5", NULL, data, len, out, &outlen)) {
        if (errmsg) *errmsg = "EVP_Q_digest(MD5) failed";
        return -1;
    }
    if (outlen != 16) {
        if (errmsg) *errmsg = "unexpected MD5 digest length";
        return -1;
    }
    return 0;
#else
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) { if (errmsg) *errmsg = "EVP_MD_CTX_new failed"; return -1; }
    unsigned int outlen = 0;
    if (EVP_DigestInit_ex(ctx, EVP_md5(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, out, &outlen) != 1 ||
        outlen != 16) {
        EVP_MD_CTX_free(ctx);
        if (errmsg) *errmsg = "MD5 EVP digest failed";
        return -1;
    }
    EVP_MD_CTX_free(ctx);
    return 0;
#endif
}

static void push_hex(lua_State *L, const unsigned char *bytes, size_t n) {
    static const char hexd[] = "0123456789abcdef";
    /* Use a small fixed buffer since MD5 output is bounded (16 bytes -> 32
     * hex chars); this keeps the hot ETag path allocation-free below the
     * Lua interning layer. */
    char buf[64];
    for (size_t i = 0; i < n; i++) {
        buf[i * 2]     = hexd[(bytes[i] >> 4) & 0xf];
        buf[i * 2 + 1] = hexd[bytes[i]        & 0xf];
    }
    lua_pushlstring(L, buf, n * 2);
}

/* fan.crypto.md5(data) -> lowercase hex digest string.
 * Nil / non-string input surfaces as an argument error to catch typos
 * (v1's third-party md5.digest() also errored on bad types). */
static int l_md5(lua_State *L) {
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    unsigned char out[16];
    const char *err = NULL;
    if (md5_raw((const unsigned char *)data, len, out, &err) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "md5 failed");
        return 2;
    }
    push_hex(L, out, 16);
    return 1;
}

/* fan.crypto.md5_binary(data) -> raw 16-byte digest string. */
static int l_md5_binary(lua_State *L) {
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    unsigned char out[16];
    const char *err = NULL;
    if (md5_raw((const unsigned char *)data, len, out, &err) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "md5 failed");
        return 2;
    }
    lua_pushlstring(L, (const char *)out, 16);
    return 1;
}

#else /* !FAN_WITH_OPENSSL */

static int l_md5(lua_State *L) {
    (void)L;
    lua_pushnil(L);
    lua_pushliteral(L, "openssl not compiled in (build with -DFAN_WITH_OPENSSL=ON)");
    return 2;
}
static int l_md5_binary(lua_State *L) { return l_md5(L); }

#endif /* FAN_WITH_OPENSSL */

static int l_available(lua_State *L) {
    lua_pushboolean(L, fan_crypto_available());
    return 1;
}

static const luaL_Reg crypto_funcs[] = {
    {"available",  l_available},
    {"md5",        l_md5},
    {"md5_binary", l_md5_binary},
    {NULL, NULL},
};

void fan_crypto_register(lua_State *L) {
    /* module table 'fan' is on top of the stack when the caller invokes us
     * (mirrors every other fan_*_register). */
    lua_newtable(L);
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, crypto_funcs, 0);
#else
    luaL_register(L, NULL, crypto_funcs);
#endif
    lua_pushboolean(L, fan_crypto_available());
    lua_setfield(L, -2, "enabled");
    /* GCM submodule: fan.crypto.gcm.encrypt / decrypt (crypto/gcm.c). */
    fan_crypto_gcm_register(L);
    lua_setfield(L, -2, "crypto");
}
