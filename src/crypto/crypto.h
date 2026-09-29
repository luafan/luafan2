/*
 * crypto/crypto.h — LuaFan v2 crypto helpers (M12.1).
 *
 * Registers the `fan.crypto` table on the module at stack top (-1).
 * Functions provided:
 *   fan.crypto.md5(data)            -> lowercase-hex digest string (32 chars)
 *   fan.crypto.md5_binary(data)     -> raw 16-byte digest string
 *   fan.crypto.available()          -> boolean; false when built without OpenSSL
 *   fan.crypto.gcm.encrypt(...)     -> ciphertext, tag   (see gcm.c)
 *   fan.crypto.gcm.decrypt(...)     -> plaintext | nil   (see gcm.c)
 *
 * We deliberately expose a tiny surface — enough to unblock the webase
 * port (ETag from md5) and the AES-GCM w/ AAD binding v1 apps used via
 * the external `gcm.so`. Any richer crypto surface (HMAC, key
 * derivation, generic EVP) should go through openssl.* on sandbox
 * builds, not here.
 */
#ifndef FAN2_CRYPTO_H
#define FAN2_CRYPTO_H

#include <lua.h>

int fan_crypto_available(void);
void fan_crypto_register(lua_State *L);

#endif /* FAN2_CRYPTO_H */
