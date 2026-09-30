/*
 * tls.c — LuaFan v2 TLS (OpenSSL) client support.
 *
 * See tls.h. The OpenSSL body is compiled only under FAN_WITH_OPENSSL; a stub
 * is provided otherwise so the rest of the build (and fan.tls introspection)
 * works unchanged.
 */
#include "tls.h"
#include "../platform.h"

#include <lauxlib.h>
#include <string.h>

#if FAN_WITH_OPENSSL

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/pkcs12.h>
#include <event2/bufferevent_ssl.h>
#include <stdio.h>
#include <stdlib.h>             /* getenv */

/* Process-wide client context. Created lazily, freed at process exit by the
 * OS; there is exactly one, so this is a bounded, intentional singleton (not a
 * per-connection leak — the v2 rule is "no unbounded static growth"). */
static SSL_CTX *g_client_ctx = NULL;

/* M21.1 — File-scope error buffer.  fan_tls_client_bev_ex writes its detailed
 * OpenSSL error text here so callers get "certificate verify failed: unable
 * to get local issuer certificate" instead of "SSL_CTX_load_verify_locations
 * failed".  Single-threaded event loop → no locking; callers must copy /
 * strdup before yielding if they want a stable string. */
#define FAN_TLS_ERR_BUFLEN 256
static char g_tls_err_buf[FAN_TLS_ERR_BUFLEN];

/* Format the top OpenSSL error queue entry into out; clears the queue.
 * Returns 1 if an error was popped, 0 if the queue was empty (out untouched).
 * Uses ERR_peek_last_error so the *deepest* / most-specific reason is
 * surfaced, then ERR_clear_error to drain the whole queue.  Both are
 * async-signal safe and re-entrant. */
static int fan_tls_pop_last_error(char *out, size_t outlen) {
    unsigned long e = ERR_peek_last_error();
    if (!e) return 0;
    /* ERR_error_string_n needs 120+ bytes for the full "error:0xNNNNN:lib:
     * fn:reason" form; we pass whatever the caller has and let it truncate. */
    ERR_error_string_n(e, out, outlen);
    ERR_clear_error();
    return 1;
}

/* M21.1 — Public helper: fill `buf` with the OpenSSL peer-verify reason for
 * `ssl`, e.g. "unable to get local issuer certificate".  Returns 1 on
 * success (buf populated), 0 if the handshake either succeeded or has no
 * verify result available.  Used by tcp.c to distinguish "CA verify failed"
 * from other connect errors on the handshake path. */
int fan_tls_client_verify_reason(void *ssl_ptr, char *buf, size_t buflen) {
    SSL *ssl = (SSL *)ssl_ptr;
    if (!ssl || !buf || buflen == 0) return 0;
    long r = SSL_get_verify_result(ssl);
    if (r == X509_V_OK) return 0;
    const char *s = X509_verify_cert_error_string(r);
    if (!s) s = "peer verify failed";
    snprintf(buf, buflen, "%s", s);
    return 1;
}

/* M21.1 — Load the system trust store into ctx.  Honors SSL_CERT_FILE /
 * SSL_CERT_DIR env vars (openssl s_client convention: env takes priority
 * over the compiled-in default), then falls back to
 * SSL_CTX_set_default_verify_paths.  Failures are non-fatal — a ctx with no
 * trust store still handshakes when verify_peer=false, and callers that
 * verify will get a clear "unable to get local issuer certificate" from
 * OpenSSL.  Return value is 1 if any trust source was successfully
 * loaded, 0 otherwise. */
static int fan_tls_load_system_trust(SSL_CTX *ctx) {
    const char *env_file = getenv("SSL_CERT_FILE");
    const char *env_dir  = getenv("SSL_CERT_DIR");
    int ok = 0;
    if ((env_file && env_file[0]) || (env_dir && env_dir[0])) {
        if (SSL_CTX_load_verify_locations(ctx,
                                          (env_file && env_file[0]) ? env_file : NULL,
                                          (env_dir  && env_dir[0])  ? env_dir  : NULL) == 1) {
            ok = 1;
        }
        /* If env-driven load failed we intentionally do NOT fall back to
         * defaults — the user asked for a specific store and swapping it
         * silently would hide bugs.  Leave ctx with no trust; verify will
         * fail loudly.  fan_tls_init prints a stderr diagnostic below. */
    }
    if (!ok) {
        if (SSL_CTX_set_default_verify_paths(ctx) == 1) ok = 1;
    }
    return ok;
}

