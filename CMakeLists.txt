cmake_minimum_required(VERSION 3.15)
project(luafan2 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Feature switches (single build, toggled per deployment). M0 keeps them OFF;
# later milestones flip them ON as the OpenSSL/CURL/MariaDB/worker code lands.
# ---------------------------------------------------------------------------
option(FAN_WITH_OPENSSL "Enable TLS/SSL support (OpenSSL)"      OFF)
option(FAN_WITH_ZLIB    "Enable zlib deflate/inflate support"   OFF)
option(FAN_WITH_CURL    "Enable HTTP client (libcurl)"          OFF)
option(FAN_WITH_MARIADB "Enable MariaDB client"                 OFF)
option(FAN_WITH_SQLITE3 "Enable SQLite3 embedded DB"            OFF)
option(FAN_WITH_WORKER  "Enable multi-thread worker pool"       OFF)

# Build variants for testing / memory checking.
option(LUAFAN2_ASAN     "Build with AddressSanitizer + LeakSanitizer" OFF)
option(LUAFAN2_TESTING  "Build the C unit test runner"                ON)
option(FAN_COVERAGE     "Build with gcov instrumentation for lcov"    OFF)

# ---------------------------------------------------------------------------
# Common warnings + sanitizer / coverage flags
# ---------------------------------------------------------------------------
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
if(LUAFAN2_ASAN AND FAN_COVERAGE)
    message(FATAL_ERROR "LUAFAN2_ASAN and FAN_COVERAGE are mutually exclusive "
        "(-fsanitize=address + -fprofile-arcs has known interactions).")
endif()
if(LUAFAN2_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
    message(STATUS "LUAFAN2_ASAN=ON: AddressSanitizer enabled")
endif()
if(FAN_COVERAGE)
    # --coverage adds -fprofile-arcs -ftest-coverage at compile and -lgcov at link.
    # -O0 keeps line/branch tables faithful; -g gives lcov readable source refs.
    #
    # -fprofile-update=atomic is a defensive best practice: libcurl is built
    # with --enable-threaded-resolver, so DNS resolution briefly runs on a
    # helper thread whose gcov counter updates would otherwise race the main
    # thread's. It does NOT by itself fix the flaky "double free" abort we
    # see under coverage in test_httpd_c.lua / test_http_c.lua (that lives
    # elsewhere in the http C backend, still under investigation); we keep
    # the flag anyway because thread-safe counter writes are cheap and the
    # right thing to do whenever any linked library may run threads.
    add_compile_options(--coverage -O0 -g -fprofile-update=atomic)
    add_link_options(--coverage)
    # Enable the fan.http_c double-cleanup probe (see src/net/http.c). Only
    # active under coverage builds — it aborts BEFORE glibc's arena check so
    # the resulting gdb backtrace names the culprit call site instead of
    # some unrelated free() that trips over already-corrupted heap.
    add_compile_definitions(FAN_HTTP_C_TRACE_CLEANUP=1)
    message(STATUS "FAN_COVERAGE=ON: gcov instrumentation enabled "
                   "(-fprofile-update=atomic, FAN_HTTP_C_TRACE_CLEANUP=1)")
endif()

# ---------------------------------------------------------------------------
# Locate Lua (5.3/5.4 or LuaJIT). Headers only for the module (symbols come
# from the host interpreter at dlopen time); the test runner links liblua.
# ---------------------------------------------------------------------------
find_path(LUA_INCLUDE_DIR lua.h
    PATH_SUFFIXES lua5.3 lua5.4 lua luajit-2.1 luajit-2.0
    PATHS /usr/include /usr/local/include /opt/homebrew/include
)
# NOTE on Alpine: apk's lua5.3-dev installs the linker symlink as
# /usr/lib/lua5.3/liblua.so (a subdir under /usr/lib), not the more
# common /usr/lib/liblua5.3.so. Include `lua5.3` / `lua5.4` in the
# PATH_SUFFIXES so find_library recurses one level deeper on Alpine.
# On Debian/Ubuntu the file is at /usr/lib/<triplet>/liblua5.3.so and
# is picked up via the arch-specific suffixes below — no regression.
find_library(LUA_LIBRARY
    NAMES lua5.3 lua5.4 lua lua-5.3 lua-5.4 luajit-5.1
    PATHS /usr/lib /usr/local/lib /opt/homebrew/lib
    PATH_SUFFIXES lua5.3 lua5.4 x86_64-linux-gnu aarch64-linux-gnu
)
if(NOT LUA_INCLUDE_DIR)
    message(FATAL_ERROR "Lua headers not found (need lua.h)")
endif()
message(STATUS "Lua include: ${LUA_INCLUDE_DIR}")
message(STATUS "Lua library: ${LUA_LIBRARY}")

# ---------------------------------------------------------------------------
# Locate libevent (required for the runtime loop).
# ---------------------------------------------------------------------------
find_path(EVENT_INCLUDE_DIR event2/event.h
    PATHS /usr/include /usr/local/include /opt/homebrew/include /opt/homebrew/opt/libevent/include
)
find_library(EVENT_LIBRARY NAMES event
    PATHS /usr/lib /usr/local/lib /opt/homebrew/lib /opt/homebrew/opt/libevent/lib
    PATH_SUFFIXES x86_64-linux-gnu aarch64-linux-gnu
)
find_library(EVENT_PTHREADS_LIBRARY NAMES event_pthreads
    PATHS /usr/lib /usr/local/lib /opt/homebrew/lib /opt/homebrew/opt/libevent/lib
    PATH_SUFFIXES x86_64-linux-gnu aarch64-linux-gnu
)
if(NOT EVENT_INCLUDE_DIR OR NOT EVENT_LIBRARY)
    message(FATAL_ERROR "libevent not found (need event2/event.h + libevent)")
endif()
message(STATUS "libevent include: ${EVENT_INCLUDE_DIR}")
message(STATUS "libevent library: ${EVENT_LIBRARY}")

# ---------------------------------------------------------------------------
# Locate OpenSSL + libevent_openssl (only when TLS is enabled). net/tls.c wraps
# a socket bufferevent in bufferevent_openssl_socket_new, so we need both the
# crypto/ssl libraries and libevent's openssl shim.
# ---------------------------------------------------------------------------
if(FAN_WITH_OPENSSL)
    find_package(OpenSSL REQUIRED)
    find_library(EVENT_OPENSSL_LIBRARY NAMES event_openssl
        PATHS /usr/lib /usr/local/lib /opt/homebrew/lib /opt/homebrew/opt/libevent/lib
        PATH_SUFFIXES x86_64-linux-gnu aarch64-linux-gnu
    )
    if(NOT EVENT_OPENSSL_LIBRARY)
        message(FATAL_ERROR "libevent_openssl not found (need libevent-openssl); required by FAN_WITH_OPENSSL")
    endif()
    message(STATUS "OpenSSL: ${OPENSSL_SSL_LIBRARY} ${OPENSSL_CRYPTO_LIBRARY}")
    message(STATUS "libevent_openssl: ${EVENT_OPENSSL_LIBRARY}")
endif()

# ---------------------------------------------------------------------------
# Locate zlib (only when zlib support is enabled). Used for permessage-deflate
# in fan.websocket, gzip/deflate content-encoding in fan.http, etc.
# ---------------------------------------------------------------------------
if(FAN_WITH_ZLIB)
    find_package(ZLIB REQUIRED)
    message(STATUS "zlib: ${ZLIB_LIBRARIES}")
endif()

# ---------------------------------------------------------------------------
# Locate libcurl (only when enabled). Powers fan.http's C backend (M13.C).
# We use libcurl multi driven by our libevent event_base, and rely on
# curl_easy_escape / curl_easy_unescape for v1 fan.http.escape parity.
# ---------------------------------------------------------------------------
if(FAN_WITH_CURL)
    find_package(CURL REQUIRED)
    message(STATUS "libcurl: ${CURL_LIBRARIES} (${CURL_VERSION_STRING})")
endif()

# ---------------------------------------------------------------------------
# Locate SQLite3 (only when enabled). Provides fan.sqlite3 + fan.orm's
# built-in driver.
# ---------------------------------------------------------------------------
if(FAN_WITH_SQLITE3)
    find_package(SQLite3 REQUIRED)
    message(STATUS "sqlite3: ${SQLite3_INCLUDE_DIRS} ${SQLite3_LIBRARIES}")
endif()

# ---------------------------------------------------------------------------
# Locate MariaDB / libmariadb-dev (only when enabled). Provides fan.mariadb.
# ---------------------------------------------------------------------------
if(FAN_WITH_MARIADB)
    find_path(MARIADB_INCLUDE_DIR mysql.h
        PATH_SUFFIXES mariadb mysql
        PATHS /usr/include /usr/local/include /opt/homebrew/include)
    find_library(MARIADB_LIBRARY NAMES mariadb mariadbclient
        PATH_SUFFIXES x86_64-linux-gnu aarch64-linux-gnu
        PATHS /usr/lib /usr/local/lib /opt/homebrew/lib)
    if(NOT MARIADB_INCLUDE_DIR OR NOT MARIADB_LIBRARY)
        message(FATAL_ERROR "libmariadb-dev not found (need mysql.h + libmariadb)")
    endif()
    message(STATUS "mariadb: ${MARIADB_INCLUDE_DIR} ${MARIADB_LIBRARY}")
endif()

# ---------------------------------------------------------------------------
# Library source list (grows per milestone). This is everything EXCEPT main.c,
# so both the `fan` executable and the C unit test runner can reuse it.
# ---------------------------------------------------------------------------
set(FAN2_LIB_SOURCES
    src/luafan.c
    src/util/bytearray.c
    src/runtime/loop.c
    src/runtime/coro.c
    src/net/tcp.c
    src/net/fifo.c
    src/net/udp.c
    src/net/dns.c
    src/net/evdns.c
    src/net/tls.c
    src/net/zlib_wrap.c
    src/net/httpd.c
    src/net/httpd_metrics.c
    src/net/websocket.c
    src/net/http.c
    src/codec/stream.c
    src/codec/objectbuf.c
    src/codec/json.c
    src/sys/posix.c
    src/sys/popen.c
    src/crypto/crypto.c
    src/crypto/gcm.c
)
if(FAN_WITH_SQLITE3)
    list(APPEND FAN2_LIB_SOURCES src/db/sqlite3.c)
endif()
if(FAN_WITH_MARIADB)
    list(APPEND FAN2_LIB_SOURCES src/db/mariadb.c)
endif()

set(FAN2_DEFS "")
if(FAN_WITH_OPENSSL)
    list(APPEND FAN2_DEFS FAN_WITH_OPENSSL=1)
endif()
if(FAN_WITH_ZLIB)
    list(APPEND FAN2_DEFS FAN_WITH_ZLIB=1)
endif()
if(FAN_WITH_CURL)
    list(APPEND FAN2_DEFS FAN_WITH_CURL=1)
endif()
if(FAN_WITH_MARIADB)
    list(APPEND FAN2_DEFS FAN_WITH_MARIADB=1)
endif()
if(FAN_WITH_SQLITE3)
    list(APPEND FAN2_DEFS FAN_WITH_SQLITE3=1)
endif()
if(FAN_WITH_WORKER)
    list(APPEND FAN2_DEFS FAN_WITH_WORKER=1)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND FAN2_DEFS LUA_USE_LINUX _GNU_SOURCE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND FAN2_DEFS LUA_USE_MACOSX)
endif()

set(FAN2_INCLUDES src src/util src/runtime src/codec src/db src/crypto ${LUA_INCLUDE_DIR} ${EVENT_INCLUDE_DIR})

set(FAN2_LINK_LIBS ${EVENT_LIBRARY})
if(EVENT_PTHREADS_LIBRARY)
    list(APPEND FAN2_LINK_LIBS ${EVENT_PTHREADS_LIBRARY})
endif()
list(APPEND FAN2_LINK_LIBS m ${CMAKE_DL_LIBS})
if(FAN_WITH_OPENSSL)
    list(APPEND FAN2_LINK_LIBS ${EVENT_OPENSSL_LIBRARY} OpenSSL::SSL OpenSSL::Crypto)
    list(APPEND FAN2_INCLUDES ${OPENSSL_INCLUDE_DIR})
endif()
if(FAN_WITH_ZLIB)
    list(APPEND FAN2_LINK_LIBS ZLIB::ZLIB)
endif()
if(FAN_WITH_CURL)
    list(APPEND FAN2_LINK_LIBS CURL::libcurl)
endif()
if(FAN_WITH_SQLITE3)
    list(APPEND FAN2_LINK_LIBS SQLite::SQLite3)
    list(APPEND FAN2_INCLUDES  ${SQLite3_INCLUDE_DIRS})
endif()
if(FAN_WITH_MARIADB)
    list(APPEND FAN2_LINK_LIBS ${MARIADB_LIBRARY})
    list(APPEND FAN2_INCLUDES  ${MARIADB_INCLUDE_DIR})
endif()
find_package(Threads REQUIRED)
list(APPEND FAN2_LINK_LIBS Threads::Threads)

# libutil provides openpty/forkpty (Linux glibc: -lutil; macOS: in libSystem).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND FAN2_LINK_LIBS util)
endif()

# ---------------------------------------------------------------------------
# Dual-form delivery (M15):
#   1) build/fan.so   — Lua-loadable shared module (all v2 C code + heavy deps
#                       libcurl/libssl/libevent/... live here).  When a host
#                       Lua interpreter does `require("fan")` it dlopen()s this
#                       file with RTLD_LOCAL, so libcurl et al. stay scoped to
#                       fan.so's namespace and do not pollute the host.
#   2) build/fan      — thin executable that only owns main.c (signal, VM
#                       lifecycle, argv parsing) and links fan.so like any
#                       other consumer.  Its DT_NEEDED list ends up being
#                       essentially { liblua, libfan.so, libc } — no direct
#                       libcurl / libssl / libevent dependency.
#
# Rationale for the split: v1 fan.so achieved dependency isolation for free
# because Lua's package loader dlopen()s .so with RTLD_LOCAL.  v2 (up to M14)
# delivered only the executable form, which meant the process DT_NEEDED every
# dep globally.  Restoring the fan.so form (without giving up the executable)
# brings back that isolation while keeping v2's ownership of lua_State
# lifecycle in main.c.  See docs / manifest/fan-so-module.md.
# ---------------------------------------------------------------------------
if(NOT LUA_LIBRARY)
    message(FATAL_ERROR "liblua not found — the `fan` executable must link the Lua VM")
endif()

# ---- fan.so: Lua-loadable shared module carrying all v2 C code ----
add_library(fan_module MODULE ${FAN2_LIB_SOURCES})
target_include_directories(fan_module PRIVATE ${FAN2_INCLUDES})
target_compile_definitions(fan_module PRIVATE ${FAN2_DEFS})
# Note on liblua: when fan.so is loaded by a host Lua interpreter via
# `require("fan")` its lua_* symbols come from the host's own libLua (either
# statically linked into the interpreter or already dlopen()ed with
# RTLD_GLOBAL by the interpreter).  When fan.so is loaded by our own thin
# `fan` executable, the executable itself links liblua and its lua_*
# symbols are visible to fan.so through RTLD_GLOBAL scope (the executable's
# dynamic symbol table).  We still link liblua into fan.so directly so
# every symbol resolves at build time (macOS `ld` requires it; Linux `ld`
# is lenient but the extra NEEDED entry costs nothing and matches what v1
# fan.so shipped as well).
target_link_libraries(fan_module PRIVATE ${FAN2_LINK_LIBS} ${LUA_LIBRARY})
# Lua's require("fan") looks for `fan.so` (not `libfan.so`).  On macOS a
# MODULE library defaults to `.so` too (CMake makes it a bundle, not a
# dylib), so the same rule works everywhere we currently build.
set_target_properties(fan_module PROPERTIES
    OUTPUT_NAME "fan"
    PREFIX ""
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ---- fan: thin executable, only owns main.c; dlopen()s fan.so at runtime ----
#
# The executable does NOT link fan_module (which is a MODULE library — CMake
# forbids link-time dependencies on plugins, and RTLD_LOCAL isolation only
# works via runtime dlopen anyway).  main.c uses dlopen("fan.so", RTLD_LOCAL)
# + dlsym() to resolve `luaopen_fan`, `fan_clear_lua_states`, and
# `fan_loop_cleanup` at startup.  This is exactly the v1 loading model:
# libcurl / libssl / libevent land in fan.so's DT_NEEDED, but because
# RTLD_LOCAL keeps them scoped to fan.so's own namespace they don't leak
# into the executable's global symbol namespace.
#
# Resulting DT_NEEDED for `fan` on Linux: liblua + libdl + libc.  Everything
# else (curl/ssl/event/z/sqlite3/mariadb) is transitively pulled in through
# fan.so at dlopen time, not through the executable.
add_executable(fan src/main.c)
target_include_directories(fan PRIVATE ${FAN2_INCLUDES})
target_compile_definitions(fan PRIVATE ${FAN2_DEFS})
# CMAKE_DL_LIBS = "dl" on Linux (empty on macOS — libdl is in libSystem).
# We deliberately do NOT link fan_module here; see comment block above.
target_link_libraries(fan PRIVATE ${LUA_LIBRARY} ${CMAKE_DL_LIBS})
# fan_module (fan.so) must be built before `fan` runs, but there is no
# link dependency — express the build ordering explicitly.
add_dependencies(fan fan_module)
# NOTE: -rdynamic was previously needed so that a Lua C module dlopen()ed
# from the host could resolve symbols back into the executable.  Now that
# fan.so owns luaopen_fan and every other public symbol itself, and stack
# traces come from -g debug info rather than dynsym, -rdynamic is not
# required — dropping it keeps the executable's dynamic symbol table small.
set_target_properties(fan PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ---- C unit test runner ----
if(LUAFAN2_TESTING)
    enable_testing()
    file(GLOB C_TEST_SOURCES
        tests/c/framework/*.c
        tests/c/unit/*.c
    )
    add_executable(run_c_tests ${C_TEST_SOURCES} ${FAN2_LIB_SOURCES})
    target_include_directories(run_c_tests PRIVATE
        ${FAN2_INCLUDES}
        tests/c/framework
    )
    target_compile_definitions(run_c_tests PRIVATE ${FAN2_DEFS})
    target_link_libraries(run_c_tests ${LUA_LIBRARY} ${FAN2_LINK_LIBS})
    set_target_properties(run_c_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME c_unit COMMAND run_c_tests)
endif()
