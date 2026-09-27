#!/bin/sh
# build_docker.sh — build the LuaFan v2 release Docker image(s) locally.
#
# By default builds BOTH ubuntu and alpine variants tagged luafan/luafan2-*:local.
# CI publishes to Docker Hub as luafan/luafan2-ubuntu:<tag> and
# luafan/luafan2-alpine:<tag> — see .github/workflows/release-image.yml.
#
# Usage:
#   ./build_docker.sh                # both variants, tag :local
#   ./build_docker.sh ubuntu         # only ubuntu
#   ./build_docker.sh alpine         # only alpine
#   TAG=dev ./build_docker.sh        # override :local tag
#
# The context is the current directory. .dockerignore trims what gets
# sent to the daemon (no tests/, no build/, no manifest/).
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

TAG="${TAG:-local}"
TARGET="${1:-both}"

build_one() {
    variant="$1"
    dockerfile="Dockerfile.release.${variant}"
    image="luafan/luafan2-${variant}:${TAG}"
    if [ ! -f "$dockerfile" ]; then
        echo "build_docker.sh: missing $dockerfile" >&2
        exit 2
    fi
    echo "=== building $image ($dockerfile) ==="
    docker build -f "$dockerfile" -t "$image" .
    echo "--- $image ready ---"
    docker image inspect --format='  size:  {{.Size}} bytes' "$image"
    docker image inspect --format='  arch:  {{.Architecture}}' "$image"
}

case "$TARGET" in
    both)
        build_one ubuntu
        build_one alpine
        ;;
    ubuntu|alpine)
        build_one "$TARGET"
        ;;
    -h|--help)
        sed -n '2,17p' "$0"
        exit 0
        ;;
    *)
        echo "build_docker.sh: unknown target '$TARGET' (want: both|ubuntu|alpine)" >&2
        exit 2
        ;;
esac
