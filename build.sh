#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"

if [[ -n "${JOBS:-}" ]]; then
    PARALLEL="${JOBS}"
elif command -v nproc >/dev/null 2>&1; then
    PARALLEL="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
    PARALLEL="$(sysctl -n hw.ncpu)"
else
    PARALLEL=2
fi

git -C "${ROOT_DIR}" submodule update --init --recursive

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build "${BUILD_DIR}" \
    --config "${BUILD_TYPE}" \
    --target sakura-wx-prototype \
    --parallel "${PARALLEL}"
