/*
 * crypto/gcm.c — AES-GCM (with AAD) binding, registered as fan.crypto.gcm.
 *
 * Contract (matches webase's luagcm.c, so v1 apps port cleanly):
 *   fan.crypto.gcm.encrypt(key, nonce, plaintext [, aad]) -> ct, tag
 *   fan.crypto.gcm.decrypt(key, nonce, ciphertext, tag [, aad]) -> pt | nil
 *
 * Invariants (all argument checks are hard errors, not (nil, msg), because
 * hitting one indicates a coding bug — v1's binding did the same):
 *   key    : 16 bytes (AES-128-GCM) or 32 bytes (AES-256-GCM)
 *   nonce  : 12 bytes  (standard GCM IV)
 *   tag    : 16 bytes  (128-bit auth tag; GET_TAG output is always 16)
 *   aad    : optional; empty string == no AAD
 *
 * Decrypt returns nil on tag mismatch (not an error) so callers can branch
 * `if not pt then reject() end`. Any *internal* failure (context alloc,
 * cipher init) is still surfaced as a Lua error — those are never a
 * legitimate outcome and shouldn't be silently swallowed.
 *
 * Memory model: we allocate ciphertext / plaintext in a `luaL_Buffer`
 * (Lua-owned) instead of parking a userdata for the whole call.
 * luaL_prepbuffsize with pt_len+16 gives us enough space for GCM's
 * padding-free output plus the finalize slack. On success we
 * `luaL_pushresult` and the buffer becomes the string return; on
 * failure the buffer is discarded when the Lua stack unwinds.
 */
#include "../platform.h"

#include <lauxlib.h>
#include <string.h>

#if FAN_WITH_OPENSSL
#  include <openssl/evp.h>
#endif

#if FAN_WITH_OPENSSL

/* Pick the EVP_CIPHER by key length. Returns NULL for invalid sizes so
 * the caller can emit a precise argument error. */
static const EVP_CIPHER *cipher_for_keylen(size_t keylen) {
    switch (keylen) {
        case 16: return EVP_aes_128_gcm();
        case 32: return EVP_aes_256_gcm();
        default: return NULL;
    }
}

