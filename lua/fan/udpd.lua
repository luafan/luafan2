-- fan/udpd.lua — v1 fan.udpd compatibility shim (M18).
--
-- Restores `require("fan.udpd")` so legacy code that reads
--
--     local udpd = require("fan.udpd")
--     local dest = udpd.make_dest(host, port)
--     local sock = udpd.new{ bind_port = P, onread = fn, ... }
--     sock:send(data, dest)
--
-- keeps working.  Both `.new` and `.make_dest` / `.make_dests` resolve to
-- the native async entries added in M18.
--
-- Two v1 fields are intentionally rejected by fan.udp.new_async (see
-- src/net/udp.c):
--
--   * `worker` — v2 has no in-process worker event bases; fan.worker
--     is a multi-process model.
--
--   * `callback_self_first` — callbacks always receive `self` as the
--     first argument in v2; the legacy no-self signature is not offered.
--
-- Everything else the v1 API took (host, port, bind_host, bind_port,
-- onread, onsendready) is forwarded verbatim.  The `evdns` third
-- argument on make_dest / make_dests is accepted (for API parity) but
-- currently ignored: v2 uses libc getaddrinfo synchronously; callers
-- that need async DNS should resolve up front via `fan.dns.resolve`
-- and pass the numeric IP.

local fan = require("fan")

return {
    new        = fan.udp.new_async,
    make_dest  = fan.udp.make_dest,
    make_dests = fan.udp.make_dests,
}
