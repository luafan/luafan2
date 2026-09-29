# webase (luafan2 port)

Application framework ported from [webase](https://github.com/luafan/webase)
to run on **luafan2**. Sources are drop-in for v1 webase with three
mechanical dependency swaps handled at the C layer (see below).

Not ported (deliberate): `curl-impersonate` — luafan2 does not embed
libcurl-impersonate and has no plans to. Everything else — file-based
handlers, static-file cache, gzip / ETag, JSONP, WebSocket, LRU cache,
weakify, AES-GCM (via `fan.crypto.gcm`), dynamic route registration,
service lifecycle — is available.

## Quick start

```
cd webase
WORKDIR=./ ../build/fan core.lua
```

Then, in a project that mounts its own handlers, mapping files, and web
root next to `core.lua`:

```
WORKDIR=/path/to/project/ /path/to/luafan2/build/fan /path/to/luafan2/webase/core.lua
```

WORKDIR is a *prefix* (v1 convention): it's concatenated with directory
names, so it typically ends with a `/`. If you omit `WORKDIR`, webase
looks in the current working directory.

## Environment variables

| Var                | Default    | Purpose                             |
|--------------------|------------|-------------------------------------|
| `SERVICE_HOST`     | `0.0.0.0`  | Bind interface                      |
| `SERVICE_PORT`     | `2201`     | Bind port                           |
| `WEBROOT`          | `web`      | Static file root (WORKDIR-relative) |
| `SERVICE_WORKERS`  | `0`        | Reserved; see "Worker pool" below   |
| `PURGE_TOKEN`      | (unset)    | Optional shared secret for the      |
|                    |            | `/purge_cache` handler              |
| `DEBUG`            | (unset)    | `"true"` -> `config.debug=true`     |

Config values live under `config.d/*.lua`; the two shipped files
(`service.lua`, `core.lua`) map environment variables to the config
table returned by `require "config"`.

## Directory layout

```
webase/
  core.lua          # entry: init services, bind httpd, run event loop
  route.lua         # exact + pattern route table; onXxx handler mapping
  service.lua       # background service loader (onStart/onStop/getStatus)
  webfile.lua      # static file serving with LRU + ETag + gzip
  mapping.lua       # URL rewrites (mapping/*.lua sandboxed)
  mimetypes.lua     # parses mime.types into a { ext = content-type } map
  lru.lua           # lua-lru cache (unchanged from webase v1, MIT)
  weakify.lua       # weak-valued proxy (unchanged from webase v1)
  ctxpool.lua       # stub: raises an error (see "MariaDB" below)
  mime.types        # nginx-style mime type table
  config.d/         # environment -> config bindings
```

A project directory layered on top of webase typically looks like:

```
project/
  handle/           # request handlers, one file per route
    hello.lua       # onGet(req, resp) return {...}  ->  GET /hello
    api/status.lua  # nested paths preserve the dir prefix
  service/          # background services
    worker.lua      # onStart / onStop / getStatus
  mapping/          # optional URL rewrites (sandboxed .lua files)
  web/              # static files served by webfile
```

## API surface (unchanged from v1)

* **Route handlers** — Files under `handle/` become HTTP routes. Path
  is derived from the file's relative location; `route = "/foo"` or
  `pattern = "^/api/v2/"` overrides. Every top-level `onGet`, `onPost`,
  `onDelete`, `onWebSocket`, ... becomes a per-method dispatch entry.
  Table returns are JSON-encoded (with optional `?jsonp=cb`, validated
  against `^[A-Za-z_$][A-Za-z0-9_$.]*$` up to 128 chars); string returns
  starting with `<` become `text/html`, other strings `text/plain`.

* **Dynamic routes** — Add or remove routes at runtime from any Lua code:
  ```lua
  local route = require "route"
  route.add({ route = "/x", onGet = function(req, resp) ... end })
  route.remove("/x")
  ```
  Priority: static exact > static pattern > dynamic exact > dynamic
  pattern.

* **Services** — Files under `service/` are loaded at startup. Each
  defines `onStart`, `onStop`, `getStatus`. Aliases `start` / `stop` /
  `status` (lowercased with the `on` prefix stripped) are auto-created.
  `service.list()` returns every registered service; `service.get(name)`
  fetches one; the returned table is also indexed as a callable that
  dispatches methods by key:
  ```lua
  local svc = require "service"
  svc.status("worker")   -- calls worker.getStatus()
  svc.stop()             -- calls onStop on every service
  ```

* **Static files** (`webfile.lua`) — Content-Type from `mime.types`,
  `Cache-Control: max-age=86400`, `ETag` from MD5(body), 304 on
  `If-None-Match` match, gzip when the client sends `Accept-Encoding:
  gzip`, `index.html` served automatically for directory requests,
  otherwise an HTML listing. Path traversal (%2e%2e, %00, ..%2f) is
  rejected with 400 before the file cache is consulted.

* **`req.remote_addr` / `req.remoteip`** — Peer IP (see the luafan2
  patch to `httpd_lua`). Falls back to nil on a half-closed socket;
  handlers typically OR with `req.headers["X-Real-IP"]`.

## What changed vs. v1 webase

Three lower-level swaps that keep the source above nearly identical:

| v1 dependency               | luafan2 replacement            |
|-----------------------------|--------------------------------|
| `lfs.dir` / `lfs.attributes`| `fan.posix.readdir` + `fan.posix.stat` |
| `require "md5"`             | `fan.crypto.md5_binary` (built-in) |
| `zlib.compress(b,_,_,31)`   | `fan.zlib.gzip_compress`       |

One handler API addition: `req.remote_addr` (also aliased as
`req.remoteip` for v1 parity) is populated from `conn:getpeername()`.

The `curl-impersonate` module is intentionally absent; drop-in
replacement candidates live at
[`fan.http`](../lua/fan/http.lua).

## MariaDB / ctxpool

v1 shipped a small `ctxpool` that scanned `database/*.lua` (each file
returning an ORM schema table) and passed the result to
`mariadb.pool.new(list)`. luafan2's `fan.mariadb.pool.new{...}` takes
a **connection options** table, and its ORM is a separate module
(`fan.orm`), so v1's fused constructor no longer fits.

Rather than ship a shim that would silently drop half the schema, we
kept `webase/ctxpool.lua` as a hard error with migration notes. Apps
that need a MariaDB pool should call
`fan.mariadb.pool.new{...}` directly from their service or handler
modules, and layer `fan.orm` on top when needed:

```lua
local pool = require("fan.mariadb.pool").new{
  host = config.maria_host,
  port = config.maria_port,
  user = config.maria_user,
  password = config.maria_passwd,
  database = config.maria_database,
  charset = config.maria_charset,
  max_size = config.maria_pool_size,
}
pool:with(function(db)
  local rows = db:query("select 1")
  ...
end)
```

## AES-GCM

Available as `fan.crypto.gcm`:

```lua
local gcm = require("fan").crypto.gcm
local ct, tag = gcm.encrypt(key, nonce, plaintext, aad)
local pt      = gcm.decrypt(key, nonce, ct, tag, aad)   -- nil on mismatch
```

Same contract as v1's external `gcm.so` (`gcm/luagcm.c` in webase): 16 or
32-byte keys (AES-128 / AES-256), 12-byte nonce, 16-byte auth tag,
optional AAD.

## Tests

Integration tests live in `luafan2/tests/webase/` and drive `fan
core.lua` against the fixtures in `tests/webase/fixtures/`; the runner
launches the server on `127.0.0.1:<random port>` and exercises static
files, dynamic routes, JSONP validation, path traversal, HEAD, gzip,
ETag/304, and WebSocket echo.
