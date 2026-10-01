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

## Deliberate differences from the retired webase image

The release image intentionally does **not** carry legacy image-only
extras that the downstream application does not use:

* **jQuery Mobile / demo assets** — the image ships only the small
  architecture landing page at `/web/index.html`; production users mount
  their own `WEBROOT` and assets.
* **curl-impersonate / curlimp.so** — not needed by the luafan2 HTTP
  clients; use the native `fan.http` C backend or the pure-Lua backend.
* **standalone `gcm.so`** — not needed; AES-GCM is provided by the native
  `fan.crypto.gcm` API.
* `ctxpool` — API shape diverged from luafan2's `fan.mariadb.pool` +
  `fan.orm`; applications using the retired helper must migrate to those
  native APIs.

These are intentional scope decisions, not missing release-image files.
The standard Web service contract remains available: `/root/core.lua`,
`handle/`, `service/`, `mapping/`, `web/`, `mime.types`, route dispatch,
static files, gzip, ETag, and the default port 2201 service.
