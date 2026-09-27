# luafan2/tests/Dockerfile
# ---------------------------------------------------------------------------
# Single test/CI image (`luafan2-ci:local`). One image drives every mode:
#   run_tests.sh              -> normal build inside this image
#   run_tests.sh --asan       -> AddressSanitizer + LeakSanitizer
#   run_tests.sh --coverage   -> gcov (C) + luacov (Lua) instrumentation
#
# Design goals:
#   - stock ubuntu:22.04 base (no dependency on downstream site images);
#   - everything to build & test `./fan` and every optional feature flag
#     (openssl / zlib / sqlite3 / mariadb / curl) available out of the box;
#   - coverage tooling (lcov + luacov) preinstalled so `./fan test.lua`
#     under LUAFAN_COVERAGE=1 can `require "luacov"` without further setup.
#
# Build:
#   docker build -f tests/Dockerfile -t luafan2-ci:local .
# ---------------------------------------------------------------------------
FROM ubuntu:22.04

# Non-interactive apt; keep the image reproducible.
ENV DEBIAN_FRONTEND=noninteractive \
    LC_ALL=C.UTF-8 \
    LANG=C.UTF-8

# ---- Toolchain + Lua + libevent + optional feature libs + coverage --------
#
# Grouped for review clarity, one `apt-get install` for a single layer.
#
# Toolchain:      build-essential cmake pkg-config git ca-certificates
# Lua:            liblua5.3-dev lua5.3 luarocks
#                   - `./fan` links liblua5.3 at build time; `lua5.3` CLI is
#                     handy for luarocks and quick sanity checks (not used
#                     by our tests, which always run under `./fan`).
# Runtime deps:   libevent-dev libevent-pthreads-2.1-7 libevent-openssl-2.1-7
#                   - openssl/zlib/sqlite3/mariadb/curl -dev packages so all
#                     FAN_WITH_* feature switches build without extra setup.
# Coverage:       lcov  (`genhtml` ships in this package)
#                   - luacov comes from luarocks below.
# Debug/diag:     gdb strace ltrace valgrind
#                   - preinstalled so an AI or human debugger can attach
#                     to a running ./fan without needing to `apt-get install`
#                     mid-run. All under a few MB apiece; harmless in the CI
#                     image because they never execute unless invoked.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        git \
        ca-certificates \
        liblua5.3-dev \
        lua5.3 \
        luarocks \
        libevent-dev \
        libevent-pthreads-2.1-7 \
        libevent-openssl-2.1-7 \
        libssl-dev \
        zlib1g-dev \
        libsqlite3-dev \
        libmariadb-dev \
        libcurl4-openssl-dev \
        lcov \
        mariadb-server \
        gdb \
        strace \
        ltrace \
        valgrind \
    && rm -rf /var/lib/apt/lists/*

# Prime the MariaDB data directory once at build time. Ubuntu's default
# post-install already runs `mysql_install_db` under `mysql:mysql`; we
# just make sure the runtime directory exists so `mariadbd` can create
# its unix socket at /run/mysqld/mysqld.sock when the container starts.
RUN install -d -o mysql -g mysql -m 0755 /run/mysqld /var/log/mysql

# Provide a tiny entrypoint that starts MariaDB (background) and waits
# for the socket before handing control to the caller's command. Tests
# connect via /run/mysqld/mysqld.sock as root without a password, which
# is Ubuntu/Debian's default auth_socket setup — no extra grants needed.
COPY tests/docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

# Lua coverage: install once so `./fan` (which links stock liblua5.3 and
# therefore uses the default LUA_PATH of /usr/local/share/lua/5.3/?.lua)
# can `require "luacov"` without any extra path plumbing.
RUN luarocks install --tree=/usr/local luacov \
    && test -f /usr/local/share/lua/5.3/luacov.lua

# The scripts mount the workspace at /work and run there. Not a fixed
# COPY-in: keeping the image workspace-agnostic makes it reusable across
# checkouts and CI runners without a rebuild.
WORKDIR /work

# Sanity: fail fast at image build time if any critical tool is missing.
RUN gcc --version >/dev/null \
    && cmake --version >/dev/null \
    && lua5.3 -v \
    && lcov --version | head -1 \
    && genhtml --version | head -1 \
    && mariadbd --version | head -1 \
    && gdb --version | head -1 \
    && strace --version | head -1 \
    && valgrind --version

# Every container invocation flows through docker-entrypoint.sh, which
# starts MariaDB in the background, waits for its unix socket, then
# exec's the caller's command (`/bin/bash` by default, or whatever
# run_tests.sh passes via `docker run ... $CMD`). Users who explicitly
# don't want MariaDB can override with `--entrypoint /bin/bash`.
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD ["/bin/bash"]
