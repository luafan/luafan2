# CI and release Docker images (M14.F)

Two GitHub Actions workflows and two release Dockerfiles that together
run the test matrix on every push/PR and publish thin runtime images to
Docker Hub once main is green.

## Image inventory

| File                          | Image tag                       | Purpose               | Size (typ.) |
|-------------------------------|---------------------------------|-----------------------|-------------|
| `tests/Dockerfile`            | `luafan2-ci:local`              | CI + local test image | ~450 MB     |
| `Dockerfile.release.ubuntu`   | `luafan/luafan2-ubuntu:<tag>`   | Runtime, glibc        | ~60 MB      |
| `Dockerfile.release.alpine`   | `luafan/luafan2-alpine:<tag>`   | Runtime, musl         | ~18 MB      |

`tests/Dockerfile` is a **fat** image: build toolchain, `mariadb-server`,
`luarocks`, `luacov`, `lcov`, `gdb`/`valgrind`. It is used by
`./tests/run_tests.sh` and by the CI matrix. Release images are strictly
runtime — no compilers, no headers, no rocks. See
`manifest/coverage.md` for the CI image's role in the three test modes.

Both release images are multi-stage:
1. **builder** — full `-dev` packages (apt on ubuntu, apk on alpine) →
   `cmake --build --target fan -DCMAKE_BUILD_TYPE=Release` → `strip`.
2. **runtime** — stock base image (`ubuntu:22.04` / `alpine:3.20.10`) +
   the exact runtime `.so` packages that match `ldd fan`. `fan` binary
   installed to `/usr/local/bin/fan`, Lua modules to
   `/usr/local/share/lua/5.3/`. `ENTRYPOINT = ["/usr/local/bin/fan"]`,
   `WORKDIR = /work`.

Every release image build ends with an in-image smoke test that runs
`fan -v` and `require`s the full v2 module surface
(`fan`, `fan.compat`, `fan.utils`, `fan.log`, `fan.json`,
`fan.objectbuf`, `fan.stream`, `fan.httpd`, `fan.http`,
`fan.websocket`, `fan.orm`, `fan.pool`, `fan.worker`, `fan.popen`,
`fan.upnp`, `fan.reliable_udp`, `fan.connector`, `fan.config`, top-level
`config`). If any require chain breaks because a runtime `.so` is
missing, the image build fails there rather than shipping broken.

### Alpine musl notes

Alpine 3.20 runs against musl 1.2.5 and OpenSSL 3.3.x. Known v2
adaptations (already in-tree):

- `setprogname()` — Linux-glibc-only; `src/sys/posix.c` uses a
  nil-returning stub gated by a runtime `__progname` probe.
- `sysconf(_SC_OPEN_MAX)` replaces `getdtablesize(2)` (v1 macro path).
- OpenSSL 1.1.1 / 3.x are both handled by the shim in `src/net/tls.c`.
- `<sys/queue.h>` — musl does not ship the BSD queue header;
  `src/net/httpd.c` uses `TAILQ_INIT`. `Dockerfile.release.alpine`
  builder stage installs `libbsd-dev` to get the NetBSD-compat
  header. Headers-only — the macros expand at compile time, so the
  runtime image does not link against `libbsd`.
- Alpine 3.20's `libevent` main package now includes the openssl and
  pthreads adapter shared libraries (`libevent_openssl-2.1.so.7`,
  `libevent_pthreads-2.1.so.7`); the standalone `libevent-openssl`
  and `libevent-pthreads` subpackages that existed on 3.16 no longer
  exist. Runtime `apk add` lists `libevent` only.
- `lua5.3-dev` on alpine installs the linker symlink at
  `/usr/lib/lua5.3/liblua.so` (subdir) rather than
  `/usr/lib/liblua5.3.so`. `CMakeLists.txt` `find_library` includes
  `lua5.3` / `lua5.4` in `PATH_SUFFIXES` for this reason.

Why alpine 3.20 (not 3.16)? Alpine 3.16 hit EOL 2024-05 and by 2025+
its apk repo had dropped `libssl1.1` / `libcrypto1.1`, so
`apk add libssl1.1 libcrypto1.1` failed with "no such package".
3.20 is the current LTS through 2026-04 and ships openssl 3.3.x —
same major line as ubuntu 22.04, exercised by the same OpenSSL-3
shim in `src/net/tls.c`.

Alpine base image is pinned by patch (`alpine:3.20.10`), matching
v1's pinning style (v1 uses `alpine:3.16.9`).

ASan is **not** exercised on Alpine — LSan has known false-positives on
musl around global-TLS destructors. CI only runs ASan on the
glibc/ubuntu leg. Alpine coverage: normal mode 508/508 green on arm1
aarch64 (see M14.E status).

Verified end-to-end on arm1 (native aarch64, docker 29.5, buildkit
v0.30) via `docker build --no-cache --pull -f Dockerfile.release.alpine`
and `docker pull luafan/luafan2-alpine:latest` → `docker run … -v` →
`LuaFan 2.0.0-dev (Lua 5.3.6)`; module surface smoke passes; final
image size 18.4 MB.

## Workflows

