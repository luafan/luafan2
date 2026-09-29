-- webase/webfile.lua — static file serving with LRU caches, ETag, gzip.
--
-- Ported from webase v1 with three dependency swaps to run on luafan2:
--   lfs.attributes(path)              -> fan.posix.stat(path)
--   md5.new():update(b):digest()      -> fan.crypto.md5_binary(b)
--   zlib.compress(b, nil, nil, 31)    -> fan.zlib.gzip_compress(b)
--
-- Behaviour is otherwise identical to v1:
--  * up to 1024 file_info entries (100 KiB budget) and 1024 body/gzip
--    entries (~100 MiB budget each) via lru.new
--  * ETag = md5(body) raw bytes (16-byte binary; kept binary because
--    that is exactly what v1 stored on `file_info.etag`)
--  * `If-None-Match` header compared byte-for-byte (case sensitive
--    because ETags are opaque quoted-strings, not tokens)
--  * gzip body cached separately; Content-Encoding: gzip served when
--    the request advertises it
--  * `Cache-Control: max-age=86400` on served files
--  * directory listing with HTML-escaped filenames
--  * `index.html` (configurable via `config.directory_index`) picked
--    up automatically before falling back to a listing
--  * hardened path handling: %00 rejects, "/" inside decoded segment
--    rejects, ".." pops but never above webroot
--  * HEAD returns the same headers + empty body

local fan       = require "fan"
local lru       = require "lru"
local mimetypes = require "mimetypes"
local config    = require "config"

local posix  = fan.posix
local crypto = fan.crypto
local zlib   = fan.zlib

local file_cache
local body_cache
local gzip_cache

local function html_escape(str)
    return str:gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;")
              :gsub('"', "&quot;"):gsub("'", "&#39;")
end

local function reset_cache()
    file_cache = lru.new(1024, 1024 * 100)
    body_cache = lru.new(1024, 1024 * 100 * 1024)
    gzip_cache = lru.new(1024, 1024 * 100 * 1024)
end

reset_cache()

local function split(str, pat)
    local t = {}
    if str then
        local fpat = "(.-)" .. pat
        local last_end = 1
        local s, e, cap = str:find(fpat, 1)
        while s do
            if s ~= 1 or cap ~= "" then
                table.insert(t, cap)
            end
            last_end = e + 1
            s, e, cap = str:find(fpat, last_end)
        end
        if last_end <= #str then
            cap = str:sub(last_end)
            table.insert(t, cap)
        end
    end
    return t
end

local function get_file_info(path)
    local file_info = file_cache:get(path)

    if not file_info then
        file_info = {path = path}
        -- v1 stored the whole `lfs.attributes` table on `file_info.attr`;
        -- v2's fan.posix.stat is API-compatible (mode / size / mtime).
        file_info.attr = posix.stat(path)
        file_cache:set(path, file_info, 100)
    end

    return file_info
end

