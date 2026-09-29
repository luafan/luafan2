#!/bin/sh
# run_tests.sh — LuaFan v2 test driver.
#
# Runs entirely inside the `luafan2-ci:local` Docker image so the build
# environment matches CI and doesn't depend on the host's toolchain.
# Every mode (normal / --asan / --coverage) is one Docker invocation.
#
# Usage:
#   ./run_tests.sh                 # normal build, C + Lua tests
#   ./run_tests.sh --asan          # AddressSanitizer + LeakSanitizer memory gate
#   ./run_tests.sh --coverage      # gcov + luacov, soft-warn on threshold miss
#   ./run_tests.sh --coverage --enforce   # coverage + hard-fail on miss
#
# The image is built on first use (or when the Dockerfile changes and you
# `docker rmi luafan2-ci:local` first). Rebuild is fast (apt cache is
# inside the image; only network on first pull of ubuntu:22.04).
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"

# ---- CLI ------------------------------------------------------------------
MODE="normal"
ENFORCE=0
# Per-test wall-clock guard. Every Lua test file is invoked under
# `timeout $PER_TEST_TIMEOUT` so a single hung case (coverage-mode races
# in libcurl / libevent / luacov instrumentation have shown up before)
# can't lock the whole run for tens of minutes. Override via env:
#   PER_TEST_TIMEOUT=300 ./run_tests.sh --coverage
# Coverage runs get a larger default because gcov-instrumented builds
# are 2-3x slower and heavy tests (test_httpsd, test_http_c) run near
# the ceiling.
: "${PER_TEST_TIMEOUT:=}"
for arg in "$@"; do
    case "$arg" in
        --asan)     MODE="asan"     ;;
        --coverage) MODE="coverage" ;;
        --enforce)  ENFORCE=1       ;;
        -h|--help)
            sed -n '2,17p' "$0"
            exit 0
            ;;
        *)
            echo "run_tests.sh: unknown argument '$arg'" >&2
            exit 2
            ;;
    esac
done

# Mode-specific default when the caller didn't override.
if [ -z "$PER_TEST_TIMEOUT" ]; then
    case "$MODE" in
        coverage) PER_TEST_TIMEOUT=300 ;;
        asan)     PER_TEST_TIMEOUT=180 ;;
        *)        PER_TEST_TIMEOUT=120 ;;
    esac
fi
export PER_TEST_TIMEOUT

# ---- Docker image ---------------------------------------------------------
IMAGE="luafan2-ci:local"
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "=== building $IMAGE (first run) ==="
    docker build -f "$HERE/Dockerfile" -t "$IMAGE" "$ROOT"
fi

# ---- Run inside container -------------------------------------------------
#
# Mount the project at /work; build artefacts land under /work/build*, so
# they survive `docker run` and stay browsable on the host afterwards.
# We generate a small shell recipe per mode and pipe it into a fresh
# container. Keeping the recipe here (vs. shelling out to a separate
# script inside the image) makes the flow obvious in one file.
#
# We run as **root** inside the container so the entrypoint can start
# MariaDB (mariadbd needs to `chown` sockets / read /etc/mysql, and
# `service mariadb start` requires privilege). Build artefacts under
# /work/build* end up root-owned on the host, which is fine: build-*/
# is in .gitignore and only the CI/coverage flow writes there.
#
# The `-t` flag would allocate a TTY (nicer output), but CI runs are
# headless — the plain `-i` here works both interactively and under CI.
if [ -t 1 ]; then TTY_FLAG="-t"; else TTY_FLAG=""; fi

DOCKER_RUN="docker run --rm -i $TTY_FLAG \
    -v $ROOT:/work \
    -w /work \
    -e HOME=/work \
    -e PER_TEST_TIMEOUT=$PER_TEST_TIMEOUT \
    $IMAGE"

