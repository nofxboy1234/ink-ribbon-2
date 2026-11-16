#!/usr/bin/env bash
# Private X display helper so editor tests never touch the real desktop.
# Source this file, then call xvfb_start and xvfb_stop.
#
#   . tools/xvfb.sh
#   xvfb_start
#   DISPLAY="$XVFB_DISPLAY" ./build/editor
#   xvfb_stop

XVFB_DISPLAY=""
XVFB_PID=""
XVFB_WIDTH="${XVFB_WIDTH:-1400}"
XVFB_HEIGHT="${XVFB_HEIGHT:-900}"

_xvfb_display_free() {
    [ ! -e "/tmp/.X11-unix/X$1" ]
}

xvfb_start() {
    if [ "${INK_KEEP_DISPLAY:-0}" = "1" ] && [ -n "${DISPLAY:-}" ]; then
        XVFB_DISPLAY="${DISPLAY}"
        XVFB_PID=""
        return 0
    fi
    local n=99
    while ! _xvfb_display_free "$n"; do
        n=$((n + 1))
        if [ "$n" -gt 120 ]; then
            echo "FAIL no free X display between 99 and 120" >&2
            return 1
        fi
    done
    XVFB_DISPLAY=":$n"
    Xvfb "$XVFB_DISPLAY" -screen 0 "${XVFB_WIDTH}x${XVFB_HEIGHT}x24" -nolisten tcp \
        >/tmp/ink-ribbon-xvfb-$n.log 2>&1 &
    XVFB_PID=$!
    local i
    for i in $(seq 1 100); do
        if DISPLAY="$XVFB_DISPLAY" xdpyinfo >/dev/null 2>&1; then
            export DISPLAY="$XVFB_DISPLAY"
            return 0
        fi
        sleep 0.1
    done
    echo "FAIL Xvfb $XVFB_DISPLAY did not start" >&2
    cat "/tmp/ink-ribbon-xvfb-$n.log" >&2 || true
    xvfb_stop
    return 1
}

xvfb_stop() {
    if [ -n "$XVFB_PID" ]; then
        kill "$XVFB_PID" 2>/dev/null || true
        wait "$XVFB_PID" 2>/dev/null || true
    fi
    XVFB_PID=""
}
