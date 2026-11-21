#!/usr/bin/env bash
# Builds the Vita kernel plugin (VitaSync.skprx) on Linux with the vitasdk
# Docker image; the Linux twin of build-vita.ps1. Extra arguments go to make,
# e.g. ./build-vita.sh DEBUG=1 or ./build-vita.sh DISPLAY_OFF_OLED=1.
set -euo pipefail

cd "$(dirname "$0")"

IMAGE="vitasdk/vitasdk:2026.08"

docker run --rm \
    -v "$PWD/VitaSync:/src" \
    -w /src \
    --user "$(id -u):0" \
    "$IMAGE" \
    bash -c 'make clean "$@" && make "$@"' make "$@"

echo "Build complete: VitaSync/VitaSync.skprx"
