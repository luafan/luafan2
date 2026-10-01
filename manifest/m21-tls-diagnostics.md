# M21 — Pure-Lua HTTPS: CA bundle pinning + TLS diagnostics

## v1 HTTP server query compatibility

The HTTP server request-field contract is:
`req.query` is the raw, ordered query string; `req.params` is the
parsed query/form map with the last value winning on duplicate keys.
`req:query_values(name)` is the explicit lossless multi-value API:
it returns all decoded values for the URL query key in wire order,
without form-body values. The implementation does not introduce a
`query_map` field and does not reinterpret `req.query` as a table.

This is an additive API on both Lua and C HTTP server request objects.
For `?tag=a&tag=b`, `req.params.tag == "b"` while
`req:query_values("tag") == {"a", "b"}`. Query/form collision
handling in `req.params` remains last value wins; `query_values`
intentionally reports only URL query values.

Pure-Lua HTTPS through `fan.http_lua` regains v1 parity on two axes
that were quietly missing since the initial v2 landing:

1. **CA bundle pinning** — `fan.tcp.connect{ssl=true, cainfo=...,
   capath=...}` and the same knobs on `fan.http_lua.request` /
   `fan.http.request` now honour a per-request (or module-scoped)
   trust store instead of being locked to the process-wide
   `SSL_CTX_set_default_verify_paths` result.
2. **Actionable errors** — TLS and socket failures now surface the
   specific OpenSSL / evutil reason ("certificate verify failed:
   unable to get local issuer certificate", "Connection refused",
   "dns error: Name or service not known") instead of the pre-M21
   opaque `"connection error"`.

The C/libcurl backend already had both properties end-to-end since
M13 / M17-2; M21 brings the pure-Lua backend and every direct
`fan.tcp.connect` caller up to the same contract.

## Root cause of the pre-M21 gaps

- `fan.tcp.connect(host, port, opts)` (`src/net/tcp.c` `l_connect`)
  only parsed `{ssl, verify_peer, verify_host}` from opts and routed
  through the legacy `fan_tls_client_bev`.  The extended
  `fan_tls_client_bev_ex` bridge (with `cainfo`/`capath`/`pkcs12`
  parameters) had existed since M17-2 but was reachable only from
  `l_tcp_connect_async`.
- `fan.http_lua.request` (`lua/fan/http_lua.lua`) built its
  `tcp.connect` options table by hand and never even mentioned
  `cainfo` / `capath`, so `M.cainfo(path)` / `M.capath(path)`
  setters were stashed but never consumed by the Lua backend.
- `net/tcp.c` `conn_eventcb` mapped every `BEV_EVENT_ERROR` to a
  hard-coded `"connection error"` string.  Neither the OpenSSL
  error queue (`bufferevent_get_openssl_error`) nor the peer-verify
  reason (`SSL_get_verify_result`) nor the raw socket errno
  (`EVUTIL_SOCKET_ERROR`) nor the DNS resolver error
  (`bufferevent_socket_get_dns_error`) reached the caller.
- `net/tls.c` `SSL_CTX_load_verify_locations` failures were
  reported as the literal string `"SSL_CTX_load_verify_locations
  failed"` — no OpenSSL error queue lookup, no mention of the
  path that failed to load.
- The bootstrap `SSL_CTX_set_default_verify_paths` call swallowed
  its return value, so runtimes on stripped-down images (musl-based
  alpine without `ca-certificates`, minimal scratch layers) had no
  operator-visible signal that their trust store was empty.

## M21.1 — `net/tls.c` diagnostics + env-driven trust

New file-scope pieces:

- `fan_tls_pop_last_error(buf, buflen)` — pops the deepest entry
  from the OpenSSL error queue via `ERR_peek_last_error` /
  `ERR_error_string_n`, then drains the queue with
  `ERR_clear_error` so stale entries don't leak into the next
  request.