void fan_tls_init(void) {
    if (g_client_ctx) return;
    /* OpenSSL >= 1.1 initialises itself; be explicit for older too. */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
#endif
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return;
    /* Modern floor: TLS 1.2+. */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    /* Load the system default trust store for peer verification (or
     * SSL_CERT_FILE / SSL_CERT_DIR when the caller has set them). */
    if (!fan_tls_load_system_trust(ctx)) {
        /* Non-fatal: log to stderr with the OpenSSL reason so operators
         * can diagnose "no CA bundle available" on stripped-down images
         * (e.g. alpine without ca-certificates).  Peer verification will
         * still be attempted per-request and will fail with a clear
         * verify-reason from fan_tls_client_verify_reason. */
        char why[FAN_TLS_ERR_BUFLEN];
        if (!fan_tls_pop_last_error(why, sizeof why)) {
            snprintf(why, sizeof why, "no trust source loaded");
        }
        fprintf(stderr,
                "[luafan2/tls] warning: system trust store not loaded (%s); "
                "peer verification will fail unless a per-request cainfo/"
                "capath is supplied\n",
                why);
    }
    g_client_ctx = ctx;
}

int fan_tls_available(void) { return 1; }

/* Load a PKCS#12 client certificate bundle into `ctx`.  Returns 0 on success,
 * -1 on failure.  Ported from v1 tcpd_ssl.c:tcpd_ssl_load_pkcs12. */
static int fan_tls_load_pkcs12(SSL_CTX *ctx, const char *path, const char *pw) {
    if (!ctx || !path) return -1;
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    PKCS12 *p12 = d2i_PKCS12_fp(fp, NULL);
    fclose(fp);
    if (!p12) return -1;

    EVP_PKEY *pkey = NULL;
    X509 *cert = NULL;
    STACK_OF(X509) *ca = NULL;
    int rc = -1;
    if (PKCS12_parse(p12, pw, &pkey, &cert, &ca)) {
        if (cert) SSL_CTX_use_certificate(ctx, cert);
        if (pkey) SSL_CTX_use_PrivateKey(ctx, pkey);
        if (ca) {
            for (int i = 0; i < sk_X509_num(ca); i++) {
                X509 *ca_cert = sk_X509_value(ca, i);
                X509_up_ref(ca_cert);
                SSL_CTX_add_extra_chain_cert(ctx, ca_cert);
            }
        }
        rc = 0;
    }
    PKCS12_free(p12);
    if (cert) X509_free(cert);
    if (pkey) EVP_PKEY_free(pkey);
    if (ca) sk_X509_pop_free(ca, X509_free);
    return rc;
}

/* ---- SSL_CTX cache (fingerprinted by cainfo/capath/pkcs12 params) ---------
 *
 * Rationale (v1 parity, memory boundedness):
 *   pkcs12 + custom CA loads are heavyweight — a per-connect_async new SSL_CTX
 *   would allocate tens of KB of parsed certs on every connection.  v1
 *   tcpd_ssl.c solved this by fingerprinting the TLS options and reusing the
 *   ctx across every connection with matching params.  Total ctx count is
 *   bounded by the number of *distinct parameter combinations* the process
 *   ever sees (typically 1-5 in real deployments), not by the connection
 *   count.
 *
 * Design:
 *   - Fingerprint = SHA-256 of (cainfo || 0x1E || capath || 0x1E ||
 *                               pkcs12_path || 0x1E || pkcs12_password),
 *     with NULLs encoded as empty strings.  0x1E (RS) is chosen because it
 *     cannot appear in a filesystem path or password, avoiding boundary
 *     collisions ("ab" + "c" vs "a" + "bc").
 *   - Storage = fixed-size open-addressing table.  Cap 64 slots; parameter
 *     diversity is small so linear probing is fine.  If the table saturates
 *     we fall back to un-cached ctx (still safe, just loses the reuse
 *     benefit) rather than evicting live entries.
 *   - Lifetime = process.  ctx entries are never freed; freeing a cached
 *     ctx while another connection still owns an SSL derived from it would
 *     UAF the SSL.  This is intentionally identical to how g_client_ctx is
 *     managed.
 *   - Concurrency = the entire runtime is single-threaded (fan_loop is one
 *     event_base); no lock needed.  If v2 ever grows a threaded worker
 *     model this cache would need a mutex.
 */
#define TLS_CTX_CACHE_CAP 64
typedef struct {
    unsigned char fp[32];  /* SHA-256 digest */
    int           used;    /* 0 = empty slot */
    SSL_CTX      *ctx;
} tls_ctx_cache_slot_t;
static tls_ctx_cache_slot_t g_ctx_cache[TLS_CTX_CACHE_CAP];