### `.github/workflows/ci.yml` — CI

Trigger: `push` to `main`, `pull_request` targeting `main`.
`concurrency: ci-${{ github.ref }}` with `cancel-in-progress: true`, so
pushing a fixup on top of a still-running CI won't keep two matrixes
in flight.

Two-phase layout:

1. **`build-image`** — builds `tests/Dockerfile` once via
   `docker/build-push-action@v6`, exports the image as a tar
   (`type=docker,dest=/tmp/luafan2-ci.tar`), uploads as an artifact,
   caches to GHA (`scope=ci-image`). ~30s cold, near-instant warm.
2. **`test`** matrix `[normal, asan, coverage]` — each job downloads
   the tar, `docker load`s it, and runs
   `./tests/run_tests.sh [--asan | --coverage]`. `PER_TEST_TIMEOUT`
   is 180s normally, 300s for coverage (gcov-instrumented builds are
   ~2x slower on shared GHA runners).

Coverage is **soft-warn** at this milestone — no `--enforce`, no CI
failure on threshold miss. C sits at 84.8% vs 85% floor. Once that
closes we flip `--enforce`. Coverage HTML + summary
(`build-coverage/coverage-html/`, `summary.txt`,
`luacov.report.out`, `manifest/coverage-latest.md`) upload as an
artifact with 30d retention.

### `.github/workflows/release-image.yml` — Release Image

Triggers:
- `workflow_run` on completion of `CI` on `main`. The publish jobs
  gate on `conclusion == 'success' && event == 'push'` so PR CI reruns
  never publish.
- `workflow_dispatch` — manual re-publish button (e.g. bake a fresh
  runtime image after a base-image CVE).
- `push` of tags matching `v*` — cuts a `:vX.Y.Z` tag.

Layout: 4 parallel-ish jobs per flavour × 2 flavours = 4 jobs.

For each of `ubuntu` and `alpine`:

- **`build-<flavour>`** matrix `{amd64, arm64}` on **native** runners
  (`ubuntu-latest` for amd64, `ubuntu-24.04-arm` for arm64). Each
  job builds `Dockerfile.release.<flavour>` and pushes **by digest**
  (`push-by-digest=true`, `name-canonical=true`); no human-facing tag
  yet. Digest is exported as an empty file to
  `/tmp/digests/<hex>` and uploaded as an artifact
  (`digests-<flavour>-<arch>`). Build runs with `no-cache: true` +
  `pull: true` (cold-build, matching v1's `docker-push.yml`).
  Release images are infrequent post-CI events; a full rebuild each
  time is safer than risking GHA cache poisoning across base-image
  bumps. CI (`ci.yml`) still caches its per-commit builds.
- **`merge-<flavour>`** downloads both digests, runs
  `docker buildx imagetools create` to assemble a multi-arch manifest,
  and pushes under the real tags:
  - `luafan/luafan2-<flavour>:latest`
  - `luafan/luafan2-<flavour>:<sha>`  — always
  - `luafan/luafan2-<flavour>:<ref_name>`  — only when trigger is a
    `v*` tag push
  Then `imagetools inspect luafan/luafan2-<flavour>:latest` for a
  visible smoke line in the run log.

The multi-arch strategy mirrors v1's `docker-push.yml`. Native
runners avoid QEMU cross-build's ~5-10x arm64 slowdown; the trade-off
is that we serialise `build → merge` per flavour instead of one
buildx run doing both platforms.

For `workflow_run` triggers, both `build` and `merge` jobs check out
`github.event.workflow_run.head_sha` — the exact commit CI validated,
not HEAD of main (which could have moved between CI success and
release-image start).

## Secrets

Repository secrets required (already configured on
`github.com/luafan/luafan2`):

| Secret               | Purpose                                                 |
|----------------------|---------------------------------------------------------|
| `DOCKERHUB_USERNAME` | Docker Hub account with push access to both `luafan/`   |
|                      | image repos                                             |
| `DOCKERHUB_TOKEN`    | Docker Hub *access token* (not the account password);   |
|                      | scope `Read, Write, Delete` on the two image repos      |

CI workflow does **not** need either secret — it only builds inside
the workflow and never pushes. Release workflow logs into Docker Hub
with `docker/login-action@v3`.

## Local build shortcut

`build_docker.sh` wraps `docker build` for the two release
Dockerfiles:

```sh
./build_docker.sh both              # ubuntu + alpine, tag :local
./build_docker.sh ubuntu            # only ubuntu
./build_docker.sh alpine            # only alpine
TAG=dev ./build_docker.sh ubuntu    # override tag suffix
```

Tags produced locally: `luafan/luafan2-ubuntu:<TAG>` and/or
`luafan/luafan2-alpine:<TAG>` (default `TAG=local`). Useful for
smoke-testing a Dockerfile change on arm1 before pushing to CI.

## Files

- `.github/workflows/ci.yml`
- `.github/workflows/release-image.yml`
- `Dockerfile.release.ubuntu`
- `Dockerfile.release.alpine`
- `.dockerignore`
- `build_docker.sh`
- `tests/Dockerfile` (unchanged; CI image, M7.5)
