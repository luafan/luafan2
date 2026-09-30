# M20.1 — Pure Lua HTTP streaming callbacks

`fan.http_lua.request` now accepts optional `onheader` and `onreceive` callbacks.

- `onheader(response)` runs once after the status line and complete lower-cased response headers are parsed; the table contains `status`, `responseCode`, `reason`, and `headers`.
- `onreceive(body_chunk)` runs for each decoded body segment consumed by `read_n` or `read_until_eof`; chunked transfer framing is not exposed to the callback.
- Buffered compatibility is retained: `response.body` is still the complete aggregate when the request succeeds.
- Returning `false` or raising from either callback closes the connection and returns `nil, error` with an explicit callback/cancellation message.
- Invalid callback values are rejected before body processing.

M20.2 (libcurl C backend write/header callbacks, streaming no-body accumulation, and its cancellation/error cleanup) is not implemented by this milestone.

## Tests

`tests/lua/test_http.lua` covers decoded chunked streaming, one-shot headers, buffered-body compatibility, callback cancellation, callback exceptions, and header cancellation. The full normal/ASan/coverage matrix remains pending until M20.2 and the expanded HTTP mock matrix are complete.
