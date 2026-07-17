#!/usr/bin/env bash
set -euo pipefail

APP="${1:?expected path to sakura-wx-prototype}"

if command -v xvfb-run >/dev/null 2>&1; then
    exec env SAKURA_WX_SMOKE_TEST=1 xvfb-run -a "${APP}"
fi

exec env SAKURA_WX_SMOKE_TEST=1 "${APP}"
