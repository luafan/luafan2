-- fan/upnp.lua — LuaFan v2 UPnP IGD client (M9).
--
-- Public API (v1-compatible):
--   local upnp = require "fan.upnp"
--   local device = upnp.new(timeout_sec)
--     -- performs SSDP M-SEARCH on 239.255.255.250:1900 asking for
--     -- InternetGatewayDevice:1; waits `timeout_sec` seconds; then
--     -- fetches each responder's device-description XML and picks the
--     -- WANIPConnection:1 controlURL.
--     -- `device.devices` = array of {st, server, url} (url = SOAP endpoint).
--   local ok, body = device:AddPortMapping(intIP, intPort, extPort,
--                                          protocol, description?)
--     -- Sends a SOAP AddPortMapping to the first WANIPConnection service.
--     -- ok = true when the HTTP response is 200; body = raw SOAP response.
--
-- Contract preserved from v1 fan.upnp:
--   * Only new() and :AddPortMapping() are exposed. GetExternalIPAddress /
--     DeletePortMapping are NOT in v1 and are NOT added here.
--   * Multicast group + port + M-SEARCH template + ST value + SOAPAction
--     header + XML body layout are byte-identical to v1 so a real router
--     that accepted v1 requests accepts these.
--
-- Rewrite notes (why we didn't just copy v1):
--   * v1 code used `http.post { url=..., ... }` (single-table form) and
--     `ret.responseCode`. luafan2's fan.http uses `M.post(url, opts)`
--     with `resp.status`. We use the v2 shape here — the public
--     contract of fan.upnp is the same either way.
--   * v1 had a bespoke coroutine.wrap(timeout) trick that resumed a
--     stored coroutine by upvalue. We use fan.sleep + a flag, which is
--     both simpler and safe against double-resume when the caller
--     itself yields concurrently.
--   * v1 SSDP parser is case-sensitive (LOCATION / ST / SERVER); we do
--     lowercase-insensitive matching, matching real-world routers.
--   * v1 built the SOAP endpoint URL by re-slicing the LOCATION header
--     with a fragile pattern. We use fan.http.parse_url to resolve
--     controlURL against the base URL properly (handles absolute /
--     relative / scheme-only variants).

local fan  = require "fan"
local http = require "fan.http"
local udp  = fan.udp  -- fan.udp is a C-level sub-table of the built-in `fan`

local M = {}

local MCAST_HOST = "239.255.255.250"
local MCAST_PORT = 1900
local WANIP_ST   = "urn:schemas-upnp-org:service:WANIPConnection:1"
local IGD_ST     = "urn:schemas-upnp-org:device:InternetGatewayDevice:1"

-- ---------------------------------------------------------------------------
-- SOAP body for AddPortMapping. Kept as a template with %s placeholders in
-- the same order as v1 (ext, protocol, int_port, int_ip, description) so
-- the wire format is byte-identical.
-- ---------------------------------------------------------------------------
local ADD_PORT_MAPPING_TEMPLATE = [[<?xml version="1.0" encoding="utf-8"?>
<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">
  <s:Body>
    <u:AddPortMapping xmlns:u="urn:schemas-upnp-org:service:WANIPConnection:1">
      <NewRemoteHost></NewRemoteHost>
      <NewExternalPort>%s</NewExternalPort>
      <NewProtocol>%s</NewProtocol>
      <NewInternalPort>%s</NewInternalPort>
      <NewInternalClient>%s</NewInternalClient>
      <NewEnabled>1</NewEnabled>
      <NewPortMappingDescription>%s</NewPortMappingDescription>
      <NewLeaseDuration>0</NewLeaseDuration>
    </u:AddPortMapping>
  </s:Body>
</s:Envelope>]]

-- ---------------------------------------------------------------------------
-- Case-insensitive header extractor for SSDP responses. SSDP is HTTP/1.1
-- over UDP, so headers are ASCII text terminated by CRLF. We match by
-- lowercasing the whole message once and searching for the lowercased key.
--
-- Returns the trimmed header value, or nil.
-- ---------------------------------------------------------------------------
local function ssdp_header(msg_lower, msg, key)
    local pat = "\n" .. key:lower() .. ":%s*"
    local s, e = msg_lower:find(pat)
    if not s then return nil end
    -- reach into the original message to preserve URL casing etc.
    local rest = msg:sub(e + 1)
    local line_end = rest:find("[\r\n]") or (#rest + 1)
    local val = rest:sub(1, line_end - 1)
    return (val:gsub("^%s+", ""):gsub("%s+$", ""))
end

-- ---------------------------------------------------------------------------
-- Resolve a controlURL string (which may be absolute, root-relative, or
-- scheme-less) against a base URL from the device's LOCATION header.
--
-- Returns "scheme://host[:port]<path>" or nil,err.
-- ---------------------------------------------------------------------------
local function resolve_control_url(location, control_url)
    if control_url:match("^%w+://") then
        return control_url
    end
    local scheme, host, port = http.parse_url(location)
    if not scheme then return nil, "cannot parse LOCATION: " .. tostring(location) end
    local base = scheme .. "://" .. host
    if port and not ((scheme == "http" and port == 80)
                  or (scheme == "https" and port == 443)) then
        base = base .. ":" .. tostring(port)
    end
    if control_url:sub(1, 1) ~= "/" then
        control_url = "/" .. control_url
    end
    return base .. control_url
end

-- ---------------------------------------------------------------------------
-- SSDP M-SEARCH: send one datagram, collect responses for `timeout_sec`.
--
-- v2's fan.udp is pull-based (sock:recv() yields until a packet arrives
-- or the socket closes) rather than the callback-driven onread model v1
-- used. We spawn one coroutine to loop on recv() while the caller
-- coroutine sleeps for `timeout_sec`; when it wakes it closes the
-- socket, which unblocks recv() with (nil, "closed") and the collector
-- exits cleanly.
--
-- We don't join the multicast group because the router unicasts its
-- reply directly back to our source port.
-- ---------------------------------------------------------------------------
local function ssdp_search(timeout_sec)
    local sock, err = udp.new(nil, 0)  -- INADDR_ANY, kernel-picked port
    if not sock then return nil, "ssdp socket: " .. tostring(err) end

    local responses = {}
    -- Collector coroutine: drain until the socket closes.
    fan.spawn(function()
        while true do
            local data = sock:recv()
            if not data then return end
            responses[#responses + 1] = data
        end
    end)

    local msearch = table.concat({
        "M-SEARCH * HTTP/1.1",
        "Host:" .. MCAST_HOST .. ":" .. MCAST_PORT,
        "ST: " .. IGD_ST,
        'Man: "ssdp:discover"',
        string.format("MX:%d", math.max(1, math.floor(timeout_sec))),
        "",
        "",
    }, "\r\n")

    local ok, serr = sock:sendto(msearch, MCAST_HOST, MCAST_PORT)
    if not ok then
        sock:close()
        return nil, "ssdp sendto: " .. tostring(serr)
    end

    fan.sleep(timeout_sec)
    sock:close()
    return responses
end

-- ---------------------------------------------------------------------------
-- Fetch and parse a device-description SCPD document, returning the
-- controlURL for the WANIPConnection:1 service if present.
--
-- v1 used string.find on the raw XML with lax patterns; we do the same
-- rather than pulling in a real XML parser. Routers ship this XML with
-- predictable formatting, so plain-text scanning is fine for our
-- match-or-skip needs.
-- ---------------------------------------------------------------------------
local function extract_wanip_control_url(xml)
    local s = xml:find("<serviceType>" .. WANIP_ST .. "</serviceType>", 1, true)
    if not s then return nil end
    local _, _, path = xml:find("<controlURL>([^><]+)</controlURL>", s)
    return path
end

-- ---------------------------------------------------------------------------
-- Device object methods.
-- ---------------------------------------------------------------------------
local Device = {}
Device.__index = Device

function Device:AddPortMapping(intIP, intPort, extPort, protocol, description)
    assert(intIP and intPort and extPort and protocol,
           "AddPortMapping requires (intIP, intPort, extPort, protocol)")
    description = description or "fan.upnp"

    for _, dev in ipairs(self.devices) do
        if dev.st == WANIP_ST then
            local body = string.format(ADD_PORT_MAPPING_TEMPLATE,
                extPort, tostring(protocol):upper(), intPort, intIP, description)
            local resp, err = http.request{
                url    = dev.url,
                method = "POST",
                headers = {
                    ["Content-Type"] = "text/xml",
                    -- v1 sends the SOAPAction value pre-quoted; keep byte-identical.
                    ["SOAPAction"] = '"' .. WANIP_ST .. "#AddPortMapping" .. '"',
                },
                body = body,
            }
            if not resp then return false, err end
            return resp.status == 200, resp.body
        end
    end
    return false, "no WANIPConnection:1 service in this device set"
end

-- ---------------------------------------------------------------------------
-- Given a list of raw SSDP response datagrams, parse each, fetch its SCPD
-- XML, and build the device list. Split out from M.new so tests can drive
-- the HTTP+SOAP paths without needing multicast privileges.
-- ---------------------------------------------------------------------------
local function _devices_from_responses(responses)
    local devices = {}
    local seen_location = {}
    for _, msg in ipairs(responses) do
        local low = msg:lower()
        local location = ssdp_header(low, msg, "LOCATION")
        local st       = ssdp_header(low, msg, "ST")
        local server   = ssdp_header(low, msg, "SERVER")
        if location and st and not seen_location[location] then
            seen_location[location] = true
            local scpd = http.request{ url = location, method = "GET" }
            if scpd and scpd.status == 200 and scpd.body then
                local control_path = extract_wanip_control_url(scpd.body)
                if control_path then
                    local url = resolve_control_url(location, control_path)
                    if url then
                        devices[#devices + 1] = {
                            st     = WANIP_ST,
                            server = server,
                            url    = url,
                        }
                    end
                end
            end
        end
    end
    return devices
end
M._devices_from_responses = _devices_from_responses  -- exposed for tests

-- ---------------------------------------------------------------------------
-- Public entry point: discover IGD devices and return a Device object.
-- ---------------------------------------------------------------------------
function M.new(timeout_sec)
    timeout_sec = timeout_sec or 3
    if type(timeout_sec) ~= "number" or timeout_sec <= 0 then
        error("fan.upnp.new: timeout_sec must be > 0", 2)
    end

    local responses, err = ssdp_search(timeout_sec)
    if not responses then
        -- Return an empty device set so callers can still :AddPortMapping()
        -- deterministically (they'll get false, "no WANIP..." back).
        return setmetatable({ devices = {}, ssdp_error = err }, Device)
    end
    return setmetatable({ devices = _devices_from_responses(responses) }, Device)
end

-- Test helper: wrap a raw device list in a Device instance. Exposed so
-- tests can drive :AddPortMapping without going through M.new + SSDP.
function M._wrap(devices)
    return setmetatable({ devices = devices or {} }, Device)
end

return M
