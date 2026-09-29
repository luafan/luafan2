--[[
  test_webase_lru.lua — unit tests for webase/lru.lua.

  webase/lru.lua is a verbatim copy of lua-lru (MIT), used by
  webase/webfile.lua as three separate caches (file / body / gzip).
  Its behaviour is the entire contract webfile depends on, so we
  test it directly rather than trying to exercise every corner
  case through webfile's public surface.

  Runs as a plain unit test — no fan loop, no coroutines, no
  network. lru is pure Lua and depends on nothing outside standard
  library plus the module-scope `assert`.

  Coverage targets (miss lines that test_webase.lua's integration
  path never reaches):
    - cut()'s three non-only-element branches (middle / oldest / newest)
    - makeFreeSpace eviction loop (max_size and max_bytes triggers)
    - set() delete-existing-key path
    - delete() alias
    - pairs() iterator (both entry point and middle steps)
    - update_bytes()
    - reset_stats()
    - "not enough storage" assertion path
]]

local T = require("test_framework")

-- Add webase/ to package.path so `require "lru"` resolves. Path is
-- computed relative to this test file so we don't depend on CWD.
local this_dir = (debug.getinfo(1, "S").source:gsub("^@", ""):match("(.*)/[^/]+$")) or "."
local ROOT = this_dir:gsub("/tests/lua$", "")
package.path = ROOT .. "/webase/?.lua;" .. package.path

local lru = require("lru")

local s = T.suite("webase.lru (M12.2)")

-- ---- constructor guards --------------------------------------------------

s:test("new: max_size >= 1 is enforced", function()
  T.error_raised(function() lru.new(0) end)
  T.error_raised(function() lru.new(-1) end)
end)

s:test("new: max_bytes must be nil or >= 1", function()
  T.error_raised(function() lru.new(4, 0) end)
  T.error_raised(function() lru.new(4, -5) end)
  -- nil max_bytes is allowed (unlimited bytes budget).
  local c = lru.new(4)
  T.not_nil(c)
end)

-- ---- basic set / get / miss ---------------------------------------------

s:test("get on empty cache returns nil and counts miss", function()
  local c = lru.new(4)
  T.is_nil(c:get("nope"))
  local st = c:stats()
  T.eq(st.hits, 0)
  T.eq(st.misses, 1)
  T.eq(st.size, 0)
end)

s:test("set then get returns value and counts hit", function()
  local c = lru.new(4)
  c:set("k", "v")
  T.eq(c:get("k"), "v")
  local st = c:stats()
  T.eq(st.hits, 1)
  T.eq(st.misses, 0)
  T.eq(st.size, 1)
end)

s:test("set of existing key replaces (delete + insert path)", function()
  local c = lru.new(4)
  c:set("k", "v1", 10)
  c:set("k", "v2", 20)
  T.eq(c:get("k"), "v2")
  local st = c:stats()
  T.eq(st.size, 1, "size stays 1 after replace")
  T.eq(st.bytes_used, 20, "bytes count reflects the new value only")
end)

-- ---- eviction: max_size trigger -----------------------------------------

s:test("eviction: oldest key drops when max_size is exceeded", function()
  local c = lru.new(2)
  c:set("a", 1)
  c:set("b", 2)
  c:set("c", 3)  -- forces eviction of "a"
  T.is_nil(c:get("a"), "a should have been evicted")
  T.eq(c:get("b"), 2)
  T.eq(c:get("c"), 3)
  local st = c:stats()
  T.eq(st.evictions, 1)
  T.eq(st.size, 2)
end)

s:test("eviction: LRU order is respected (accessed = newest)", function()
  local c = lru.new(2)
  c:set("a", 1)
  c:set("b", 2)
  T.eq(c:get("a"), 1)      -- promotes a to newest
  c:set("c", 3)             -- should evict b, NOT a
  T.eq(c:get("a"), 1, "a survived because it was just accessed")
  T.is_nil(c:get("b"), "b was the LRU element and got evicted")
end)

-- ---- eviction: max_bytes trigger ----------------------------------------

s:test("eviction: max_bytes triggers even when size is under cap", function()
  local c = lru.new(10, 100)
  c:set("a", "aa", 60)
  c:set("b", "bb", 60)  -- 60+60 > 100, must evict a
  T.is_nil(c:get("a"))
  T.eq(c:get("b"), "bb")
  T.eq(c:stats().bytes_used, 60)
end)

s:test("assertion: single value larger than max_bytes cannot fit", function()
  local c = lru.new(4, 100)
  T.error_raised(function() c:set("big", "x", 500) end)
end)

-- ---- delete via set-nil and delete() alias ------------------------------

s:test("set(key, nil) removes the entry", function()
  local c = lru.new(4)
  c:set("k", "v", 5)
  c:set("k", nil)
  T.is_nil(c:get("k"))
  T.eq(c:stats().size, 0)
  T.eq(c:stats().bytes_used, 0)
end)

