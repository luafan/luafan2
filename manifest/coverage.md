# Coverage tooling (M7.5)

`tests/Dockerfile` + `tests/run_tests.sh` + `tests/coverage_summary.sh`
implement a single-image test/CI pipeline that supports three modes.

## Image

`luafan2-ci:local`, built from `tests/Dockerfile` on top of
`ubuntu:22.04`. Preinstalled tools:

- Toolchain: `build-essential cmake pkg-config git`
- Lua: `liblua5.3-dev lua5.3 luarocks`
- Runtime deps: `libevent-dev libevent-pthreads libevent-openssl`,
  `libssl-dev zlib1g-dev libsqlite3-dev libmariadb-dev libcurl4-openssl-dev`
- Database: `mariadb-server` (Ubuntu default `auth_socket` — tests
  connect as root via `/run/mysqld/mysqld.sock` with no password)
- Coverage: `lcov` (apt) + `luacov` (luarocks)

`./fan` links stock `liblua5.3`, whose default `package.path` includes
`/usr/local/share/lua/5.3/?.lua`, so `require "luacov"` from `./fan`
finds the luarocks-installed module without extra plumbing.

## Entrypoint

`tests/docker-entrypoint.sh` (baked into the image as
`/usr/local/bin/docker-entrypoint.sh`) starts `mariadbd` in the
background and waits for `/run/mysqld/mysqld.sock` to accept
connections before exec'ing the caller's command. Every mode
(`normal`, `--asan`, `--coverage`) gets MariaDB automatically.

- `SKIP_MARIADB=1` in `docker run` skips the daemon (fast smoke shells).
- `MARIADB_WAIT_SEC=<n>` extends the readiness wait (default 15s).

The container runs as **root** so the entrypoint can operate on
`/run/mysqld/` and `mariadbd` can bind its socket. Build artefacts
under `/work/build*` therefore land root-owned on the host; that's
fine because `build-*/` is gitignored and only the CI/coverage flow
writes there. Deleting them by hand needs `sudo`.

## Modes

`tests/run_tests.sh` always runs inside the image; there is no host
build path.

| Command                              | CMake option           | Purpose                          |
|--------------------------------------|------------------------|----------------------------------|
| `./run_tests.sh`                     | (none)                 | Normal build, C + Lua tests      |
| `./run_tests.sh --asan`              | `-DLUAFAN2_ASAN=ON`    | AddressSanitizer memory gate     |
| `./run_tests.sh --coverage`          | `-DFAN_COVERAGE=ON`    | gcov + luacov, soft-warn         |
| `./run_tests.sh --coverage --enforce`| `-DFAN_COVERAGE=ON`    | Same, hard-fail on threshold miss|

The three modes are mutually exclusive because `-fsanitize=address`
does not compose with `-fprofile-arcs` (CMake enforces this).

## Coverage collection

- **C**: CMake adds `--coverage -O0 -g` when `FAN_COVERAGE=ON`. `.gcno`
  files land alongside object files under `build-coverage/`; `.gcda`
  files are written by the instrumented binaries during test runs.
  `tests/coverage_summary.sh` calls `lcov --capture` + filters, then
  `genhtml` writes `build-coverage/coverage-html/`.
- **Lua**: `tests/lua/framework/coverage.lua` gates on
  `LUAFAN_COVERAGE=1` and calls `luacov.runner.init()`. The luacov
  debug hook records executed lines per process; luacov's on-exit
  handler flushes to `build-coverage/luacov.stats.out`. Because each
  `test_*.lua` runs in its own `./fan` process, stats append across
  tests. `.luacov` at the project root supplies include/exclude
  patterns (only `lua/fan/**` counts).

## Thresholds

Line coverage (branch coverage is captured but not enforced):

- C:   ≥ 85%
- Lua: ≥ 90%

Under `--coverage` (no `--enforce`), threshold misses print a warning
and the script exits 0. Under `--coverage --enforce`, threshold misses
exit with code 1 — this is the CI/release gate.

## Artefacts per run

```
build-coverage/
  coverage.info            filtered lcov data (input to genhtml)
  coverage.info.raw        unfiltered capture (kept for debugging)
  coverage-html/           genhtml HTML report (open index.html)
  luacov.stats.out         luacov raw stats
  luacov.report.out        luacov plain-text report
  summary.txt              short human-readable numbers
```

`manifest/coverage-latest.md` is refreshed on every run with the two
percentages and a UTC timestamp. That file **is** committed so history
shows coverage drift.

## Exclusions

- `LCOV_EXCL_LINE` / `LCOV_EXCL_START` / `LCOV_EXCL_STOP` markers may
  be used sparingly for **untestable** paths (OOM in `malloc`, `fork`
  failures, syscall corner cases). Every use MUST have an adjacent
  comment explaining why.
- Test framework files (`tests/lua/framework/`) are excluded from Lua
  coverage. C tests (`tests/c/`) are excluded from C coverage.
