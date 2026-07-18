#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-relwithdebinfo}"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT_DIR}/benchmark-results}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
CACHE_SIZES="${CACHE_SIZES:-2097152 4194304 8388608 16777216}"

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
    -DBUILD_TESTING=OFF \
    -DSAKURA_BUILD_BENCHMARKS=ON
cmake --build "${BUILD_DIR}" \
    --config "${BUILD_TYPE}" \
    --target sakura-wx-paint-benchmark \
    --parallel "${PARALLEL}"

BENCHMARK="${BUILD_DIR}/sakura-wx-paint-benchmark"
if [[ ! -x "${BENCHMARK}" ]]; then
    BENCHMARK="${BUILD_DIR}/${BUILD_TYPE}/sakura-wx-paint-benchmark"
fi
if [[ ! -x "${BENCHMARK}" ]]; then
    echo "wx paint benchmark executable was not found" >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"
read -r -a cache_sizes <<< "${CACHE_SIZES}"
if command -v xvfb-run >/dev/null 2>&1; then
    RUNNER=(xvfb-run -a)
else
    RUNNER=()
fi

for cache_bytes in "${cache_sizes[@]}"; do
    output="${OUTPUT_DIR}/wx-paint-${cache_bytes}.json"
    echo "running wx paint profile with ${cache_bytes} cache bytes -> ${output}"
    "${RUNNER[@]}" "${BENCHMARK}" --json \
        "--cache-bytes=${cache_bytes}" > "${output}"
done