s:test("delete(key) is an alias for set(key, nil)", function()
  local c = lru.new(4)
  c:set("k", "v", 5)
  c:delete("k")
  T.is_nil(c:get("k"))
  T.eq(c:stats().size, 0)
end)

s:test("set with nil value and nil key raises (key may not be nil)", function()
  local c = lru.new(4)
  T.error_raised(function() c:set(nil, nil) end)
end)

-- ---- cut() branches via targeted deletes --------------------------------

-- cut() has three non-degenerate branches:
--   1. tuple is in the middle (both prev and next set)
--   2. tuple is oldest (only prev set)
--   3. tuple is newest (only next set)
-- We hit all three by deleting keys in specific positions of a 3-element
-- cache. The 4th branch (single-element cache) is hit by the trivial
-- "delete the only entry" case above.

s:test("cut: delete middle entry keeps outer entries linked", function()
  local c = lru.new(4)
  c:set("a", 1)  -- newest end after each set: a
  c:set("b", 2)  -- b (newest) -> a (oldest)
  c:set("c", 3)  -- c -> b -> a
  c:delete("b")  -- middle
  -- Both remaining entries must still be reachable via pairs.
  local seen = {}
  for k, v in c:pairs() do seen[k] = v end
  T.eq(seen.a, 1)
  T.eq(seen.c, 3)
  T.is_nil(seen.b)
end)

s:test("cut: delete oldest entry updates oldest pointer", function()
  local c = lru.new(4)
  c:set("a", 1)  -- becomes oldest once b,c are inserted
  c:set("b", 2)
  c:set("c", 3)
  c:delete("a")  -- oldest
  -- Insert a new element — the next eviction should drop "b", proving
  -- "a"'s removal correctly re-anchored the oldest pointer to "b".
  local c2 = lru.new(2)
  c2:set("a", 1)
  c2:set("b", 2)
  c2:delete("a")
  c2:set("c", 3)  -- should NOT evict anything (only 2 items live)
  T.eq(c2:stats().evictions, 0)
  T.eq(c2:get("b"), 2)
  T.eq(c2:get("c"), 3)
end)

s:test("cut: delete newest entry updates newest pointer", function()
  local c = lru.new(4)
  c:set("a", 1)
  c:set("b", 2)
  c:set("c", 3)  -- c is newest
  c:delete("c")  -- newest
  -- After deletion, a fresh set should place the entry at newest without
  -- disturbing existing order; pairs() iterates newest -> oldest.
  c:set("d", 4)
  local order = {}
  for k in c:pairs() do order[#order + 1] = k end
  T.eq(order[1], "d", "d becomes new newest")
end)

-- ---- iterator (mynext / __pairs) ----------------------------------------

s:test("pairs(): iterates from newest to oldest", function()
  local c = lru.new(4)
  c:set("a", 1)
  c:set("b", 2)
  c:set("c", 3)  -- newest -> c, b, a -> oldest
  local order = {}
  for k, v in c:pairs() do order[#order + 1] = { k, v } end
  T.eq(#order, 3)
  T.eq(order[1][1], "c")
  T.eq(order[2][1], "b")
  T.eq(order[3][1], "a")
end)

s:test("__pairs metamethod: `for k in pairs(cache)` also works", function()
  local c = lru.new(4)
  c:set("x", 10)
  c:set("y", 20)
  local n = 0
  -- Lua 5.3 honours __pairs. If we're on an older runtime this still
  -- works because setmetatable exposes __pairs and we call it directly.
  for _ in pairs(c) do n = n + 1 end
  T.eq(n, 2)
end)

s:test("pairs(): empty cache yields nothing", function()
  local c = lru.new(4)
  local n = 0
  for _ in c:pairs() do n = n + 1 end
  T.eq(n, 0)
end)

-- ---- update_bytes and reset_stats ---------------------------------------

s:test("update_bytes: adjusts entry BYTES and cache bytes_used", function()
  local c = lru.new(4, 1000)
  c:set("k", "v", 50)
  T.eq(c:stats().bytes_used, 50)
  c:update_bytes("k", 25)   -- +25
  T.eq(c:stats().bytes_used, 75)
  c:update_bytes("k", -10)  -- -10
  T.eq(c:stats().bytes_used, 65)
end)

s:test("update_bytes: no-op for missing key", function()
  local c = lru.new(4, 1000)
  c:set("k", "v", 50)
  c:update_bytes("missing", 999)
  T.eq(c:stats().bytes_used, 50, "unaffected by missing key")
end)

s:test("reset_stats: zeroes hits/misses/evictions but keeps entries", function()
  local c = lru.new(2)
  c:set("a", 1); c:set("b", 2); c:set("c", 3)  -- 1 eviction
  c:get("a")   -- miss (evicted)
  c:get("b")   -- hit
  local before = c:stats()
  T.truthy(before.hits > 0 and before.misses > 0 and before.evictions > 0)

  c:reset_stats()
  local after = c:stats()
  T.eq(after.hits, 0)
  T.eq(after.misses, 0)
  T.eq(after.evictions, 0)
  T.eq(after.size, 2, "entries themselves are untouched")
end)

os.exit(T.run(s))