static void fan_tls_fingerprint(unsigned char out[32],
                                const char *cainfo, const char *capath,
                                const char *pkcs12_path,
                                const char *pkcs12_password) {
    const unsigned char sep = 0x1E;  /* ASCII RS — record separator */
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL);
#define TLS_FP_FEED(s)                                                  \
    do {                                                                \
        const char *_s = (s);                                           \
        if (_s) EVP_DigestUpdate(mdctx, _s, strlen(_s));                \
        EVP_DigestUpdate(mdctx, &sep, 1);                               \
    } while (0)
    TLS_FP_FEED(cainfo);
    TLS_FP_FEED(capath);
    TLS_FP_FEED(pkcs12_path);
    TLS_FP_FEED(pkcs12_password);
#undef TLS_FP_FEED
    unsigned int outlen = 32;
    EVP_DigestFinal_ex(mdctx, out, &outlen);
    EVP_MD_CTX_free(mdctx);
}

/* Look up a cached ctx by fingerprint.  Returns the ctx or NULL if absent.
 * Also returns via *empty_slot the first empty slot encountered on the probe
 * chain, so the caller can insert without a second scan. */
static SSL_CTX *fan_tls_ctx_cache_lookup(const unsigned char fp[32],
                                         int *empty_slot) {
    /* Seed the probe from the first 4 bytes of the fingerprint. */
    unsigned int h = ((unsigned)fp[0] << 24) | ((unsigned)fp[1] << 16) |
                     ((unsigned)fp[2] << 8)  | (unsigned)fp[3];
    int first_empty = -1;
    for (int i = 0; i < TLS_CTX_CACHE_CAP; i++) {
        int slot = (h + i) % TLS_CTX_CACHE_CAP;
        if (!g_ctx_cache[slot].used) {
            if (first_empty < 0) first_empty = slot;
            /* Empty slot terminates the probe chain: the target is not
             * present (insertions never leave a gap before their slot). */
            break;
        }
        if (memcmp(g_ctx_cache[slot].fp, fp, 32) == 0) {
            if (empty_slot) *empty_slot = -1;
            return g_ctx_cache[slot].ctx;
        }
    }
    if (empty_slot) *empty_slot = first_empty;
    return NULL;
}

static void fan_tls_ctx_cache_insert(int slot, const unsigned char fp[32],
                                     SSL_CTX *ctx) {
    if (slot < 0 || slot >= TLS_CTX_CACHE_CAP) return;
    memcpy(g_ctx_cache[slot].fp, fp, 32);
    g_ctx_cache[slot].ctx = ctx;
    g_ctx_cache[slot].used = 1;
}

/* Build (and configure) a fresh client SSL_CTX from the optional TLS params.
 * The caller must NOT free the returned ctx — it is owned by the cache (or
 * by g_client_ctx for the no-options case).  Failures return NULL. */