- `fan_tls_client_verify_reason(ssl, buf, buflen)` — exposed via
  `tls.h`; converts `SSL_get_verify_result` into a human-readable
  string via `X509_verify_cert_error_string`.  Returns 0 when
  the handshake succeeded or `ssl` is not a TLS bufferevent, so
  callers can short-circuit.
- `fan_tls_load_system_trust(ctx)` — honours `SSL_CERT_FILE` /
  `SSL_CERT_DIR` env vars (openssl `s_client` convention: env
  wins over the compiled-in default).  When the env is set but
  the load fails we intentionally do **not** fall back to
  `set_default_verify_paths` — silently swapping stores would
  hide bugs; the caller sees the specific verify reason on the
  next handshake.

`fan_tls_init` now checks the load return value and prints a
one-line stderr warning naming the OpenSSL reason when no trust
source could be loaded.  Non-fatal: `verify=false` requests still
work, and `verify=true` requests will fail with a clear reason.

The `fan_tls_new_client_ctx` `SSL_CTX_load_verify_locations`
failure path now writes the specific OpenSSL error into a
file-scope buffer `g_tls_err_buf` and points `*err` at it, so
callers see:

```
SSL_CTX_load_verify_locations(/missing/ca.pem): error:05880002:
  x509 certificate routines::system lib
```

instead of the old opaque literal.

## M21.2 — `net/tcp.c` error surface + option surface

`conn_describe_bev_error(bev, buf, buflen)` (new) walks the
priority chain **most-specific first**:

1. `bufferevent_socket_get_dns_error(bev)` → `evutil_gai_strerror`
   → `"dns error: <reason>"`
2. `bufferevent_get_openssl_error(bev)` (TLS bevs) →
   `ERR_error_string_n` → `"tls error: <reason>"`.
   `ERR_clear_error()` drains the queue for the next request.
3. `bufferevent_openssl_get_ssl(bev)` + `SSL_get_verify_result` →
   `X509_verify_cert_error_string` → `"tls verify failed: <reason>"`
   (catches the `SSL_VERIFY_NONE-would-have-succeeded` case where
   the queue is empty but the verify result is still bad).
4. `EVUTIL_SOCKET_ERROR()` → `evutil_socket_error_to_string` →
   `"socket error: <reason>"` (Connection refused, EHOSTUNREACH,
   ECONNRESET, etc.).
5. Absolute fallback `"connection error"` only when none of the
   above yielded actionable info.

`l_connect` (the coroutine-yielding `fan.tcp.connect`) gains
three new `opts` fields:

- `ssl_host = string` — override SNI + hostname verification
  identity.  Useful when connecting to a raw IP or a private
  alias while the origin cert has a canonical CN.
- `cainfo = path` — custom CA bundle path
  (`SSL_CTX_load_verify_locations`).
- `capath = path` — custom CA directory (OpenSSL hashed dir
  format).

When any of the three is set the code routes through
`fan_tls_client_bev_ex`; when none is set the legacy
`fan_tls_client_bev` path is preserved byte-for-byte so
unchanged callers hit exactly the pre-M21 codepath.  This keeps
the cached-ctx path (`g_client_ctx` singleton) intact for the
common no-options case.

## M21.3 — `fan/http_lua.lua` forwarding + module-scoped defaults

`http_lua.request` now threads `opts.cainfo` / `opts.capath` /
`opts.ssl_host` down into `fan.tcp.connect`, falling back to the
module-scoped `M._cainfo` / `M._capath` when the per-request
option is nil.

The `M.cainfo(path)` / `M.capath(path)` v1-compatibility setters
were stubs in the initial port (values stashed under `M.*` but
never read on the Lua path); M21 keeps the setter API identical
but the values now flow all the way down to
`SSL_CTX_load_verify_locations`.

