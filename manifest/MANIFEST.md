# Manifest

Index of luafan2 v2 capabilities.  Each entry links to a detail file
under `manifest/` describing the feature, its milestone, and its
current test coverage.  Manifest entries reflect real shipped features,
not planning notes.

## Delivery + operations
- [Dual-form delivery: ./fan + fan.so](fan-so-module.md) — M15 thin
  executable that dlopen()s the fan.so shared module for dependency
  isolation (RTLD_LOCAL), while keeping v2's lua_State/loop ownership.
- [CI and release Docker images](ci-and-release-image.md) — M14.F
  GitHub Actions workflows + ubuntu / alpine runtime images published
  to Docker Hub.

## Application framework
- [Webase](webase.md) — M12.2 web application framework ported from
  v1's tmp/webase/, with LRU + mapping + service registry.

## Database
- [fan.sqlite3](sqlite3.md) — M5 + M16.1 native SQLite3 binding.
  Zero LuaRocks dependencies; drop-in replacement for the `lsqlite3`
  binding (matching API names, numeric result codes, iterator shapes,
  named parameters, column metadata).
- [fan.orm](orm.md) — M5 + M16.2 active-record ORM base with SQLite
  and MariaDB driver adapters.  Rows returned by insert/find_by/list
  are live objects with `:update()` / `:delete()` / `:remove()`
  methods; auto-diff persists only changed columns.

## Codecs
- [fan.json](json.md) — M9 + M16.3 native JSON codec.  RFC 8259
  numbers/escapes/UTF-8, sentinel-based null, explicit array/object
  markers.  M16.3 restored `is_nonempty_string` and `is_present`
  presence predicates from v1.

## Test coverage
- [Coverage — latest run](coverage-latest.md) — most recent
  C + Lua line coverage numbers with target thresholds.
- [Coverage plumbing](coverage.md) — how the three test modes
  (normal / --asan / --coverage) work; luacov `.luacov` layout;
  gcov / lcov invocation flow.