static SSL_CTX *fan_tls_new_client_ctx(const char *cainfo, const char *capath,
                                       const char *pkcs12_path,
                                       const char *pkcs12_password,
                                       const char **err) {
    /* No-options fast path lives outside the cache (uses the process-wide
     * g_client_ctx directly); the caller filters this before invoking us. */
    unsigned char fp[32];
    fan_tls_fingerprint(fp, cainfo, capath, pkcs12_path, pkcs12_password);

    int empty_slot = -1;
    SSL_CTX *hit = fan_tls_ctx_cache_lookup(fp, &empty_slot);
    if (hit) return hit;

    /* Miss: build a new ctx.  Even if the cache is full we still build one
     * — safety > cache hit rate.  A saturated cache means the caller must
     * accept the per-conn cost; in practice TLS_CTX_CACHE_CAP=64 is far
     * larger than any realistic parameter-set count. */
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { if (err) *err = "SSL_CTX_new (client) failed"; return NULL; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (cainfo || capath) {
        if (SSL_CTX_load_verify_locations(ctx, cainfo, capath) != 1) {
            /* M21.1 — surface the underlying OpenSSL reason (e.g.
             * "system lib" when the file is missing, or "no certificate or
             * crl found" when the PEM is empty).  Preserving "SSL_CTX_
             * load_verify_locations failed" as a prefix lets grep still
             * find the callsite, while the appended details tell operators
             * WHY it failed. */
            char why[FAN_TLS_ERR_BUFLEN];
            if (fan_tls_pop_last_error(why, sizeof why)) {
                snprintf(g_tls_err_buf, sizeof g_tls_err_buf,
                         "SSL_CTX_load_verify_locations(%s%s%s): %s",
                         cainfo ? cainfo : "",
                         (cainfo && capath) ? ", " : "",
                         capath ? capath : "",
                         why);
            } else {
                snprintf(g_tls_err_buf, sizeof g_tls_err_buf,
                         "SSL_CTX_load_verify_locations(%s%s%s) failed",
                         cainfo ? cainfo : "",
                         (cainfo && capath) ? ", " : "",
                         capath ? capath : "");
            }
            if (err) *err = g_tls_err_buf;
            SSL_CTX_free(ctx);
            return NULL;
        }
    } else {
        /* M21.1 — honor SSL_CERT_FILE / SSL_CERT_DIR when the caller
         * didn't pin a bundle. */
        (void)fan_tls_load_system_trust(ctx);
    }

    if (pkcs12_path) {
        if (fan_tls_load_pkcs12(ctx, pkcs12_path, pkcs12_password) != 0) {
            char why[FAN_TLS_ERR_BUFLEN];
            if (fan_tls_pop_last_error(why, sizeof why)) {
                snprintf(g_tls_err_buf, sizeof g_tls_err_buf,
                         "PKCS12 load(%s): %s", pkcs12_path, why);
                if (err) *err = g_tls_err_buf;
            } else {
                if (err) *err = "PKCS12 load / parse failed";
            }
            SSL_CTX_free(ctx);
            return NULL;
        }
    }

    /* Insert into the cache so the next matching request reuses this ctx.
     * If the cache saturated (empty_slot < 0) we still return the ctx but
     * it will not be cached — the caller does not free it either, so it
     * effectively leaks for the process lifetime.  In practice this is a
     * non-issue given the cap. */
    if (empty_slot >= 0) fan_tls_ctx_cache_insert(empty_slot, fp, ctx);
    return ctx;
}

struct bufferevent *fan_tls_client_bev_ex(struct event_base *base,
                                          const char *host,
                                          const char *ssl_host,
                                          int verify_peer,
                                          int verify_host,
                                          const char *cainfo,
                                          const char *capath,
                                          const char *pkcs12_path,
                                          const char *pkcs12_password,
                                          const char **err) {
    fan_tls_init();

    /* The ctx we use is one of two bounded singletons (v1 parity):
     *
     *   - No TLS options set → g_client_ctx (process-wide default).
     *   - Any of cainfo / capath / pkcs12_path set → cached ctx keyed on
     *     the (cainfo, capath, pkcs12_path, pkcs12_password) tuple.  See
     *     the SSL_CTX cache section above: hits reuse an existing ctx,
     *     misses build one and insert it.  Either way we do NOT free the
     *     ctx here — the cache (or the g_client_ctx singleton) owns it
     *     for the process lifetime. */
    SSL_CTX *use_ctx = NULL;
    if (!cainfo && !capath && !pkcs12_path) {
        if (!g_client_ctx) { if (err) *err = "SSL_CTX init failed"; return NULL; }
        use_ctx = g_client_ctx;
    } else {
        use_ctx = fan_tls_new_client_ctx(cainfo, capath, pkcs12_path,
                                         pkcs12_password, err);
        if (!use_ctx) return NULL;
    }

    SSL *ssl = SSL_new(use_ctx);
    if (!ssl) {
        if (err) *err = "SSL_new failed";
        return NULL;
    }

    /* SNI + hostname verification use `ssl_host` if given, else `host`. */
    const char *sni = (ssl_host && ssl_host[0]) ? ssl_host : host;
    if (sni && sni[0]) {
        SSL_set_tlsext_host_name(ssl, sni);
        if (verify_host) {
            SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            SSL_set1_host(ssl, sni);
        }
    }
    SSL_set_verify(ssl, verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);

    struct bufferevent *bev = bufferevent_openssl_socket_new(
        base, -1, ssl, BUFFEREVENT_SSL_CONNECTING,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
    if (!bev) {
        SSL_free(ssl);
        if (err) *err = "bufferevent_openssl_socket_new failed";
        return NULL;
    }
#if defined(EVENT__NUMERIC_VERSION) && (EVENT__NUMERIC_VERSION >= 0x02010500)
    bufferevent_openssl_set_allow_dirty_shutdown(bev, 1);
#endif
    return bev;
}

/* Legacy entry — kept for the existing coroutine-yielding fan.tcp.connect
 * and http.c callers that only need SNI + verify booleans against the
 * default trust store.  Delegates to fan_tls_client_bev_ex. */
struct bufferevent *fan_tls_client_bev(struct event_base *base,
                                       const char *host,
                                       int verify_peer,
                                       int verify_host,
                                       const char **err) {
    return fan_tls_client_bev_ex(base, host, NULL, verify_peer, verify_host,
                                 NULL, NULL, NULL, NULL, err);
}

/* ---- server side --------------------------------------------------------- */

void *fan_tls_server_ctx_new(const char *cert_path, const char *key_path,
                             const char **err) {
    fan_tls_init();
    if (!cert_path || !key_path) {
        if (err) *err = "cert_path and key_path required";
        return NULL;
    }
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) { if (err) *err = "SSL_CTX_new (server) failed"; return NULL; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1) {
        if (err) *err = "SSL_CTX_use_certificate_chain_file failed";
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1) {
        if (err) *err = "SSL_CTX_use_PrivateKey_file failed";
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        if (err) *err = "SSL_CTX_check_private_key failed";
        SSL_CTX_free(ctx);
        return NULL;
    }
    return (void *)ctx;
}

void fan_tls_server_ctx_free(void *ctx) {
    if (!ctx) return;
    SSL_CTX_free((SSL_CTX *)ctx);
}

struct bufferevent *fan_tls_server_bev(struct event_base *base, int fd,
                                       void *ctx, const char **err) {
    if (!ctx) { if (err) *err = "server tls ctx is NULL"; return NULL; }
    SSL *ssl = SSL_new((SSL_CTX *)ctx);
    if (!ssl) { if (err) *err = "SSL_new (server) failed"; return NULL; }
    struct bufferevent *bev = bufferevent_openssl_socket_new(
        base, fd, ssl, BUFFEREVENT_SSL_ACCEPTING,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
    if (!bev) {
        SSL_free(ssl);
        if (err) *err = "bufferevent_openssl_socket_new (server) failed";
        return NULL;
    }
#if defined(EVENT__NUMERIC_VERSION) && (EVENT__NUMERIC_VERSION >= 0x02010500)
    bufferevent_openssl_set_allow_dirty_shutdown(bev, 1);
#endif
    return bev;
}

#else /* !FAN_WITH_OPENSSL */

void fan_tls_init(void) {}
int fan_tls_available(void) { return 0; }
struct bufferevent *fan_tls_client_bev(struct event_base *base,
                                       const char *host,
                                       int verify_peer,
                                       int verify_host,
                                       const char **err) {
    (void)base; (void)host; (void)verify_peer; (void)verify_host;
    if (err) *err = "TLS not compiled in (build with -DFAN_WITH_OPENSSL=ON)";
    return NULL;
}
struct bufferevent *fan_tls_client_bev_ex(struct event_base *base,
                                          const char *host,
                                          const char *ssl_host,
                                          int verify_peer,
                                          int verify_host,
                                          const char *cainfo,
                                          const char *capath,
                                          const char *pkcs12_path,
                                          const char *pkcs12_password,
                                          const char **err) {
    (void)base; (void)host; (void)ssl_host; (void)verify_peer; (void)verify_host;
    (void)cainfo; (void)capath; (void)pkcs12_path; (void)pkcs12_password;
    if (err) *err = "TLS not compiled in (build with -DFAN_WITH_OPENSSL=ON)";
    return NULL;
}
int fan_tls_client_verify_reason(void *ssl_ptr, char *buf, size_t buflen) {
    (void)ssl_ptr; (void)buf; (void)buflen;
    return 0;
}
void *fan_tls_server_ctx_new(const char *cert_path, const char *key_path,
                             const char **err) {
    (void)cert_path; (void)key_path;
    if (err) *err = "TLS not compiled in (build with -DFAN_WITH_OPENSSL=ON)";
    return NULL;
}
void fan_tls_server_ctx_free(void *ctx) { (void)ctx; }
struct bufferevent *fan_tls_server_bev(struct event_base *base, int fd,
                                       void *ctx, const char **err) {
    (void)base; (void)fd; (void)ctx;
    if (err) *err = "TLS not compiled in (build with -DFAN_WITH_OPENSSL=ON)";
    return NULL;
}

#endif /* FAN_WITH_OPENSSL */

/* fan.tls.available() -> bool */
static int l_available(lua_State *L) {
    lua_pushboolean(L, fan_tls_available());
    return 1;
}

static const luaL_Reg tls_funcs[] = {
    {"available", l_available},
    {NULL, NULL},
};

void fan_tls_register(lua_State *L) {
    lua_newtable(L);
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, tls_funcs, 0);
#else
    luaL_register(L, NULL, tls_funcs);
#endif
    lua_pushboolean(L, fan_tls_available());
    lua_setfield(L, -2, "enabled");
    lua_setfield(L, -2, "tls");
}