local function get_file_body(file_info)
    local path = file_info.path
    local body = body_cache:get(path)
    if not body and file_info.attr and file_info.attr.mode ~= "directory" then
        local f = io.open(path, "rb")
        if f then
            body = f:read("*all")
            f:close()

            body_cache:set(path, body, #(body))
            print("load body", file_info.path)
        end
    end

    return body
end

-- Return the gzip-compressed body for a file, or nil if compression is
-- unavailable / failed (caller should then serve the identity body
-- without the Content-Encoding header).
--
-- M16.4: robustness fix — the previous version dereferenced the gzip
-- result with `#gbody` unconditionally.  If gzip_compress returned nil
-- (fan.zlib compiled out, or the input somehow tripped the compressor),
-- we'd (a) crash on `#nil`, (b) serve an empty body, and (c) still
-- announce `Content-Encoding: gzip`, so the client would try to inflate
-- an empty stream.  The three failures now short-circuit into a plain
-- fallback path.
local function get_file_gzip_body(file_info)
    local path = file_info.path
    local cached = gzip_cache:get(path)
    if cached then return cached end

    local body = get_file_body(file_info)
    if not body then return nil end   -- source read failed

    local gbody = zlib.gzip_compress(body)
    if not gbody then return nil end  -- compressor unavailable / errored

    gzip_cache:set(path, gbody, #gbody)
    return gbody
end

local function get_file_etag(file_info)
    if not file_info.etag then
        local body = get_file_body(file_info)
        -- v1 stored the raw 16-byte MD5 digest and sent it verbatim as
        -- the ETag header. That happened to work with libevent's evhttp
        -- because evhttp doesn't validate header value bytes, but:
        --   (a) evhttp_add_header now rejects 0x80+ bytes in modern
        --       libevent (M14.C-a's C backend 500s on it);
        --   (b) raw 16-byte strings can contain CR/LF, breaking the
        --       header line boundary in any hand-rolled writer.
        -- We store the lowercase-hex representation instead — same
        -- collision characteristics as v1 (still MD5(body)), portable
        -- across every HTTP backend, and 304 comparisons stay
        -- byte-for-byte because the client sends back the exact string
        -- it received.
        file_info.etag = crypto.md5(body)
        print("calc etag", file_info.path, file_info.etag)
    end

    return file_info.etag
end

local function is_within_webroot(path, webroot)
    if path:sub(1, #webroot) ~= webroot then
        return false
    end
    local next_char = path:sub(#webroot + 1, #webroot + 1)
    return next_char == "" or next_char == "/"
end

local function url_decode(str)
    return str:gsub("%%(%x%x)", function(hex)
        return string.char(tonumber(hex, 16))
    end)
end

local function web(req, resp)
    local parts = split(req.path, "/")
    local list = {config.webroot}
    for i, v in ipairs(parts) do
        local decoded = url_decode(v)
        -- Reject null-byte injection and embedded slashes (they'd break
        -- the "each `parts` element is one path segment" invariant).
        if decoded:find("\0") then
            return resp:reply(400, "Invalid Request", "")
        end
        if decoded:find("/") then
            return resp:reply(400, "Invalid Request", "")
        end
        if decoded == ".." then
            if #(list) > 1 then
                table.remove(list)
            else
                return resp:reply(400, "Invalid Request", "")
            end
        elseif decoded == "." then
            -- skip
        elseif #(decoded) > 0 then
            table.insert(list, decoded)
        end
    end

    local path = table.concat(list, "/")

    if not is_within_webroot(path, config.webroot) then
        return resp:reply(403, "Forbidden", "")
    end

    local file_info = get_file_info(path)
    local if_none_match = req.headers["If-None-Match"]
    if file_info and file_info.attr and file_info.attr.mode ~= "directory" and if_none_match then
        local etag = get_file_etag(file_info)
        if etag == if_none_match then
            local ext = path:match("([^.]+)$")
            if mimetypes[ext] then
                resp:addheader("Content-Type", mimetypes[ext])
            end

            return resp:reply(304, "Not Modified", "")
        end
    end

    if file_info.attr then
        if file_info.attr.mode == "directory" then
            -- 先尝试 index page
            local index_name = config.directory_index or "index.html"
            local index_path = path .. "/" .. index_name
            local index_info = get_file_info(index_path)
            if index_info and index_info.attr and index_info.attr.mode == "file" then
                path = index_path
                file_info = index_info
                -- fall through 到下面的文件 serve 逻辑
            else
                -- list directory
                resp:addheader("Content-Type", "text/html; charset=utf-8")
                resp:reply_start(200, "OK")

                list[1] = ""
                local parentpath = table.concat(list, "/")

                local entries = posix.readdir(path) or {}
                for _, name in ipairs(entries) do
                    if string.sub(name, 1, 1) ~= "." then
                        local f = path .. "/" .. name
                        local item_info = get_file_info(f)
                        local safe_name = html_escape(name)
                        local safe_href = html_escape(parentpath .. "/" .. name)

                        if item_info.attr and item_info.attr.mode == "directory" then
                            resp:reply_chunk(
                                string.format([[<a href="%s">[%s]</a><br/>]], safe_href, safe_name)
                            )
                        else
                            resp:reply_chunk(
                                string.format([[<a href="%s">%s</a><br/>]], safe_href, safe_name)
                            )
                        end
                    end
                end

                return resp:reply_end()
            end
        end

        -- serve file (regular file or resolved index.html)
        if file_info.attr.mode ~= "directory" then
            local body = get_file_body(file_info)
            if body then
                local ext = path:match("([^.]+)$")
                if mimetypes[ext] then
                    resp:addheader("Content-Type", mimetypes[ext])
                end

                local accept = req.headers["Accept-Encoding"]
                if accept and type(accept) == "string" and string.find(accept, "gzip") then
                    -- M16.4: only switch to the gzip body + advertise the
                    -- encoding when compression actually succeeded.  If
                    -- get_file_gzip_body returns nil we keep the identity
                    -- body and omit the header — a valid, spec-compliant
                    -- response.  Content-Length is set below off `body`,
                    -- so the client sees consistent framing either way.
                    local gbody = get_file_gzip_body(file_info)
                    if gbody then
                        body = gbody
                        resp:addheader("Content-Encoding", "gzip")
                    end
                end

                resp:addheader("Cache-Control", "max-age=86400")
                resp:addheader("ETag", get_file_etag(file_info))
                resp:addheader("Content-Length", #(body))
                if req.method == "HEAD" then
                    return resp:reply(200, "OK", "")
                else
                    return resp:reply(200, "OK", body)
                end
            end
        end
    end

    return resp:reply(404, "Not Found", "Not Found")
end

local function stats()
    return {
        file_cache = file_cache:stats(),
        body_cache = body_cache:stats(),
        gzip_cache = gzip_cache:stats(),
    }
end

return {
    web = web,
    reset_cache = reset_cache,
    stats = stats,
}
