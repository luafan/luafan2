# Dual-form delivery: `./fan` + `fan.so`  (M15)

Prior to M15, `luafan2` shipped as a single self-contained executable
(`build/fan`) with every C module linked in statically.  That was a
deliberate reaction to v1's fan.so-in-host-Lua model (see
`docs/luafan-v2-plan.md` §0), and it fixed real v1 problems: the global
VM lock, brittle `-rdynamic` symbol lookup back into the interpreter,
and per-thread `lua_State` ownership that assumed the host interpreter's
lifecycle.

**M15 restores the fan.so shape as a second, first-class output** without
giving up the executable form.  One build now produces two artefacts:

```
build/fan       — thin executable (~25 KB), owns lua_State + argv + signals
build/fan.so    — Lua-loadable module (~500 KB), carries every v2 C module
```

Both are built by the default `cmake --build --target fan` (via
`add_dependencies(fan fan_module)` — see `CMakeLists.txt` §M15 block).
Both release images (`Dockerfile.release.ubuntu`,
`Dockerfile.release.alpine`) COPY the pair side-by-side into
`/usr/local/bin/`.

## Why: dependency isolation

The whole reason to bring fan.so back is `RTLD_LOCAL`.  When any host Lua
does `require("fan")` its package loader dlopen()s `fan.so` with
`RTLD_LAZY | RTLD_LOCAL`.  Every symbol dragged in by fan.so — libcurl,
libssl, libcrypto, libevent, libsqlite3, libmariadb, libnghttp2,
libgnutls, libkrb5, libsasl2, libldap, libidn2, librtmp, libssh, libpsl,
libgssapi_krb5, libbrotlidec, ... — stays *scoped to fan.so's own
namespace*.  Nothing pollutes the host process's global symbol table.

The M14 executable-only form did **not** have this property.  Its
`DT_NEEDED` list carried libcurl / libssl / libevent / ... directly, so
those symbols entered the process's global scope.  When a Lua script
loaded some other native module that happened to also `dlopen` a
different libcurl / libssl (e.g. curl-impersonate for JA3 spoofing),
symbols collided and one library got the other's function bodies —
the exact pain point v1 fan.so had solved by accident.

M15 gets that isolation back without giving up `main.c`'s ownership of
lua_State/loop lifecycle: **the `fan` executable itself dlopen()s
`fan.so`** at startup rather than link-time-depending on it.

## The thin executable (`src/main.c`)

`main.c` is 273 lines total.  Its only jobs are:

1. `signal(SIGPIPE, SIG_IGN)` — libevent bufferevent writes to dead peers
   otherwise take default SIGPIPE and kill the process on macOS.
2. `luaL_newstate()` + `luaL_openlibs()`.
3. **`load_fan_so(argv[0])`** — resolve executable path via
   `/proc/self/exe` (Linux) or `_NSGetExecutablePath` (macOS), fall back
   to `argv[0]`; `dirname()` it; `dlopen("<dir>/fan.so", RTLD_LAZY |
   RTLD_LOCAL)`; `dlsym()` the three ABI symbols:
     - `luaopen_fan`          — module entry for `luaL_requiref`
     - `fan_clear_lua_states` — NULL every module's cached main-thread
                                pointer before `lua_close(L)`
     - `fan_loop_cleanup`     — free the shared event base after
                                `lua_close(L)`
4. `luaL_requiref(L, "fan", fan_so.luaopen_fan, 1)`.
5. argv parsing (`-v`, `-h`, `-e "chunk"`, script path).
6. `docall()` + msghandler-driven traceback on error.
7. `fan_so.fan_clear_lua_states(); lua_close(L); fan_so.fan_loop_cleanup();`.

Everything else — TCP, HTTP, WebSocket, TLS, DNS, JSON, ORM, worker,
crypto, zlib, sqlite3, mariadb, webase — lives behind `fan.so`.

