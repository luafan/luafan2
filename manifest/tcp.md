# fan.tcp — TCP client + server (M2 + M17 + M21)

`fan.tcp` exposes both a coroutine-yielding API (M2) and a callback-based
async API (M17).  The callback API restores the v1 `fan.tcpd` contract
downstream code depends on for CDP, WebSocket accumulation, and mobile
rebind lifecycles.

Both APIs share the same libevent bufferevent machinery, so they can be
mixed freely inside one process.

## Two entry points

| Style         | Client                           | Server                       |
|---------------|----------------------------------|------------------------------|
| Coroutine-yielding | `fan.tcp.connect(host, port[, opts])` | `fan.tcp.bind(host, port, fn[, opts])` |
| Callback-based (M17) | `fan.tcp.connect_async{...}`  | `fan.tcp.bind_async{...}`   |

`require("fan.tcpd").connect / .bind` are aliases for the async variants
(v1 API parity).

## Callback API — `fan.tcp.connect_async{...}`

Returns the connection handle **immediately**; the handshake completes
asynchronously and the caller is notified through callbacks.  Writes
issued before onconnected fires are buffered by libevent and flushed
once the socket becomes writable (pre-connect send queue).

### Options

| Field                   | Type   | Notes |
|-------------------------|--------|-------|
| `host`                  | string | required |
| `port`                  | int    | required, 1..65535 |
| `ssl`                   | bool   | enable TLS |
| `ssl_host`              | string | SNI + hostname verification override; defaults to `host` |
| `ssl_verifypeer`        | int    | 1 = verify (default), 0 = accept any peer |
| `ssl_verifyhost`        | int    | 1 = verify hostname, 0 = ignore |
| `cainfo`                | string | PEM CA bundle file; NULL = OpenSSL defaults |
| `capath`                | string | hashed CA dir; NULL = OpenSSL defaults |
| `pkcs12`                | table  | `{ path = ..., password = ... }` client cert |
| `connect_timeout`       | number | seconds; 0 = no timeout |
| `read_timeout`          | number | seconds; 0 = no timeout |
| `write_timeout`         | number | seconds; 0 = no timeout |
| `evdns`                 | userdata | custom `fan.evdns` resolver |
| `onconnected`           | fn     | `function(self)` |
| `onread`                | fn     | `function(self, data)` |
| `onsendready`           | fn     | `function(self)` when output drains |
| `ondisconnected`        | fn     | `function(self, reason)` — exactly-once |

### Callback dispatch contract

* All callbacks receive `self` (the conn userdata) as the first argument.
  The v1 `callback_self_first = false` option is **not** supported (v2
  callbacks are always `self`-first); passing it raises `luaL_error`.
* `ondisconnected(self, reason)` fires **exactly once** per conn — even
  if peer EOF and local `close()` race, the second one is a no-op.
  Reasons: `"eof"`, `"timeout"`, `"connect_timeout"`, `"error"`,
  `"closed"` (local close), or a build-failure message.
* `onconnected` never fires after `close()` — the state machine
  guarantees this even when close races with the CONNECTED event.
* `connect_timeout` uses an independent one-shot timer (bufferevent's
  read/write timeouts do not cover the SYN state).  On expiry the bev
  is torn down and `ondisconnected(self, "connect_timeout")` fires.

### Handle methods

* `conn:send(data)` — buffers into the bufferevent output queue; safe
  before onconnected (libevent flushes on writable).  Returns `true`
  or `(nil, err)`.
* `conn:close()` — synchronous.  Fires `ondisconnected(self, "closed")`
  (unless another reason already fired).
* `conn:reconnect()` — tears the bev down and rebuilds it against the
  original host/port + TLS/evdns options.  `dispatched_disc` reset so
  the next ondisconnected can fire again.
* `conn:pause_read()` / `conn:resume_read()` — backpressure.
* `conn:shutdown()` — half-close (`shutdown(fd, SHUT_WR)`).
* `conn:getpeername()` / `conn:getsockname()` — returns `(ip, port)` or
  `(nil, err)`.

### v1 fields intentionally rejected

* `worker = N` — v2 has no in-process worker event bases (fan.worker is
  a multi-process model).  Passing `worker` raises `luaL_error` so
  legacy callers cannot silently ship broken affinity assumptions.
* `callback_self_first` — see above.

## Callback API — `fan.tcp.bind_async{...}`

Returns a listener handle.  Each accepted connection produces a fresh
`accept` userdata handed to the user's `onaccept` callback; the user
must synchronously call `accept:bind{...}` to install the accept-side
callbacks (matches v1 fan.tcpd).

### Options

| Field                   | Type     | Notes |
|-------------------------|----------|-------|
| `host`                  | string   | numeric IPv4 or `"localhost"`; default `"0.0.0.0"` |
| `port`                  | int      | 0..65535; 0 = kernel-picked |
| `ssl`                   | bool     | enable TLS |
| `cert` / `key`          | strings  | required when `ssl = true` |
| `send_buffer_size`      | int      | SO_SNDBUF on each accept fd |
| `receive_buffer_size`   | int      | SO_RCVBUF on each accept fd |
| `onaccept`              | fn       | `function(self, accept)` — required |
| `onsslhostname`         | fn       | `function(hostname)` — reserved, see below |

### Accept object

The `accept` handed to `onaccept` is idle until the user installs
callbacks via:

```lua
accept:bind{
    onread         = function(self, data)   ... end,
    onsendready    = function(self)         ... end,
    ondisconnected = function(self, reason) ... end,
}
```

