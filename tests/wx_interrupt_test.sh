#!/usr/bin/env bash
set -euo pipefail

APP="${1:?expected path to sakura-wx-prototype}"

if ! command -v xdotool >/dev/null 2>&1; then
    echo "wx_interrupt: SKIP (xdotool is not installed)"
    exit 0
fi

if ! command -v xvfb-run >/dev/null 2>&1; then
    echo "wx_interrupt: SKIP (xvfb-run is not installed)"
    exit 0
fi

exec xvfb-run -a bash -s -- "${APP}" <<'TEST_SCRIPT'
set -euo pipefail

app="$1"
log="$(mktemp)"
app_pid=""
cleanup() {
    if [[ -n "${app_pid}" ]] && kill -0 "${app_pid}" 2>/dev/null; then
        kill "${app_pid}" 2>/dev/null || true
        wait "${app_pid}" 2>/dev/null || true
    fi
    rm -f "${log}"
}
trap cleanup EXIT

SAKURA_WX_INTERRUPT_TEST=1 "${app}" >"${log}" 2>&1 &
app_pid=$!

window_id=""
for _ in $(seq 1 200); do
    window_id="$(xdotool search --onlyvisible --class 'sakura-wx-prototype' 2>/dev/null | head -n 1 || true)"
    if [[ -n "${window_id}" ]]; then
        break
    fi
    sleep 0.05
done
if [[ -z "${window_id}" ]]; then
    echo "wx_interrupt: FAIL (terminal window did not appear)" >&2
    tail -n 80 "${log}" >&2 || true
    exit 1
fi

xdotool windowfocus "${window_id}" 2>/dev/null || true
xdotool type --window "${window_id}" --delay 0 yes
xdotool key --window "${window_id}" Return
sleep 0.30

start_ns="$(date +%s%N)"
xdotool key --window "${window_id}" ctrl+c
xdotool type --window "${window_id}" --delay 0 \
    "printf 'sakura-wx-e2e-marker\\n\\033]2;sakura-wx-interrupt-ok\\007'; sleep 10"
xdotool key --window "${window_id}" Return

deadline=$((SECONDS + 10))
while true; do
    if grep -q '^wx interrupt e2e: PASS' "${log}"; then
        break
    fi
    if ! kill -0 "${app_pid}" 2>/dev/null; then
        echo "wx_interrupt: FAIL (application exited before PASS)" >&2
        cat "${log}" >&2 || true
        exit 1
    fi
    if (( SECONDS >= deadline )); then
        echo "wx_interrupt: FAIL (timeout waiting for painted marker)" >&2
        cat "${log}" >&2 || true
        exit 1
    fi
    sleep 0.05
done

end_ns="$(date +%s%N)"
elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
printf 'wx_interrupt: PASS interrupt_to_paint_ms=%s\n' "${elapsed_ms}"
if (( elapsed_ms > 2000 )); then
    echo "wx_interrupt: FAIL (interrupt-to-paint latency exceeded 2000 ms)" >&2
    cat "${log}" >&2 || true
    exit 1
fi
TEST_SCRIPT