The dlopen path is **not** searched via `LD_LIBRARY_PATH`.  We explicitly
require `fan.so` to be sitting next to the executable, mirroring the
install shape and refusing to be surprised by a stray fan.so somewhere
else on the search path.

## CMake shape

```cmake
# fan.so — MODULE library, exports luaopen_fan et al.
add_library(fan_module MODULE ${FAN2_LIB_SOURCES})
set_target_properties(fan_module PROPERTIES
    OUTPUT_NAME "fan"           # produces fan.so, not libfan.so
    PREFIX      ""
    SUFFIX      ".so")
target_link_libraries(fan_module PRIVATE ${FAN2_LINK_LIBS} ${LUA_LIBRARY})

# fan — thin executable, does NOT link fan_module (CMake forbids linking
# MODULE libraries anyway; and RTLD_LOCAL isolation only works via
# runtime dlopen).
add_executable(fan src/main.c)
target_link_libraries(fan PRIVATE ${LUA_LIBRARY} ${CMAKE_DL_LIBS})
add_dependencies(fan fan_module)   # build order only
```

Notes:

- `fan_module` links `${LUA_LIBRARY}` explicitly.  macOS `ld` requires
  every symbol resolve at link time (no `-undefined dynamic_lookup`
  hack), and even on Linux the extra `NEEDED liblua5.3.so.0` entry is
  free and matches what v1 fan.so shipped.
- The old executable-only build used `target_link_options(fan PRIVATE
  -rdynamic)` so a dlopen'd C module could resolve symbols back into
  the executable.  M15 drops that: `fan.so` exports its own
  `luaopen_fan`, and stack traces work via `-g` debug info, not the
  dynamic symbol table.
- `add_library(fan_module MODULE ...)` is *deliberate*; a `SHARED`
  library would allow the executable to link it directly, which would
  DT_NEEDED-attach fan.so and make its symbols visible under
  `RTLD_GLOBAL` semantics on Linux — defeating the point.
- `run_c_tests` is unchanged: it links `${FAN2_LIB_SOURCES}` statically.
  Unit tests want deterministic static-init order and don't benefit
  from the shared-module split.  Keeping it static is the reference
  point for how the executable would look if we ever regressed.

## Cross-interpreter smoke test

`tests/lua_smoke_fan_so.lua`, run under stock `/usr/bin/lua5.3` at the
end of the `normal` mode of `tests/run_tests.sh`, verifies the second
half of the dual-form promise: any host Lua interpreter can
`require("fan")` and get back a working module table.  It does not
exercise the network — that's what the standard `test_*.lua` suites
under `./fan` are for — but it catches every failure mode where
`fan.so` cannot be dlopen()ed cleanly (missing symbol, missing NEEDED
library, wrong ABI, etc.).

## Verified DT_NEEDED shape (Linux aarch64, glibc)

```
$ readelf -d build/fan | grep NEEDED
    NEEDED  [liblua5.3.so.0]
    NEEDED  [libc.so.6]
    NEEDED  [ld-linux-aarch64.so.1]

$ readelf -d build/fan.so | grep NEEDED
    NEEDED  [libevent-2.1.so.7]
    NEEDED  [libevent_openssl-2.1.so.7]
    NEEDED  [libssl.so.3]
    NEEDED  [libcrypto.so.3]
    NEEDED  [libz.so.1]
    NEEDED  [libcurl.so.4]
    NEEDED  [libsqlite3.so.0]
    NEEDED  [libmariadb.so.3]
    NEEDED  [liblua5.3.so.0]
    NEEDED  [libc.so.6]
```

On macOS the same split holds (`otool -L build/fan` shows only
`liblua.dylib + libSystem`; every curl/ssl/event dep lives under
`build/fan.so`).

Sizes on Linux aarch64 (Debug build): `fan` 25 KB, `fan.so` 514 KB.
Release + strip is smaller (measured in the release images).
