# M20 — HTTP streaming callbacks

Both `fan.http` backends (Pure Lua and libcurl/C) accept optional
`onheader` and `onreceive` callbacks, restoring the v1 streaming
semantics that SSE, download progress, and long-poll clients relied
on.

## Shared contract

- `onheader(response)` fires **once** after the status line and the
  complete lower-cased response headers are parsed.  The table
  contains `status`, `responseCode`, `reason`, and `headers`.
- `onreceive(chunk)` fires for **each decoded body segment**.  Chunked
  transfer framing is not exposed to the callback; the callback sees
  the same bytes that would land in `response.body`.
- **Buffered mode is the default even when `onreceive` is set.**
  `response.body` remains the complete aggregate so existing callers
  keep working while a streaming consumer is attached.
- Pass `opts.buffered = false` to **skip accumulation** — the request
  still runs to completion, but `response.body` is `""` and the
  callback is the only sink.  This is the v1 "streaming-only"
  behaviour.
- Returning **exactly `false`** from `onheader` or `onreceive` (or
  raising a Lua error) closes the connection and returns
  `nil, error` with an explicit callback/cancellation message.
- Invalid callback values (non-function, non-nil) are rejected before
  the request starts.

The two backends share the same option surface and produce the same
response shape, so switching between them is transparent to callers.

## M20.1 — Pure Lua backend (`lua/fan/http_lua.lua`)

- `Reader.onreceive` wired into `read_n` / `read_until_eof` /
  `read_chunked` so every decoded segment reaches the callback.
- `onheader` dispatched immediately after `read_headers` succeeds,
  before any body byte is consumed.
- Callback errors are surfaced through the existing `pcall` path;
  the socket is closed on error/cancellation.
- Buffered mode is implemented by keeping the existing
  concatenation path and adding a `buffered = false` short-circuit
  that discards accumulated bytes.

## M20.2 — libcurl / C backend (`src/net/http.c`)

- `CURLOPT_WRITEFUNCTION` (`cb_write`) grows a streaming branch:
  when `onreceive` is set it `lua_pcall`s the callback on
  `g_main_L` with each libcurl body slice.  Accumulation happens
  only when `buffered ~= false`.
- `CURLOPT_HEADERFUNCTION` (`cb_header`, new) folds every header
  line into a lower-cased comma-joined map.  When it observes the
  header/body separator (`"\r\n"` or `"\n"`) it latches
  `header_dispatched` and fires `onheader` exactly once.  1xx
  intermediate responses that carry their own header block will
  cause a duplicate dispatch — that mirrors v1 and is documented
  as a known quirk.
- Callback lifetimes use `luaL_ref` on `g_main_L`.  Refs are
  released in `check_multi_info` on the completion path and in
  `l_req_gc` as a defensive backstop.
- Cancellation: a callback returning `false` or raising records
  `r->cancel_err` and returns `0`, which libcurl surfaces as
  `CURLE_WRITE_ERROR`.  `check_multi_info` prefers `cancel_err`
  over the raw libcurl message when composing the failure return.
- `push_reason` / `push_headers_table` / `record_cancel` helpers
  are factored out so `cb_header` and `check_multi_info` share the
  same response-shape construction.

The compat shim `lua/fan/http.lua` forwards `onreceive`, `onheader`,
and `buffered` from the front door down to whichever backend is
selected.

## Tests

`tests/lua/test_http.lua` — Pure Lua backend (M20.1 + M20.2 lua
mode): decoded chunked streaming, one-shot headers, buffered-body
compatibility, callback cancellation, callback exceptions, header
cancellation, and `buffered = false` skipping the aggregate.

`tests/lua/test_http_c.lua` — C/curl backend (M20.2): onheader
once, onreceive streaming with buffered aggregate preserved,
`buffered = false` skipping accumulation, `onreceive` returning
`false` aborts, `onreceive` raising propagates cleanly, `onheader`
returning `false` aborts before the body, and non-function callback
values rejected at request-build time.

## Verification matrix

Verified on arm1 in the pinned `luafan2-ci:local` container.

| Mode     | suites  | HTTP suites detail                        | ASan | Leaks |
|----------|---------|-------------------------------------------|------|-------|
| normal   | 50 / 50 | test\_http.lua 27/27, test\_http\_c.lua 20/20 | n/a  | n/a   |
| --asan   | 50 / 50 | same                                       | 0    | 0     |
| --coverage (--enforce) | 50 / 50 | same | n/a | n/a |

Coverage: C **85.5 %** (target 85 %), Lua **90.39 %** (target 90 %).
See `manifest/coverage-latest.md` for the current numbers.
