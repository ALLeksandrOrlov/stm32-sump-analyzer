#!/bin/sh
set -eu

# All services belong to this session. Stop them if setup or PulseView exits.
children=
cleanup() {
    if test -n "$children"; then
        kill $children 2>/dev/null || true
        wait $children 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

wait_ready() {
    pid=$1
    kind=$2
    path=$3
    log=$4
    attempts=0
    while test "$attempts" -lt 100; do
        if ! kill -0 "$pid" 2>/dev/null; then
            printf 'Service exited before creating %s\n' "$path" >&2
            cat "$log" >&2
            return 1
        fi
        if test "$kind" "$path"; then
            return 0
        fi
        attempts=$((attempts + 1))
        sleep 0.1
    done
    printf 'Timed out waiting for %s\n' "$path" >&2
    cat "$log" >&2
    return 1
}

make pulseview-fixture
Xvfb :99 -screen 0 1440x900x24 -nolisten tcp >out/pulseview/xvfb.log 2>&1 &
xvfb_pid=$!
children="$xvfb_pid"
wait_ready "$xvfb_pid" -S /tmp/.X11-unix/X99 out/pulseview/xvfb.log
openbox >out/pulseview/openbox.log 2>&1 &
children="$children $!"
# Remove a previous session's readiness marker before starting a new device.
rm -f /tmp/stm32-sump-analyzer
out/pulseview/sump-fixture >out/pulseview/fixture.log 2>&1 &
fixture_pid=$!
children="$children $fixture_pid"
wait_ready "$fixture_pid" -L /tmp/stm32-sump-analyzer out/pulseview/fixture.log
x11vnc -display :99 -forever -shared -nopw -localhost \
  >out/pulseview/vnc.log 2>&1 &
children="$children $!"
websockify --web=/usr/share/novnc 6080 localhost:5900 \
  >out/pulseview/websockify.log 2>&1 &
children="$children $!"
pulseview -d ols:conn=/tmp/stm32-sump-analyzer -l 3 &
pulseview_pid=$!
children="$children $pulseview_pid"
wait "$pulseview_pid"