`bufferevent_enable(EV_READ)` fires **inside** `accept:bind`, so any
data that arrives before then stays in the kernel receive queue.
Callers that never bind end up with an idle bev the GC collects.

Methods:

* `accept:send(data)` — write.
* `accept:close()` — synchronous close; fires `ondisconnected(self,
  "closed")` exactly once.
* `accept:flush()` — API parity with v1 fan.tcpd.  Socket bufferevents
  are already write-through so this is effectively a no-op that
  returns `true`.
* `accept:remoteinfo()` — returns `{ ip = ..., port = ... }` or
  `(nil, err)`.
* `accept:pause_read()` / `accept:resume_read()` — backpressure.

### Server object methods

* `server:close()` — releases listener, TLS ctx, all Lua refs, and
  strdup'd rebind params.  Idempotent.
* `server:rebind()` — rebuilds the listener on the same host/port.
  Useful for mobile / VPN transitions.  Returns `true` or `(nil, err)`.
* `server:getport()` — actual bound port (useful when `port = 0` was
  passed).  Returns int or `(nil, err)`.

### Deferred features

**`onsslhostname` (SNI dispatch to Lua)** — the field is parsed and its
Lua ref is pinned so a future patch can install
`SSL_CTX_set_tlsext_servername_callback` in `tls.c` without changing
`bind_async`'s signature.  Today the callback is never invoked.
Downstream code that requires SNI-driven cert selection should watch
for a follow-up patch.

## v1 `fan.tcpd` compatibility

`lua/fan/tcpd.lua` maps the v1 module names to the async entries:

```lua
local tcpd = require("fan.tcpd")
tcpd.connect{...}    -- fan.tcp.connect_async
tcpd.bind{...}       -- fan.tcp.bind_async
```

CDP / Remote Hands / other v1 downstreams can drop their private
`tcpd_compat.lua` shim and switch their `require` to `fan.tcpd`.

## Implementation notes

### SSL_CTX cache (client side)

`fan.tls_client_bev_ex` looks up an SSL_CTX in a 64-slot open-address
hash table keyed on a SHA-256 fingerprint of
`(cainfo, capath, pkcs12_path, pkcs12_password)` with `0x1E` (ASCII RS)
as the field separator.  Two connections with identical TLS parameters
share a single ctx; total ctx count is bounded by the parameter-set
count (typically 1–5 in real deployments), NOT by the connection
count.  This is the memory fix over a naive per-conn `SSL_CTX_new`
which would allocate tens of KB of parsed certs per connection.

Cache entries live for the process; freeing them while a derived SSL
is still in flight would UAF.  If the cache saturates (unlikely at
64 slots) new ctxs are built without caching — safe but loses the
reuse benefit.

Callers with **no** TLS options (`cainfo`, `capath`, `pkcs12` all
NULL) short-circuit to a process-wide `g_client_ctx` singleton with
default verify paths, identical to the pre-M17 fast path.

### PKCS#12 client certificates

`fan_tls_load_pkcs12` (ported from v1 `tcpd_ssl.c`) parses a PKCS#12
bundle with `d2i_PKCS12_fp` + `PKCS12_parse`, then installs the cert,
private key, and any CA chain into the SSL_CTX.  This is what makes
APNs-style client authentication work.

### Exactly-once ondisconnected

Both client and server sides use a `dispatched_disc` flag on the conn
struct.  Every teardown path (`close()`, peer EOF, socket error,
read/write timeout, connect_timeout, GC) routes through a helper that
checks the flag first.  The self-ref pin is released **after** the
callback dispatch so the callback always sees a live `self`.

## M21 additions

`fan.tcp.connect(host, port, opts)` gained three fields that were
previously only reachable via `connect_async{...}`:

| opts key   | Type   | Effect                                         |
|-----------|--------|-------------------------------------------------|
| `ssl_host` | string | Override SNI + hostname verification identity  |
| `cainfo`   | path   | Custom CA bundle (PEM) for peer verification   |
| `capath`   | path   | Custom hashed CA directory                      |

When any of the three is set the code routes through
`fan_tls_client_bev_ex` (which fingerprint-caches an `SSL_CTX` per
distinct parameter set).  When none is set the legacy
`fan_tls_client_bev` path is preserved byte-for-byte and reuses
the process-wide `g_client_ctx` singleton.

`conn_eventcb` now surfaces a specific error reason when
`BEV_EVENT_ERROR` fires:

1. DNS: `"dns error: <evutil_gai_strerror>"`
2. TLS: `"tls error: <ERR_error_string>"`
3. TLS verify: `"tls verify failed: <X509_verify_cert_error_string>"`
4. Socket: `"socket error: <evutil_socket_error_to_string>"`
5. Fallback: `"connection error"` (only when no source yielded info)

The pre-M21 `"connection error"` opaque literal is gone from the
common path.  See [M21 TLS diagnostics](m21-tls-diagnostics.md)
for the full story.

## Tests

Contract-level assertions live in `tests/lua/test_tcp_async.lua`
(41 cases) and `tests/lua/test_tcp.lua` (11 cases).  M21 TLS
pinning + diagnostics are covered by
`tests/lua/test_http_tls.lua` (9 cases; three of them exercise the
tcp.c error surface directly via bad host / closed port / bad
DNS).  All three modes green: normal / --asan / --coverage; C
line coverage **85.5%**, Lua **90.58%**.
