# webase (M12.2)

Application framework ported from webase v1
(https://github.com/luafan/webase). Lives under `webase/` at the luafan2
repo root; runs via `fan webase/core.lua` with WORKDIR set to a project
directory. Full contract in `webase/README.md`.

## What runs where

| Component        | Path                     | Notes                                  |
|------------------|--------------------------|----------------------------------------|
| Entry            | `webase/core.lua`        | Loads config → service → route → httpd |
| Route dispatch   | `webase/route.lua`       | Exact + pattern, static + dynamic      |
| Static files     | `webase/webfile.lua`     | LRU cache, ETag/304, gzip, HEAD        |
| Service loader   | `webase/service.lua`     | onStart/onStop/getStatus lifecycle     |
| URL rewrites     | `webase/mapping.lua`     | Sandboxed mapping/*.lua                |
| MIME types       | `webase/mimetypes.lua`   | Parses `webase/mime.types`             |
| LRU cache        | `webase/lru.lua`         | lua-lru (MIT), verbatim from v1        |
| Weak proxy       | `webase/weakify.lua`     | Verbatim from v1                       |
| MariaDB pool     | `webase/ctxpool.lua`     | Not ported — hard error with migration |

## Kernel dependencies added for the port

Enabled unconditionally in the luafan2 build (see manifest links):

| API                            | Purpose in webase                 |
|--------------------------------|-----------------------------------|
| `fan.posix.stat(path)`         | file/directory attributes         |
| `fan.posix.readdir(path)`      | already existed; scans handle/... |
| `fan.crypto.md5(data)`         | debug logging                     |
| `fan.crypto.md5_binary(data)`  | ETag body digest                  |
| `fan.crypto.gcm.encrypt/decrypt` | AES-GCM (v1 gcm.so replacement) |
| `fan.zlib.gzip_compress(data)` | Content-Encoding: gzip framing    |
| `req.remote_addr` / `req.remoteip` | httpd peer IP for auth checks |

## webfile gzip fallback (M16.4)

`webase/webfile.lua` serves static files with an on-disk-body → in-RAM
gzip cache.  Prior to M16.4, `get_file_gzip_body` unconditionally
dereferenced the compressor's return value with `#gbody`, which had
three failure modes if `fan.zlib.gzip_compress` ever returned nil:

* `#nil` raised, tearing down the request coroutine;
* the response body ended up empty on the recovery path;
* `Content-Encoding: gzip` was still announced, so a client that
  survived would try to inflate an empty stream.

M16.4 makes each stage of `get_file_gzip_body` fall back cleanly:

* cache miss + body read failure → returns nil early;
* compressor returns nil → returns nil (no cache write, no crash).

The `web()` dispatcher then keeps the identity body and **omits** the
`Content-Encoding` header entirely — a spec-compliant response.
`Content-Length` is computed off the served body in both branches so
framing stays consistent.

Covered by `tests/lua/test_webase_webfile.lua`: happy path (gzip
succeeds, header set), forced failure via monkey-patched
`fan.zlib.gzip_compress`, and the "no Accept-Encoding" identity path.

## Not ported (deliberate)

* `curl-impersonate` — not in scope for luafan2.
* `ctxpool` — API shape diverged from luafan2's `fan.mariadb.pool` +
  `fan.orm`; documented replacement in `webase/README.md`.
* v1 `web/` demo assets (jquery 1.x etc.) — the port ships no static
  demo tree; users mount their own WEBROOT.
