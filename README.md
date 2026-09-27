# LuaFan v2 (second generation)

[![CI](https://github.com/luafan/luafan2/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/luafan/luafan2/actions/workflows/ci.yml)
[![Docker Ubuntu](https://img.shields.io/docker/v/luafan/luafan2-ubuntu?label=luafan2-ubuntu&logo=docker&sort=semver)](https://hub.docker.com/r/luafan/luafan2-ubuntu)
[![Docker Alpine](https://img.shields.io/docker/v/luafan/luafan2-alpine?label=luafan2-alpine&logo=docker&sort=semver)](https://hub.docker.com/r/luafan/luafan2-alpine)

Second-generation reimplementation of [LuaFan](https://github.com/luafan/luafan).
See design docs in the site's `docs/`:
- `docs/luafan-v1-analysis.md` — analysis of the v1 codebase
- `docs/luafan-v2-plan.md` — v2 architecture, milestones & test matrix
- `docs/baseline/v1-baseline-arm1.md` — v1 baseline measured on arm1 (ARM64)

## Why v2

v1 shares **one `lua_State`** across worker threads and serialises it with a
**global lock that hooks the Lua core** (`fan_lua_lock`). That model needed a
long series of concurrency/memory fixes (R1–R20) and a custom-built interpreter.

v2 changes the concurrency model to **per-thread `lua_State` + message passing**:
no global VM lock, no interpreter hook, works on stock Lua / LuaJIT, real
multi-core Lua parallelism. A single-`State` compatibility mode remains available
via `fan.compat`.

## Layout

```
luafan2/
  src/
    platform.h          # centralized platform detection & feature switches
    main.c              # the `fan` executable entry (embeds Lua, owns lua_State)
    luafan.c            # built-in `fan` library (luaopen_fan)
    util/bytearray.c    # dynamic byte buffer (overflow-safe)
    runtime/            # loop + coro (+ ref/message queue in later milestones)
  lua/                  # Lua-layer modules (connector/http/httpd/orm/...)
  tests/
    c/                  # C unit tests + framework
    lua/                # Lua tests + framework (run via ./fan)
    run_tests.sh        # build + run (normal / --asan memory gate)
  CMakeLists.txt        # single build, feature options, ASan variant
```

`fan` is a **self-contained executable** (no `fan.so`): it embeds the Lua VM,
statically links every v2 C module, and preloads the built-in `fan` library.
Scripts use `local fan = require("fan")` and run via `fan script.lua`.

## Build & test

Every mode runs inside a single Docker image (`luafan2-ci:local`,
built on `ubuntu:22.04`) so builds are reproducible and match CI.
The image is built automatically on first use from
`tests/Dockerfile`.

```sh
# normal build: C unit tests + Lua tests (Lua tests run under the built ./fan)
sh tests/run_tests.sh

# memory gate: AddressSanitizer + LeakSanitizer (target: leaked=0)
sh tests/run_tests.sh --asan

# coverage gate: gcov (C, via lcov) + luacov (Lua). Soft-warn by default;
# `--enforce` turns threshold misses (C≥85%, Lua≥90% line coverage) into
# a hard failure. HTML report -> build-coverage/coverage-html/.
sh tests/run_tests.sh --coverage
sh tests/run_tests.sh --coverage --enforce
```

The three modes are mutually exclusive: ASan and coverage share the
compiler flag namespace and CMake refuses to combine them. Details in
`manifest/coverage.md`; latest run summary in `manifest/coverage-latest.md`.

CMake feature options (all OFF by default, flipped on per milestone):
`-DFAN_WITH_OPENSSL=ON -DFAN_WITH_CURL=ON -DFAN_WITH_MARIADB=ON -DFAN_WITH_WORKER=ON`.

## Docker

Prebuilt release images are published to Docker Hub as multi-arch
(`linux/amd64` + `linux/arm64`) manifests. Two flavours are available:

| Image                          | Base            | libc  | Size (typ.) |
|--------------------------------|-----------------|-------|-------------|
| `luafan/luafan2-ubuntu`        | `ubuntu:22.04`  | glibc | ~79 MB      |
| `luafan/luafan2-alpine`        | `alpine:3.20.10`| musl  | ~18 MB      |

Tags:
- `latest`         — head of `main` after CI passes
- `<sha>`          — immutable, pinned to the git commit CI validated
- `vX.Y.Z`         — pushed when a git tag matching `v*` is created

Both images ship the `fan` executable at `/usr/local/bin/fan` and the
pure-Lua modules under `/usr/local/share/lua/5.3/`. No `luarocks` or
third-party rocks are preinstalled — v2's Lua layer is self-contained.

Run a script by bind-mounting the working directory:

```sh
docker run --rm -v "$PWD:/work" -w /work \
  luafan/luafan2-ubuntu:latest fan your_script.lua
```

Quick smoke test:

```sh
docker run --rm luafan/luafan2-ubuntu:latest \
  fan -e 'print(require("fan").loop and "ok" or "fail")'
```

### Building images locally

`build_docker.sh` wraps `docker build` for both flavours:

```sh
./build_docker.sh both              # builds ubuntu + alpine, tag :local
./build_docker.sh ubuntu            # only ubuntu
./build_docker.sh alpine            # only alpine
TAG=dev ./build_docker.sh ubuntu    # override tag
```

Design notes and CI/release workflow layout live in
[`manifest/ci-and-release-image.md`](manifest/ci-and-release-image.md).

## Status

- **M0 (infrastructure)**: DONE — single CMake build, C+Lua test frameworks,
  overflow-safe `bytearray`, ASan memory gate green on arm1/ARM64 (leaked=0).
- **M1 (runtime core)**: DONE — self-contained `fan` executable, libevent loop
  (`fan.loop`/`loopbreak`), coroutine `fan.sleep` park/resume + `fan.spawn`,
  R17/R20 lifetime invariant. arm1/ARM64: C 7/7, coro 6/6, util 9/9, leaked=0.
- **M2 (TCP + FIFO)**: DONE — `fan.tcp` (connect/bind/send/receive/close with
  drain-on-close), `fan.fifo` named-pipe IPC, URL `connector` (tcp://, fifo:///),
  evdns base wired into the loop. arm1/ARM64: tcp 5/5, fifo+connector 5/5, leaked=0.
- **M3 (UDP + DNS + reliable UDP)**: DONE — `fan.udp` (async IPv4/IPv6 sendto/recv,
  numeric `inet_pton`, no send TOCTOU, multicast join AND leave for IPv4+IPv6,
  wake-on-close), `fan.dns.resolve` (async evdns getaddrinfo, sync-callback safe,
  bounded per-query timeout), and `fan.reliable_udp` — a self-contained reliable
  message layer over `fan.udp`: fragmentation/reassembly, sliding send/recv window,
  cumulative + SACK acks, timeout retransmission, out-of-order/duplicate tolerance,
  window flow control, idle session sweep. arm1/ARM64: udp 8/8, dns 6/6,
  reliable_udp 8/8 (incl. 30%% loss recovery, reorder/dup, flow control), leaked=0.
- **M4 (TLS + HTTP + WebSocket + zlib)**: DONE.
  - TLS client — `net/tls.c` wraps `bufferevent_openssl_socket_new`;
    `fan.tcp.connect{ssl=true, verify_peer, verify_host}` reuses the tcp
    conn lifecycle. `fan.tls.available()` / `fan.tls.enabled`.
  - HTTP/1.1 client — `lua/fan/http.lua`, pure Lua on top of `fan.tcp`.
    `http.request{url,method,headers,query,body,follow_redirects,
    max_redirects,verify}` + `get/post/put/delete/head`. Chunked decoding,
    Content-Length, close-framed bodies, 301/302/303/307/308 redirects
    (303 + POST->GET body drop), per-request connection isolation, TLS
    via `https://`.
  - HTTP/1.1 server — `lua/fan/httpd.lua`, pure Lua on `fan.tcp.bind`.
    Request parsing (path/query/headers/body), one-shot `resp:reply`
    (auto Content-Length + `Connection: close`) and streaming
    `reply_start`/`reply_chunk`/`reply_end` (chunked TE). Handler-error
    contract: parse failure -> 400; handler crash -> 500; empty
    handler -> 204; unfinished chunked mode auto-terminates.
  - HTTPS server — TLS server-side bufferevent
    (`fan_tls_server_ctx_new`/`fan_tls_server_bev`, `BUFFEREVENT_SSL_ACCEPTING`)
    threaded through `fan.tcp.bind{ssl=true, cert, key}` and
    `fan.httpd.bind{ssl=..., cert=..., key=...}`. Critical fix in
    `conn_eventcb`: server-side CONNECTED events are ignored (only
    `l_connect`'s park expects one), preventing a spurious resume of
    the handler's `receive()`.
  - WebSocket (RFC 6455) — `lua/fan/websocket.lua`. Handshake (SHA-1 +
    base64 KAT-verified), masked client frames, unmasked server frames,
    7/7+16/7+64 extended length, TEXT/BINARY/PING/PONG/CLOSE. `ws:recv`
    joins fragments and auto-answers control frames; `ws:send*` are
    serialised across coroutines (covers "cross-thread send" on the
    single-loop model). `websocket.accept(req, resp)` integrates with
    `fan.httpd`.
  - zlib — `net/zlib_wrap.{h,c}` behind `FAN_WITH_ZLIB`. `fan.zlib.
    deflate_raw(data [, level [, sync]])` (Z_FINISH default, Z_SYNC_FLUSH
    opt-in) and `fan.zlib.inflate_raw(data)`. One-shot, no shared state.
  - **permessage-deflate** (RFC 7692) end-to-end: `accept` negotiates
    `permessage-deflate; server_no_context_takeover; client_no_context_takeover`
    when the peer offers it and zlib is compiled in. `send` compresses
    with Z_SYNC_FLUSH, strips the 00 00 FF FF marker, and sets RSV1.
    `recv` inverts the transform on RSV1 messages.
  - arm1/ARM64: 104 Lua cases across 11 suites + 7 C tests PASSED
    (tls 3/3, http-client 14/14, httpd 11/11, httpsd 6/6, websocket
    14/14 incl. permessage-deflate 3/3, zlib 9/9, and M0-M3 stable).
    --asan build: same, 0 ASan/LSan reports, leaked=0.
- **M5 (Codec + DB + ORM)**: DONE.
  - M5.1 fan.json (C) — `src/codec/json.c`. RFC 8259 encode/decode with
    array/object markers (`__jsontype` metatables), `fan.json.null`
    sentinel, surrogate-pair `\uXXXX` decode, sorted-key output,
    NaN/Inf and trailing-garbage rejection. 17/17 contract tests.
  - M5.2 fan.objectbuf (C) — `src/codec/objectbuf.c`. Compact binary
    serializer (tags 0x00..0x0c). Refid-based cycle handling: encoder
    assigns id before recursing, decoder registers table before descending;
    self / mutual cycles and shared subgraphs round-trip to reference-
    identical tables. Encode buffer is a GC-guarded userdata so a
    luaL_error mid-encode does not leak. 14/14 contract tests.
  - M5.3 fan.stream (C) — `src/codec/stream.c`. Userdata state machine:
    write buffer + read cursor. AddU/S 8/16/24/32, AddU30 VLQ (1/2/3/5-
    byte forms), AddBytes, AddString (u30-prefixed). Matching Get*
    readers return `nil, "eof"` on short reads. package / available /
    len / pos / reset introspection; `stream.new(str)` pre-fills for
    decoding. 10/10 contract tests.
  - arm1/ARM64: normal + --asan builds both pass; LSan-clean.
  - M5.4 fan.sqlite3 (C) + fan.orm base — `src/db/sqlite3.c` binds
    libsqlite3 (userdata for `sqlite3*` + `sqlite3_stmt*`). Methods:
    `open(path[,flags])`, `close` (idempotent), `exec`, `query` (row-map
    array), `prepare` -> `stmt:bind/bind_all/step/columns/reset/finalize`,
    `begin/commit/rollback`, `last_insert_rowid`, `changes`. Errors
    surface as `nil, "sqlite3: ..."`. Value marshalling: NULL<->nil,
    INTEGER<->lua integer, REAL<->lua number, TEXT/BLOB<->lua string.
    `lua/fan/orm.lua` layers a driver-agnostic ORM base:
    `ctx:define(table, schema)` runs `CREATE TABLE IF NOT EXISTS` and
    `ALTER TABLE ADD COLUMN` for any new field; models expose
    `insert / find_by / list{where,order,limit} / update / delete /
    raw_query`; `ctx:transaction(fn)` wraps `pcall` around
    `begin/commit`, rolling back on error. Gated by `FAN_WITH_SQLITE3`.
    7 sqlite3 + 5 orm contract tests.
  - M5.5.a fan.mariadb (C, synchronous) + ORM MariaDB driver —
    `src/db/mariadb.c` binds libmariadb (userdata for `MYSQL*` +
    `MYSQL_STMT*`). API: `connect{host,port,user,password,database,
    unix_socket,charset,autocommit}`, `close` (idempotent), `exec`,
    `query(sql, ...)` with `?`-placeholder expansion via
    `mysql_real_escape_string`, `prepare` -> `stmt:bind_all/step/
    columns/finalize` with per-column 4KB buffers + truncation refetch,
    `begin/commit/rollback`, `last_insert_id`, `affected_rows`, `ping`,
    `server_version`. Column type coercion INT/FLOAT/DECIMAL -> lua
    integer/number, everything else -> string. `lua/fan/orm.lua`
    adds a `mariadb_driver` adapter (backtick idents, BIGINT UNSIGNED
    PK AUTO_INCREMENT, `SHOW COLUMNS` migration). Gated by
    `FAN_WITH_MARIADB`. 8 mariadb + 3 orm_mariadb contract tests
    (skipped gracefully when no server). Cases include utf8mb4
    round-trip and SQL-injection escape checks.
  - M5.5.b fan.mariadb.pool (sync) — `lua/fan/mariadb/pool.lua` layers a
    simple synchronous connection pool over `fan.mariadb`: `pool.new{...}`
    returns `pool:get() / pool:put(db)` with a max-size soft cap; failed
    or already-closed connections are discarded on `put`. `pool:close_all()`
    synchronously mysql_close()es every idle conn (R13 sync-close path).
    5 pool contract tests including reuse, cap enforcement, and R13.
  - M5.5.c fan.mariadb async wait + R19 / R13 regressions —
    `src/db/mariadb.c` now enables `MYSQL_OPT_NONBLOCK` on every connection
    and adds a libevent-integrated `mysql_*_start / _cont` state machine:
    `db:query_async(sql, ...) / db:exec_async(sql, ...)` yield the calling
    coroutine while the query is in flight, resuming on
    `MYSQL_WAIT_READ / WRITE / EXCEPT / TIMEOUT` transitions (translated
    to `EV_READ / WRITE` + `mysql_get_timeout_value_ms` via
    `event_new / event_add`). Each `mdb_t` holds at most one in-flight
    `mdb_wait_t`; the second async op errors "one at a time". `push_result_
    rows` builds the row-map array on the coroutine's stack before
    `fan_coro_wake`. `l_close` and `db_gc` both invoke
    `cancel_pending_wait` which detaches from `owner`, pushes
    (nil, "mariadb: closed") to the parked coroutine, and frees the
    event before `mysql_close` (R19 during-flight cancel + R13
    defensive `__gc` idempotence). 7 async contract tests: sync-completion
    path, `?`-expansion in async, SELECT row-maps, exec `affected_rows`,
    bad-SQL nil-err, concurrent conns, R19 slow-query + close, R13 GC
    idempotence.
- **M6 (System + Worker + Pool)**: DONE.
  - M6.1 fan.posix — `src/sys/{posix.h,posix.c}` exposes POSIX process
    (getpid/fork/waitpid/kill/setpgid/getpgid/setsid), CPU
    (getcpucount/getaffinity/setaffinity), and network
    (getinterfaces IPv4+IPv6-correct netmask, setprogname Linux-only)
    services. v2 hardening: kill() refuses dangerous defaults (pid==0/-1/1)
    unless force=true, getinterfaces() uses sockaddr_in6 length for IPv6
    netmasks, setprogname() re-points __progname at a static 128-byte
    buffer (no environment corruption). fork() also invokes
    fan_loop_reinit_after_fork() so libevent state is safe in the child.
    `fan.posix.signals` and `fan.posix.wait` maps expose SIG* and W* flags.
    18 posix contract tests.
  - M6.2 fan.pool — `lua/fan/pool.lua` generic resource pool. LIFO idle
    stack, lazy factory, soft max-cap with coroutine-yield when
    exhausted, optional health() check on put(), with()-wrapper,
    close_all() teardown. Waiter queue is a FIFO; put() (or slot-free
    put(nil)) directly resumes the head with either the returned
    resource or a freshly created one. 9 pool contract tests.
  - M6.3 fan.worker — `lua/fan/worker.lua` process-based worker pool.
    Master forks N slaves, communicates via a per-slave TCP loopback
    socket, with 4-byte-length-prefix + fan.objectbuf framing.
    `w:call(name, ...)` picks the least-loaded live slave, parks the
    caller with coroutine.yield, and resumes on reply. `slaves == 0`
    collapses to inline direct dispatch. `w:terminate()` SIGTERMs every
    slave, WNOHANG-polls with fan.sleep, escalates to SIGKILL on holdout,
    and resumes any parked callers with (false, "slave terminated" /
    "slave dead"). 9 worker contract tests (inline + multi-process).
- **M7 (Hardening & release)**: in progress.
  - M7.1 fan.compat — `lua/fan/compat.lua`. Zero-argument, auto-apply
    v1 shim that re-exposes flat top-level POSIX helpers (fan.getpid,
    fan.fork, fan.kill, ...) as aliases into `fan.posix.*`. Idempotent.
    Preserves v2 kill() dangerous-PID guardrail. 4 contract tests.
  - M7.2 Cross-platform matrix: arm1 (ubuntu 22.04 aarch64 glibc)
    green under both normal + ASan builds; arm1 (alpine 3.16 aarch64
    musl) green under normal build (3 mariadb suites skip when no
    mysqld). 26 test suites total, leaked=0. x86_64 pending.
  - M7.3 manifest: capability docs live under `manifest/` (see
    `manifest/MANIFEST.md`).
  - Remaining: performance regression baseline, x86_64 platform, TSan
    (not applicable to v2 method-A: workers are fork()'d processes,
    single event loop per process).