/* gcm.encrypt(key, nonce, plaintext [, aad]) -> ct, tag */
static int l_gcm_encrypt(lua_State *L) {
    size_t key_len, nonce_len, pt_len, aad_len = 0;
    const unsigned char *key   = (const unsigned char *)luaL_checklstring(L, 1, &key_len);
    const unsigned char *nonce = (const unsigned char *)luaL_checklstring(L, 2, &nonce_len);
    const unsigned char *pt    = (const unsigned char *)luaL_checklstring(L, 3, &pt_len);
    const unsigned char *aad   = NULL;
    if (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) {
        aad = (const unsigned char *)luaL_checklstring(L, 4, &aad_len);
    }

    const EVP_CIPHER *cipher = cipher_for_keylen(key_len);
    if (!cipher) return luaL_error(L, "key must be 16 or 32 bytes, got %d", (int)key_len);
    if (nonce_len != 12) return luaL_error(L, "nonce must be 12 bytes, got %d", (int)nonce_len);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return luaL_error(L, "EVP_CIPHER_CTX_new failed");

    /* Prepare the output buffer up front so a mid-stream failure just
     * unwinds naturally when the Lua stack collapses. */
    luaL_Buffer B;
    /* +16 to give EVP_EncryptFinal_ex headroom even though GCM has no
     * padding (matches v1's slack). */
    unsigned char *ct = (unsigned char *)luaL_buffinitsize(L, &B, pt_len + 16);
    int outlen = 0, ct_len = 0;

    if (EVP_EncryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)nonce_len, NULL) != 1 ||
        EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return luaL_error(L, "GCM encrypt init failed");
    }

    if (aad_len > 0 &&
        EVP_EncryptUpdate(ctx, NULL, &outlen, aad, (int)aad_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return luaL_error(L, "GCM AAD update failed");
    }

    if (EVP_EncryptUpdate(ctx, ct, &outlen, pt, (int)pt_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return luaL_error(L, "GCM encrypt update failed");
    }
    ct_len = outlen;

    int finlen = 0;
    if (EVP_EncryptFinal_ex(ctx, ct + ct_len, &finlen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return luaL_error(L, "GCM encrypt final failed");
    }
    ct_len += finlen;

    unsigned char tag[16];
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return luaL_error(L, "GCM tag extract failed");
    }
    EVP_CIPHER_CTX_free(ctx);

    luaL_pushresultsize(&B, (size_t)ct_len);   /* ciphertext string on stack */
    lua_pushlstring(L, (const char *)tag, 16); /* auth tag string on stack */
    return 2;
}

/* gcm.decrypt(key, nonce, ciphertext, tag [, aad]) -> pt | nil */
static int l_gcm_decrypt(lua_State *L) {
    size_t key_len, nonce_len, ct_len, tag_len, aad_len = 0;
    const unsigned char *key   = (const unsigned char *)luaL_checklstring(L, 1, &key_len);
    const unsigned char *nonce = (const unsigned char *)luaL_checklstring(L, 2, &nonce_len);
    const unsigned char *ct    = (const unsigned char *)luaL_checklstring(L, 3, &ct_len);
    const unsigned char *tag   = (const unsigned char *)luaL_checklstring(L, 4, &tag_len);
    const unsigned char *aad   = NULL;
    if (lua_gettop(L) >= 5 && !lua_isnil(L, 5)) {
        aad = (const unsigned char *)luaL_checklstring(L, 5, &aad_len);
    }

    const EVP_CIPHER *cipher = cipher_for_keylen(key_len);
    if (!cipher) return luaL_error(L, "key must be 16 or 32 bytes, got %d", (int)key_len);
    if (nonce_len != 12) return luaL_error(L, "nonce must be 12 bytes, got %d", (int)nonce_len);
    if (tag_len != 16)   return luaL_error(L, "tag must be 16 bytes, got %d", (int)tag_len);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return luaL_error(L, "EVP_CIPHER_CTX_new failed");

    luaL_Buffer B;
    unsigned char *pt = (unsigned char *)luaL_buffinitsize(L, &B, ct_len + 16);
    int outlen = 0, pt_len = 0;

    if (EVP_DecryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)nonce_len, NULL) != 1 ||
        EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return luaL_error(L, "GCM decrypt init failed");
    }

    if (aad_len > 0 &&
        EVP_DecryptUpdate(ctx, NULL, &outlen, aad, (int)aad_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return luaL_error(L, "GCM AAD update failed");
    }

    if (EVP_DecryptUpdate(ctx, pt, &outlen, ct, (int)ct_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return luaL_error(L, "GCM decrypt update failed");
    }
    pt_len = outlen;

    /* SET_TAG needs a writable buffer; we can't hand OpenSSL a
     * `const` pointer even though it only reads. */
    unsigned char tagbuf[16];
    memcpy(tagbuf, tag, 16);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tagbuf) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return luaL_error(L, "GCM set-tag failed");
    }

    int finlen = 0;
    int verified = EVP_DecryptFinal_ex(ctx, pt + pt_len, &finlen);
    EVP_CIPHER_CTX_free(ctx);

    if (!verified) {
        /* tag mismatch — return nil (not an error) */
        lua_pushnil(L);
        return 1;
    }
    pt_len += finlen;
    luaL_pushresultsize(&B, (size_t)pt_len);
    return 1;
}

#else /* !FAN_WITH_OPENSSL */

static int l_gcm_encrypt(lua_State *L) {
    (void)L;
    return luaL_error(L, "openssl not compiled in (build with -DFAN_WITH_OPENSSL=ON)");
}
static int l_gcm_decrypt(lua_State *L) {
    (void)L;
    return luaL_error(L, "openssl not compiled in (build with -DFAN_WITH_OPENSSL=ON)");
}

#endif /* FAN_WITH_OPENSSL */

static const luaL_Reg gcm_funcs[] = {
    {"encrypt", l_gcm_encrypt},
    {"decrypt", l_gcm_decrypt},
    {NULL, NULL},
};

/* Push the gcm subtable and setfield("gcm") on the parent (fan.crypto) that
 * must be on stack top when we're called. */
void fan_crypto_gcm_register(lua_State *L) {
    lua_newtable(L);
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, gcm_funcs, 0);
#else
    luaL_register(L, NULL, gcm_funcs);
#endif
    lua_setfield(L, -2, "gcm");
}
