# fan.json — JSON codec (M9 + M16.3 + M23)

Native C JSON codec, registered under `fan.json` when compiled in.  No
external dependencies (no cjson / lua-cjson / dkjson).  Full RFC 8259
conformance for numbers, escapes, surrogate pairs, and UTF-8 pass-through.

## API surface

### Core codec
- `json.encode(v)` → string  or  raises on unsupported types
- `json.decode(s)` → value  or  raises with line/col info on parse error

### Container-shape markers

Because Lua tables are ambiguous (a table with no keys could be an
empty array OR an empty object), fan.json exposes explicit constructors:

- `json.array()` / `json.array{ ... }` — tag as array; encodes to `[...]`
- `json.object()` / `json.object{ k=v, ... }` — tag as object; encodes to `{...}`
- `json.is_array(t)` / `json.is_object(t)` — reflection

Untagged tables use a heuristic: contiguous 1..#t integer keys → array,
otherwise object.  **Prefer explicit tagging** — the heuristic will
misidentify sparse or mixed-key tables.

### Null handling — the sentinel

JSON `null` is round-tripped through a **shared sentinel table**
`json.null`:

- `json.encode(json.null)` → `"null"`
- `json.decode("null")` → `json.null`   (the exact same table each time)
- Inside objects: `json.decode('{"a": null}').a` → `json.null`  (**not** Lua nil)
- `tostring(json.null)` → `"null"` (via `__tostring` metamethod, so
  `print()`, `string.format("%s", ...)`, string concat, and every
  logging library that stringifies its args all render null as the
  literal `"null"` rather than `"table: 0x7f..."`)
- `type(json.null)` → `"table"` (implementation detail: it's a table
  with a metatable, not a userdata)

The sentinel approach is deliberate: representing JSON null as Lua nil
would silently drop the key on encode round-trip (`{a=nil}` encodes to
`{}`, not `{"a":null}`), which loses information.  v2 has **no
`enable_null` toggle** — the sentinel behaviour is the only mode.  See
"Migration from v1" below if you're porting code that toggled this.

### Presence tests (M16.3)

Two convenience predicates for the common "validate a decoded body"
patterns that come up in HTTP handlers:

- `json.is_nonempty_string(v)` → boolean

  True iff `v` is a Lua string with length > 0.  The very common check
  for "did the caller pass a real, non-empty string field?":

  ```lua
  local body = json.decode(req.body)
  if not json.is_nonempty_string(body.name) then
    resp:reply(400, {}, "name required")
    return
  end
  ```

  Missing key (nil) → false.  Empty string `""` → false.  Any non-string
  (number, table, json.null, boolean) → false.

- `json.is_present(v)` → boolean

  True iff `v` is neither Lua nil nor the `json.null` sentinel.  Because
  decode always uses the sentinel for JSON null, a plain `if body.x then
  ... end` check treats "user wrote null" and "user wrote 42" the same
  way (both truthy).  `is_present` distinguishes them:

  ```lua
  local body = json.decode('{"a": null, "b": 42}')
  json.is_present(body.a)       -- false  (explicit null)
  json.is_present(body.b)       -- true   (42)
  json.is_present(body.missing) -- false  (absent -> nil)
  ```

  Note: empty string `""`, number `0`, boolean `false`, and `{}` are all
  **present** — they have a real, decoded value.  If you also want to
  reject `""`, combine with `is_nonempty_string`.

## Migration from v1

v1's `fan.json` had these API points; the M16.3 status is:

| v1 API | v2 status | Notes |
|---|---|---|
| `encode` / `decode` | ✅ same | |
| `array` / `object` | ✅ same | |
| `null` sentinel | ✅ same | |
| `is_nonempty_string` | ✅ M16.3 added | Identical semantics |
| `is_present` | ✅ M16.3 added | Identical semantics |
| **`enable_null(bool)`** | ❌ **intentionally omitted** | v2 always uses the sentinel.  Migration: delete the call entirely; if code depended on `enable_null(false)` for legacy nil-mapping, rewrite it to check `not json.is_present(v)` where it used to check `v == nil`. |
| `is_array` / `is_object` | ➕ new in v2 | Not in v1 |

## Encoding rules (edge cases)

- **NaN / Inf** — `encode` raises (JSON has no representation)
- **Sparse arrays** — untagged sparse tables encode as objects; use
  `json.array()` to force array shape (nil slots become `null`)
- **Integer subtype (Lua 5.3+)** — preserved on encode; decode pushes
  integer for whole values within `lua_Integer` range, else float
- **Float precision** — `%.17g` on encode, guaranteeing lossless IEEE
  754 double round-trip (`json.decode(json.encode(n)) == n` holds for
  every finite double).  **Do not** use `%.15g` as a "cleaner output"
  shortcut — it corrupts up to 2 bits of mantissa.  The regression
  test `floats round-trip within IEEE 754 doubles (M23: assert exact
  bit identity)` compares the decoded value to the Lua original
  directly, not via a re-encoded string, so a stealth downgrade
  cannot pass by having both sides lose precision together.
- **String escapes** — control chars < 0x20 → `\uXXXX`; `"` and `\`
  escaped; forward slash `/` NOT escaped (RFC allows either)
- **UTF-8** — pass-through verbatim in strings; no `\uXXXX` for BMP chars
- **Non-string object keys** — `encode` raises (JSON spec)
- **Circular references** (M23) — `encode` raises
  `"cannot encode a circular table as JSON"` when the recursion path
  revisits a table pointer.  Detection is **per-recursion-path**, not
  a global visited set: shared but non-circular subtables (e.g.
  `root = { left = child, right = child }`) still encode correctly.
  Pre-M23 behaviour was infinite recursion → SIGSEGV, uncatchable by
  pcall.

## Test coverage

`tests/lua/test_json.lua` — 27 tests covering:
- primitive round-trips
- empty containers (with disambiguation)
- deep nesting
- string escapes + UTF-8 encode/decode + surrogate pairs
- integer preservation
- **M23**: float round-trip asserting *bit-exact* equality to the
  Lua original (not via re-encoded string); includes
  `1.2345678901234567` as the key `%.15g`-vs-`%.17g` regression sample
- NaN/Inf rejection on encode
- decoder error message quality (line/col)
- json.null round-trip inside objects
- indent option pretty-print
- object key type check
- **M16.3**: `is_nonempty_string` on strings/nil/numbers/tables/`""`/`" "`
- **M16.3**: `is_present` on nil / json.null / real values (`""`, `0`,
  `false`, `{}`)
- **M16.3**: real-world case — decoded body with explicit `null` field,
  presence check combined with encode + is_nonempty_string sanity chain
- **M23 — circular-reference detection (6 cases)**:
  self-ref object; mutual-ref; self-ref array; shared non-circular
  subtable (must succeed — path-local, not global); 3-level deep
  cycle (grandparent reached); encoder state clean after error (next
  encode of a non-cyclic value succeeds)

## Build

Always compiled in (no `FAN_WITH_JSON` gate — JSON is a hard dependency
of fan.httpd, fan.http, fan.orm, and webase).  Zero external deps.
