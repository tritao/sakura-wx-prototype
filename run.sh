#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"

if [[ -x "${BUILD_DIR}/sakura-wx-prototype" ]]; then
    APP="${BUILD_DIR}/sakura-wx-prototype"
elif [[ -x "${BUILD_DIR}/${BUILD_TYPE}/sakura-wx-prototype" ]]; then
    APP="${BUILD_DIR}/${BUILD_TYPE}/sakura-wx-prototype"
else
    "${ROOT_DIR}/build.sh"
    APP="${BUILD_DIR}/sakura-wx-prototype"
fi

exec "${APP}" "$@"
