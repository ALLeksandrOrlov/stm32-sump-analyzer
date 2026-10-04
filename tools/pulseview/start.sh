#!/bin/sh
set -eu
mkdir -p out/pulseview
gcc -std=c11 -O2 -Wall -Wextra -Werror -Icore \
  tools/pulseview/sump_fixture.c core/analyzer_app.c core/capture.c core/sump_protocol.c \
  -lutil -o out/pulseview/sump-fixture
Xvfb :99 -screen 0 1440x900x24 -nolisten tcp >out/pulseview/xvfb.log 2>&1 &
while ! test -S /tmp/.X11-unix/X99; do sleep 0.1; done
openbox >out/pulseview/openbox.log 2>&1 &
out/pulseview/sump-fixture >out/pulseview/fixture.log 2>&1 &
while ! test -L /tmp/stm32-sump-analyzer; do sleep 0.1; done
x11vnc -display :99 -forever -shared -nopw -localhost \
  >out/pulseview/vnc.log 2>&1 &
websockify --web=/usr/share/novnc 6080 localhost:5900 \
  >out/pulseview/websockify.log 2>&1 &
exec pulseview -d ols:conn=/tmp/stm32-sump-analyzer -l 3
