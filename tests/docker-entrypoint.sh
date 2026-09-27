#!/bin/sh
# docker-entrypoint.sh — start MariaDB (background), wait for its unix
# socket, then exec the caller's command.
#
# Runs as root inside the container. Every `docker run luafan2-ci:local
# ...` flows through this. The whole point is that tests can `connect
# via /run/mysqld/mysqld.sock, user=root, password=""` unconditionally.
#
# Environment knobs (all optional):
#   SKIP_MARIADB=1          skip the daemon (useful for smoke shells)
#   MARIADB_WAIT_SEC=15     max seconds to wait for the socket (default 15)
set -eu

if [ "${SKIP_MARIADB:-0}" != "1" ]; then
    # Ensure /run/mysqld exists (tmpfs on some hosts wipes it between
    # container starts). Ownership is baked in the image; recreate if
    # it disappeared (harmless when it already exists).
    install -d -o mysql -g mysql -m 0755 /run/mysqld

    # Fire MariaDB in the background. We drop stdout/stderr into a log
    # file so `docker run -t` isn't drowned in server chatter, but keep
    # errors visible if the daemon refuses to come up.
    mariadbd --user=mysql --skip-networking=1 \
             --socket=/run/mysqld/mysqld.sock \
             >/var/log/mysql/mariadbd.log 2>&1 &
    MARIADB_PID=$!

    # Wait for the socket. mariadb-admin ping is more decisive than a
    # bare `test -S` because it also verifies the server accepts a
    # connection (not just that the socket file exists).
    WAIT_SEC=${MARIADB_WAIT_SEC:-15}
    i=0
    until mariadb-admin --socket=/run/mysqld/mysqld.sock --user=root ping >/dev/null 2>&1; do
        i=$((i + 1))
        if [ "$i" -ge "$((WAIT_SEC * 10))" ]; then
            echo "docker-entrypoint: MariaDB failed to start within ${WAIT_SEC}s" >&2
            echo "--- tail of /var/log/mysql/mariadbd.log ---" >&2
            tail -20 /var/log/mysql/mariadbd.log >&2 || true
            kill "$MARIADB_PID" 2>/dev/null || true
            exit 1
        fi
        # 100ms polling: fast enough that a healthy start (~2-3s on ARM
        # server-class hardware, ~500ms on x86 dev boxes) is uncapped.
        sleep 0.1
    done
fi

# Hand off to whatever `docker run` asked for (default is /bin/bash from
# the image CMD). `exec` so signals reach the caller cleanly.
exec "$@"