`fan.http` (front-door shim) already forwarded `opts.cainfo` /
`opts.capath` to the C backend since M13; the shim's redirect
loop already re-populated the fields from `M._cainfo` /
`M._capath` per-hop for the Lua backend.  M21.3 is the final
link that closes the shim → `http_lua.request` → `fan.tcp.connect`
chain.

## Layered defaults (highest precedence first)

1. Per-request `opts.cainfo` / `opts.capath`.
2. Module-scoped `require("fan.http").cainfo(path)` /
   `require("fan.http_lua").cainfo(path)`.
3. `SSL_CERT_FILE` / `SSL_CERT_DIR` environment variables
   (openssl s_client convention).
4. `SSL_CTX_set_default_verify_paths` — the OpenSSL
   compile-time default (typically
   `/etc/ssl/certs/ca-certificates.crt` on Debian/Ubuntu,
   `/etc/pki/tls/certs/ca-bundle.crt` on RHEL family).
5. If none of the above load successfully, the process starts
   with an empty trust store and every `verify=true` request
   fails with `"tls verify failed: unable to get local issuer
   certificate"` — a clear operator signal.

## What did NOT change

- The C/libcurl backend (`fan.http` with the default backend on
  a build that has libcurl).  It already had per-request
  `CURLOPT_CAINFO` / `CURLOPT_CAPATH` support since M13 and
  reads `M._cainfo` / `M._capath` via the shim.  M21 explicitly
  did not touch that path.
- The failure shape when `SSL_CTX` construction fails
  synchronously.  `fan.tcp.connect` still raises via `luaL_error`
  (matching the pre-M21 API for bad opts); only the *content* of
  the error string changed.  Callers that want the `(nil, err)`
  shape can `pcall` the request — this is what
  `tests/lua/test_http_tls.lua` does for the missing-cainfo case.

## Tests — `tests/lua/test_http_tls.lua` (9 cases, all backends
covered where relevant)

Pinned to the Lua backend via `__FAN_HTTP_BACKEND_DEFAULT = "lua"`.
Uses `openssl req -x509` from the CI image to generate a
self-signed cert (which doubles as its own CA for pinning) and
an unrelated cert.  Suite self-skips when the `openssl` CLI is
absent.

1. `fan.tls compiled in (guard)` — refuses to run when the
   build lacks OpenSSL.
2. `cainfo=<correct CA>` — HTTPS to a self-signed origin with
   `verify=true` succeeds when the cert-as-CA is pinned via
   `cainfo`.
3. `cainfo=<wrong CA>` — verify fails with a TLS/verify-specific
   error message; asserts the message is NOT the bare
   `"connect: connection error"`.
4. `cainfo=<missing file>` — `pcall`-wraps the request; asserts
   the raised error names `load_verify_locations`, the bogus
   path, or `system lib`.
5. `verify=false` — trust store bypass regression check.
6. `M.cainfo(path)` module setter is consumed by the Lua path.
7. Per-request `opts.cainfo` overrides the module setter.
8. Closed port `127.0.0.1:1` surfaces `socket error:
   Connection refused` (or another specific errno), never
   `"connection error"`.
9. Bad DNS (`*.invalid` RFC 6761 reserved TLD) surfaces a
   `"dns error:"` prefix or another specific reason.

## Verification matrix (arm1, `luafan2-ci:local`)

| Mode                     | suites  | test\_http\_tls.lua | ASan     | Leaks |
|--------------------------|---------|--------------------|----------|-------|
| normal                   | 51 / 51 | 9 / 9              | n/a      | n/a   |
| --asan                   | 51 / 51 | 9 / 9              | 0 error  | 0     |
| --coverage --enforce     | 51 / 51 | 9 / 9              | n/a      | n/a   |

Coverage: **C 85.5 %** (target 85 %), **Lua 90.58 %** (target 90 %).
Previous milestone M20.2 baseline was C 85.5 %, Lua 90.39 %; M21
lifted Lua coverage by 0.19 pp with the new TLS tests.
