# fan.udp — UDP socket (M3 + M18)

`fan.udp` exposes both a coroutine-yielding API (M3) and a
callback-based async API (M18).  M18 restores the v1 `fan.udpd`
contract: immediate handle, push-style `onread` per datagram with
peer info, `sock:rebind()` for mobile network hops, and destination
objects (`UDP_AddrInfo`) with hostname resolution helpers.

## Two entry points

| Style              | Constructor                          |
|--------------------|--------------------------------------|
| Coroutine-yielding | `fan.udp.new(bind_host, bind_port)` — `sock:recv()` yields |
| Callback-based (M18) | `fan.udp.new_async{...}` — `onread(self, data, dest)` |

`require("fan.udpd").new / .make_dest / .make_dests` are aliases for
the async entries (v1 API parity).

## `fan.udp.new_async{...}`

Returns the socket handle **immediately**; the read loop dispatches
each received datagram to `onread(self, data, dest)` on a fresh
coroutine.

### Options

| Field                   | Type   | Notes |
|-------------------------|--------|-------|
| `bind_host`             | string | numeric IPv4 / IPv6; default `"0.0.0.0"` |
| `bind_port`             | int    | 0..65535; 0 = kernel-picked; omit to skip bind |
| `host`                  | string | default destination (numeric IP) for `sock:send()` without a dest |
| `port`                  | int    | default destination port |
| `onread`                | fn     | `function(self, data, dest)` |
| `onsendready`           | fn     | `function(self)`; fires after `sock:send_req()` |

### Callback contract

* All callbacks receive `self` (the socket userdata) as the first
  argument.  The v1 `callback_self_first = false` option is **not**
  supported; passing it raises `luaL_error`.
* `onread(self, data, dest)` fires once per received datagram.  The
  `data` is the raw datagram body; `dest` is a fresh `UDP_AddrInfo`
  userdata for the sending peer (see below).  Datagrams received while
  no `onread` is set are drained silently (libevent stays edge-quiet).
* `onsendready(self)` is armed only by an explicit `sock:send_req()`
  and fires **once** per arm.  UDP sockets are essentially always
  writable, so the callback typically fires on the next loop tick.
  This is v1 parity for code that wants to pace outbound traffic.
* The `worker` field is rejected (v2 has no in-process worker event
  bases — see the M17 `fan.tcp` documentation for the reasoning).

### Handle methods

* `sock:send(data[, dest])` — send a datagram.  `dest` is a
  `UDP_AddrInfo` from `make_dest`; when omitted, the socket falls
  back to the `host`/`port` passed at construction.  Returns
  `true` or `(nil, err)`.
* `sock:send_req()` — arm the EV_WRITE one-shot; `onsendready` fires
  when the socket is writable.
* `sock:getPort()` / `sock:getport()` — local bound port (useful
  when `bind_port = 0` lets the kernel pick).  Both names are
  provided; v1 uses mixed case.
* `sock:rebind()` — tears the socket + read watcher down and rebuilds
  them on the original `bind_host` / `bind_port`.  Use case: mobile
  network transition, VPN reconnect.
* `sock:close()` — idempotent.  Releases the fd, both watchers, and
  the self-pin.  Subsequent `send` / `send_req` / `rebind` return
  `(nil, err)`.

## `fan.udp.make_dest(host, port[, evdns])` → `UDP_AddrInfo`

Builds a destination object from a host + port.  Uses
`getaddrinfo(3)` synchronously.  Accepts both numeric IPv4 / IPv6 and
hostnames (though the resolution is blocking — see notes below).

### `fan.udp.make_dests(host, port[, evdns])` → array

Same as `make_dest` but returns **every** address `getaddrinfo`
resolves.  Useful for round-robin / failover load balancing across
multiple A records.

### `UDP_AddrInfo` methods

* `dest:getHost()` — the string form (IP address or resolved
  hostname's IP).
* `dest:getIP()` — same as `getHost()` for numeric IPs.  Provided
  for clarity in code that specifically needs the IP form.
* `dest:getPort()` — integer.

### DNS resolution notes

The `evdns` third argument is accepted for API compatibility with
v1 (which used `libevent`'s async `evdns_getaddrinfo` under a
coroutine yield) but **currently ignored**.  v2 resolves through
`getaddrinfo(3)` synchronously.  For most downstream code this is
transparent:

* Numeric IPs return immediately (getaddrinfo takes the fast path).
* Hostnames block the loop for the duration of the DNS lookup.
  Downstream code that repeatedly resolves hostnames from a hot loop
  should hoist the resolution once at startup, or use `fan.dns.resolve`
  (async) up front and pass the numeric IP.

Adding async DNS to `make_dest` is a self-contained follow-up (the
v1 flow lived in `src/udpd_dns.c` and used `evdns_getaddrinfo` with a
coroutine yield/resume dance).  Signature stays the same; only the
resolution path inside changes.

## v1 `fan.udpd` compatibility

`lua/fan/udpd.lua` maps the v1 module names to the async entries:

```lua
local udpd = require("fan.udpd")
local dest = udpd.make_dest(host, port)
local sock = udpd.new{
    bind_port = 8080,
    onread    = function(self, data, dest) ... end,
}
sock:send("payload", dest)
```

The three v2 divergences are documented inline:

* `worker` is rejected (see fan.tcp).
* `callback_self_first` is rejected (callbacks always self-first).
* `evdns` on `make_dest` / `make_dests` is silently ignored (sync
  `getaddrinfo` fallback; see DNS notes above).

## Tests

`tests/lua/test_udp_async.lua` (15 cases) — all three modes green
(normal / --asan / --coverage; C 85.4%, Lua 90.43%).  Covers happy
path (self+data+dest), pre-connect send queue equivalent (send with
default host:port), send_req+onsendready, getPort, rebind, close
idempotence, three rejection paths, silent-drain when onread is
unset, and the fan.udpd shim.
