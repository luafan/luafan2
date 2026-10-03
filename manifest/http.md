# fan.http — client shim (M4 + M13.C + M16.4 + M20 + M21)

`require("fan.http")` returns the HTTP/1.1 client shim used by both
webase applications and standalone tools.  It exposes verb helpers
(`get` / `post` / `put` / `patch` / `delete` / `head`) plus a table-form
`request{}` and picks one of two backends:

* **C backend** — libcurl-based; single-hop from C, redirect loop lives
  in the shim. Enabled when the process links `libcurl`.
* **Lua backend** — pure-Lua HTTP/1.1 over `fan.tcp`; chunked reader,
  redirects, v1 repeated-header arrays, upload callbacks, completion
  callbacks, and response aliases live in `fan/http_lua.lua`. Always
  available; used when the C backend is absent, when the caller pins with
  `_G.__FAN_HTTP_BACKEND_DEFAULT = "lua"`, or when `opts.backend = "lua"`
  is passed per call.

Both backends return the same core response shape. C-only transport
parameters that require libcurl or native per-request socket/TLS controls
(such as custom DNS servers, proxy credentials/tunnel, client certificates,
`resolve`, and detailed transfer timings) remain unavailable on the
pure-Lua fallback and are not silently emulated.

## Response shape

The response protocol is implemented in the C HTTP backend. It preserves the
v1 field names and types while retaining v2 additions:

```
{ status          = 200,          -- v2 name
  responseCode    = 200,          -- v1 name
  reason          = "OK",         -- v2 reason phrase
  responseMessage = "OK",         -- v1 reason phrase
  headers         = {              -- lower-case keys, v1 value shape
    ["content-type"] = "...",    -- one value: string
    ["set-cookie"]   = {"a=1", "b=2"}, -- repeated: 1-based array
  },
  body            = "...",        -- raw response body (bytes)
  cookies         = {...},         -- libcurl COOKIEINFO lines, when enabled
  dns_time        = 0.001,         -- seconds, libcurl timing info
  connect_time    = 0.002,
  appconnect_time = 0.000,
  pretransfer_time= 0.002,
  starttransfer_time = 0.010,
  total_time      = 0.012,
  error           = "..."          -- present on completed transfer errors
}
```

The `headers` rule is part of the wire-compatible v1 contract: a repeated
header is never comma-folded into a string. Both the final response and the
C `onheader` callback use this same string-or-array shape. The compatibility
fields and timing/cookie fields are produced in C; the Lua shim does not
rewrite or synthesize response objects.

## Verb helper calling conventions (M16.4)

The verb helpers accept **either** shape:

```
http.get(url, opts)           -- positional
http.get{ url = url, ... }    -- single-table form, matches v1
```

Dispatch is by the first argument's type:

* `type(a) == "table"` → single-table form; the second argument is
  ignored.  `a` is passed straight to `M.request`.
* `type(a) == "string"` → positional; a shallow copy of `opts` (or a
  new table) receives `url = a` before dispatch.

`M.request{ url = ..., ... }` remains the low-level entry point that
both call.

## Redirect handling

`M.request` follows redirects by default (matches modern HTTP clients
— `fetch`, `axios`, `requests`).  Two overrides:

* **Per call**: `opts.follow_redirects = false` (or `true`) wins over
  the module default.  `opts.max_redirects = N` caps the loop
  (default 5).
* **Module-wide (M16.4)**:
  ```
  require("fan.http").set_default_follow_redirects(false)
  ```
  Flips the default for every subsequent call that does not pass
  `follow_redirects` explicitly.  Legacy v1 code that expects "return
  the first hop even if it's a 302" can drop this one line at boot
  and stop threading `follow_redirects = false` through every call
  site.  Bool-normalised (`not not bool`), reflected in
  `M._follow_redirects_default`.  The shim propagates the resolved
  value down into `http_lua.request` so the Lua backend honours the
  same knob (previously it re-defaulted internally).

303 (and 301/302 with a non-GET request) switch to GET and drop the
request body on redirect — same as v1 and the pure-Lua backend.

## v1 parameter and callback compatibility

The C backend accepts the v1 request parameters in addition to the v2
options: `verbose`, `dns_servers`, `onprogress`, `timeout`, `conntimeout`,
`ssl_verifypeer`, `ssl_verifyhost`, `sslcert`, `sslcertpasswd`,
`sslcerttype`, `sslkey`, `sslkeypasswd`, `sslkeytype`, `cainfo`, `capath`,
`proxy`, `proxyport`, `proxyuser`, `proxypassword`, `proxytunnel`, `onsend`,
`onbodylength`, `oncomplete`, `forbid_reuse`, and `resolve`.
The Lua file only forwards them. The legacy `worker` option is intentionally
out of scope in v2.

