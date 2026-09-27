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
#include <event2/bufferevent_ssl.h>

/* Process-wide client context. Created lazily, freed at process exit by the
 * OS; there is exactly one, so this is a bounded, intentional singleton (not a
 * per-connection leak — the v2 rule is "no unbounded static growth"). */
static SSL_CTX *g_client_ctx = NULL;

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
    /* Load the system default trust store for peer verification. */
    SSL_CTX_set_default_verify_paths(ctx);
    g_client_ctx = ctx;
}

int fan_tls_available(void) { return 1; }

struct bufferevent *fan_tls_client_bev(struct event_base *base,
                                       const char *host,
                                       int verify_peer,
                                       int verify_host,
                                       const char **err) {
    fan_tls_init();
    if (!g_client_ctx) { if (err) *err = "SSL_CTX init failed"; return NULL; }

    SSL *ssl = SSL_new(g_client_ctx);
    if (!ssl) { if (err) *err = "SSL_new failed"; return NULL; }

    if (host && host[0]) {
        /* SNI so virtual-hosted TLS servers pick the right cert. */
        SSL_set_tlsext_host_name(ssl, host);
        if (verify_host) {
            SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            SSL_set1_host(ssl, host);
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
    /* Tolerate servers that close without a clean TLS shutdown (common). */
    bufferevent_openssl_set_allow_dirty_shutdown(bev, 1);
#endif
    return bev;
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