case "$MODE" in
    normal)
        echo "=== normal build (Docker: $IMAGE) ==="
        $DOCKER_RUN /bin/sh -c '
            set -eu
            BUILD=/work/build
            cmake -S /work -B "$BUILD" \
                -DCMAKE_BUILD_TYPE=Debug \
                -DFAN_WITH_OPENSSL=ON -DFAN_WITH_ZLIB=ON \
                -DFAN_WITH_CURL=ON \
                -DFAN_WITH_SQLITE3=ON -DFAN_WITH_MARIADB=ON \
                >/dev/null
            cmake --build "$BUILD" -j2 >/dev/null
            echo "build ok: $BUILD"
            echo
            echo "=== C unit tests ==="
            "$BUILD/run_c_tests"
            echo
            echo "=== Lua tests (via ./fan) ==="
            export LUA_PATH="/work/lua/?.lua;/work/lua/?/init.lua;/work/tests/lua/framework/?.lua;/work/tests/lua/?.lua;;"
            LUA_RC=0
            HUNG=""
            for t in /work/tests/lua/test_*.lua; do
                [ -f "$t" ] || continue
                echo "--- $(basename "$t") ---"
                # timeout --kill-after=5s escalates SIGTERM -> SIGKILL 5s
                # later if the target ignores TERM (libevent loop, luacov
                # hook). Exit code 124 = coreutils "timed out".
                FAN_BIN="$BUILD/fan" timeout --kill-after=5s "${PER_TEST_TIMEOUT}s" \
                    "$BUILD/fan" "$t"
                rc=$?
                if [ $rc -eq 124 ]; then
                    echo "!!! $(basename "$t"): TIMED OUT after ${PER_TEST_TIMEOUT}s"
                    HUNG="$HUNG $(basename "$t")"
                    LUA_RC=1
                elif [ $rc -ne 0 ]; then
                    LUA_RC=1
                fi
            done
            if [ -n "$HUNG" ]; then
                echo "!!! HUNG SUITES:$HUNG"
            fi

            # ---- M15: cross-interpreter fan.so smoke test ----
            # Verifies the second half of the dual-form delivery promise:
            # build/fan.so can be dlopen()ed by ANY stock Lua interpreter
            # via require("fan"), not just our own thin `fan` executable.
            # If this ever regresses (e.g. someone reintroduces a symbol
            # that only fan.c exports, or fan_module drops liblua from its
            # link line and macOS-only builds keep working while Linux
            # breaks), the failure is caught here rather than at deploy.
            # The actual asserts live in tests/lua_smoke_fan_so.lua so we
            # can keep the smoke code out of this shell heredoc entirely.
            echo
            echo "=== fan.so cross-interpreter smoke (stock lua5.3) ==="
            if command -v lua5.3 >/dev/null 2>&1; then
                LUA_CPATH="$BUILD/?.so;;" \
                LUA_PATH="/work/lua/?.lua;/work/lua/?/init.lua;;" \
                    lua5.3 /work/tests/lua_smoke_fan_so.lua
                SMOKE_RC=$?
                if [ $SMOKE_RC -ne 0 ]; then
                    echo "!!! fan.so cross-interpreter smoke FAILED (rc=$SMOKE_RC)"
                    LUA_RC=1
                fi
            else
                echo "(skip: /usr/bin/lua5.3 not present in this image)"
            fi

            exit $LUA_RC
        '
        ;;

    asan)
        echo "=== ASan/LSan build (Docker: $IMAGE) ==="
        $DOCKER_RUN /bin/sh -c '
            set -eu
            BUILD=/work/build-asan
            cmake -S /work -B "$BUILD" \
                -DCMAKE_BUILD_TYPE=Debug -DLUAFAN2_ASAN=ON \
                -DFAN_WITH_OPENSSL=ON -DFAN_WITH_ZLIB=ON \
                -DFAN_WITH_CURL=ON \
                -DFAN_WITH_SQLITE3=ON -DFAN_WITH_MARIADB=ON \
                >/dev/null
            cmake --build "$BUILD" -j2 >/dev/null
            echo "build ok: $BUILD"
            echo
            echo "=== C unit tests (ASan) ==="
            ASAN_OPTIONS="detect_leaks=1" "$BUILD/run_c_tests"
            echo
            echo "=== Lua tests (via ./fan under ASan) ==="
            export LUA_PATH="/work/lua/?.lua;/work/lua/?/init.lua;/work/tests/lua/framework/?.lua;/work/tests/lua/?.lua;;"
            LUA_RC=0
            HUNG=""
            for t in /work/tests/lua/test_*.lua; do
                [ -f "$t" ] || continue
                echo "--- $(basename "$t") ---"
                ASAN_OPTIONS="detect_leaks=1" FAN_BIN="$BUILD/fan" \
                    timeout --kill-after=5s "${PER_TEST_TIMEOUT}s" \
                    "$BUILD/fan" "$t"
                rc=$?
                if [ $rc -eq 124 ]; then
                    echo "!!! $(basename "$t"): TIMED OUT after ${PER_TEST_TIMEOUT}s"
                    HUNG="$HUNG $(basename "$t")"
                    LUA_RC=1
                elif [ $rc -ne 0 ]; then
                    LUA_RC=1
                fi
            done
            if [ -n "$HUNG" ]; then
                echo "!!! HUNG SUITES:$HUNG"
            fi
            exit $LUA_RC
        '
        ;;

    coverage)
        echo "=== coverage build (Docker: $IMAGE, enforce=$ENFORCE) ==="
        # NB: coverage builds are separate from normal/asan on purpose —
        # gcov instrumentation clashes with ASan (see CMakeLists guard) and
        # reruns need clean .gcda files, so build-coverage/ is wiped every
        # time. Small price; keeps the numbers honest.
        $DOCKER_RUN /bin/sh -c "
            set -eu
            BUILD=/work/build-coverage
            rm -rf \"\$BUILD\"
            mkdir -p \"\$BUILD\"
            cmake -S /work -B \"\$BUILD\" \
                -DCMAKE_BUILD_TYPE=Debug -DFAN_COVERAGE=ON \
                -DFAN_WITH_OPENSSL=ON -DFAN_WITH_ZLIB=ON \
                -DFAN_WITH_CURL=ON \
                -DFAN_WITH_SQLITE3=ON -DFAN_WITH_MARIADB=ON \
                >/dev/null
            cmake --build \"\$BUILD\" -j2 >/dev/null
            echo \"build ok: \$BUILD\"
            echo
            echo '=== C unit tests (coverage) ==='
            \"\$BUILD/run_c_tests\"
            echo
            echo '=== Lua tests (via ./fan with luacov) ==='
            export LUA_PATH='/work/lua/?.lua;/work/lua/?/init.lua;/work/tests/lua/framework/?.lua;/work/tests/lua/?.lua;;'
            export LUAFAN_COVERAGE=1
            LUA_RC=0
            HUNG=\"\"
            for t in /work/tests/lua/test_*.lua; do
                [ -f \"\$t\" ] || continue
                echo \"--- \$(basename \"\$t\") ---\"
                FAN_BIN=\"\$BUILD/fan\" \\
                    timeout --kill-after=5s \"\${PER_TEST_TIMEOUT}s\" \\
                    \"\$BUILD/fan\" -e 'require \"coverage\"' \"\$t\"
                rc=\$?
                if [ \$rc -eq 124 ]; then
                    echo \"!!! \$(basename \"\$t\"): TIMED OUT after \${PER_TEST_TIMEOUT}s\"
                    HUNG=\"\$HUNG \$(basename \"\$t\")\"
                    LUA_RC=1
                elif [ \$rc -ne 0 ]; then
                    LUA_RC=1
                fi
            done
            if [ -n \"\$HUNG\" ]; then
                echo \"!!! HUNG SUITES:\$HUNG\"
            fi
            echo
            /bin/sh /work/tests/coverage_summary.sh \"\$BUILD\" $ENFORCE
            SUM_RC=\$?
            # Propagate the strictest failure (Lua test failure > coverage
            # threshold miss); explicit exits so a subshell doesn't reset RC.
            if [ \$LUA_RC -ne 0 ]; then exit 1; fi
            if [ \$SUM_RC -ne 0 ]; then exit \$SUM_RC; fi
            exit 0
        "
        ;;
esac
