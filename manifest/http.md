# fan.http — client shim (M4 + M13.C + M16.4 + M20 + M21)

`require("fan.http")` returns the HTTP/1.1 client shim used by both
webase applications and standalone tools.  It exposes verb helpers
(`get` / `post` / `put` / `patch` / `delete` / `head`) plus a table-form
`request{}` and picks one of two backends:

* **C backend** — libcurl-based; single-hop from C, redirect loop lives
  in the shim.  Enabled when the process links `libcurl`.
* **Lua backend** — pure-Lua HTTP/1.1 over `fan.tcp`; chunked reader
  + redirect handling in `fan/http_lua.lua`.  Always available; used
  when the C backend is absent, when the caller pins with
  `_G.__FAN_HTTP_BACKEND_DEFAULT = "lua"`, or when `opts.backend =
  "lua"` is passed per call.

Both backends return the **same response shape** so the shim is
transparent to callers.

## Response shape

```
{ status         = 200,          -- numeric HTTP status
  responseCode   = 200,          -- v1-compatible alias (M16.4)
  reason         = "OK",         -- reason phrase
  headers        = { ["content-type"] = "..." },  -- keys lowercased
  body           = "..."         -- raw response body (bytes)
}
```

The `responseCode` field is an alias for `status` — v1 code frequently
reads `resp.responseCode` and rejecting that would force every ported
handler to patch its status checks.  Both backends set both fields on
every response; keeping them in sync is part of the contract.

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

## Streaming callbacks (M20)

Both backends accept optional `onheader` and `onreceive` callbacks
plus a `buffered` toggle.  See
[M20 HTTP streaming callbacks](m20-http-streaming.md) for the full
contract, but the summary is:

* `onheader(response)` runs once when the status line + headers are
  fully parsed, before any body byte is delivered.
* `onreceive(chunk)` runs for each decoded body slice; chunked
  framing is not exposed.
* `buffered = true` (default even when `onreceive` is set) keeps
  `response.body` as the complete aggregate.  `buffered = false`
  skips accumulation for streaming-only consumers (SSE / large
  downloads / long-poll).
* Returning `false` (or raising) from either callback cancels the
  in-flight request and produces `nil, err`.

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
  type validation).
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