* `onprogress(dltotal, dlnow, ultotal, ulnow)` uses the v1 four-counter
  callback contract.
* `onsend(size)` returns a string chunk or `nil` to finish; `onbodylength` is
  called as `onbodylength(args)` with the v1 options table as self and its
  numeric result controls the upload length.
* `oncomplete(response)` receives both successful and error response tables
  and suppresses the normal return value; callback errors are logged without
  replacing the response protocol.
* `onheader(response)` and `onreceive(chunk)` retain the streaming behavior;
  callback errors return the v1 `{error=...}` response plus the v2 error
  string. `buffered=false` and HEAD preserve the v2 empty-string body rule.

## Streaming callbacks (M20)

Both backends accept optional `onheader` and `onreceive` callbacks
plus a `buffered` toggle. `onheader` runs before body delivery;
`onreceive` sees decoded body slices, and `buffered=false` skips
aggregation for streaming consumers.

## HTTP server request query contract (v1 compatibility)

For both HTTP server backends, request fields keep the v1 meanings:

* `req.query` is the **raw query string** from the request target,
  with its original parameter order, repeated keys, and percent
  encoding preserved.  For `/items?b=2&a=1&tag=x&tag=y`, it is
  `"b=2&a=1&tag=x&tag=y"`.
* `req.params` is the decoded parameter map used for convenient
  name-based access.  It contains query parameters merged with
  `application/x-www-form-urlencoded` body parameters; the last
  value wins on key collisions.  Values are strings.
* `req:query_values(name)` returns a new array containing every
  decoded value for that URL query key, in original wire order.
  It does not include form-body values.

`req.query` must not be changed to a table.  Code needing parsed
single values should use `req.params`; code needing exact ordering
or repeated-key fidelity should use the raw `req.query` string or
`req:query_values(name)`. No `query_map` field is added.

## CA bundle pinning + TLS diagnostics (M21)

Both backends accept `cainfo` (PEM bundle path) and `capath`
(hashed CA directory) per-request.  When absent, the shim falls
back to `require("fan.http").cainfo(path)` / `.capath(path)` /
the `M._cainfo` / `M._capath` module-scoped setters (v1-compatible
API surface).  When those are absent, the process-wide trust store
is used — either from `SSL_CERT_FILE` / `SSL_CERT_DIR` env vars
or from `SSL_CTX_set_default_verify_paths`.

TLS / DNS / socket failures now surface specific reasons instead
of the opaque pre-M21 `"connection error"`.  Full contract in
[M21 HTTPS CA pinning + TLS diagnostics](m21-tls-diagnostics.md).

## Tests

* `tests/lua/test_http.lua` — Lua backend end-to-end (pinned via
  `__FAN_HTTP_BACKEND_DEFAULT = "lua"`); covers verb-form dispatch,
  chunked reader, redirect loop, `responseCode` alias, the M16.4
  `set_default_follow_redirects` knob, and the M20 streaming
  callbacks (onheader / onreceive / buffered=false / cancellation).
* `tests/lua/test_http_c.lua` — C backend focused suite; covers the
  same response contract (including `responseCode`), TLS + libcurl
  specifics, and the M20 streaming callbacks on the libcurl
  bridge (onheader once, onreceive with buffered aggregate,
  buffered=false, cancellation via `false` / raised error, callback
  type validation). It also includes a 120-request concurrent
  socket-context stress test mixing delayed responses, callback
  cancellation, repeated socket rearming, and completion cleanup.
* `tests/c/unit/test_http_clear_lua_state.c` — teardown regression:
  a pending C-backend request with `onreceive` completes after
  `fan_http_clear_lua_state()` and must release C/CURL state without
  touching the cleared Lua registry or coroutine. Runs under ASan.
* `tests/lua/test_http_tls.lua` (M21) — pure-Lua HTTPS + CA
  pinning contract: correct-CA success, wrong-CA specific verify
  error, missing-file CTX diagnostic, `verify=false` bypass,
  `M.cainfo(path)` module setter, per-request override, closed
  port socket errno, bad DNS surface — 9 cases.

The teardown regression test also exposed and guards the shared
Lua-state lifecycle rule used by other event callbacks: after the clear
hook, completion paths must do native cleanup without using Lua. The
cross-module C regression coverage now includes pending HTTP, DNS, UDP,
TCP receive, FIFO receive, HTTPD handler, and MariaDB async operations
after `fan_clear_lua_states()`; the arm64 ASan run covers all of these
paths. WebSocket handshake, frame, recv, and close behavior remains
covered by `tests/lua/test_websocket.lua` (24 cases), including all
server-side ownership paths.

Both suites run in-process against a `fan.tcp.bind` origin server
built at the top of the file.
