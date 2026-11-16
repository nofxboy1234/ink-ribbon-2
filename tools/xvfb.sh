#!/usr/bin/env bash
# Private X display helper so editor tests never touch the real desktop.
# Source this file, then call xvfb_start and xvfb_stop (or xvfb_trap_all).
#
#   . tools/xvfb.sh
#   xvfb_trap_all
#   xvfb_start
#   DISPLAY="$XVFB_DISPLAY" ./build/editor
#   xvfb_stop

XVFB_DISPLAY=""
XVFB_PID=""
XVFB_WIDTH="${XVFB_WIDTH:-1400}"
XVFB_HEIGHT="${XVFB_HEIGHT:-900}"

# Every test process gets a distinctive argv[0] so a later run can reap it
# without touching the operator's own display or editor.
XVFB_TAG="ink-ribbon-xvfb"
EDITOR_TAG="ink-ribbon-editor"

_xvfb_display_busy() {
    [ -e "/tmp/.X11-unix/X$1" ] || [ -e "/tmp/.X$1-lock" ]
}

test_reap() {
    pkill -f "^${EDITOR_TAG}" 2>/dev/null || true
    pkill -f "^${XVFB_TAG}" 2>/dev/null || true
}

test_leftovers() {
    pgrep -af "^(${EDITOR_TAG}|${XVFB_TAG})" 2>/dev/null || true
}

xvfb_start() {
    if [ "${INK_KEEP_DISPLAY:-0}" = "1" ] && [ -n "${DISPLAY:-}" ]; then
        XVFB_DISPLAY="${DISPLAY}"
        XVFB_PID=""
        return 0
    fi
    test_reap
    local n
    for n in $(seq 99 120); do
        if _xvfb_display_busy "$n"; then
            continue
        fi
        XVFB_DISPLAY=":$n"
        ( exec -a "$XVFB_TAG" Xvfb "$XVFB_DISPLAY" -screen 0 \
            "${XVFB_WIDTH}x${XVFB_HEIGHT}x24" -nolisten tcp ) \
            >"/tmp/ink-ribbon-xvfb-$n.log" 2>&1 &
        XVFB_PID=$!
        local i
        for i in $(seq 1 50); do
            if DISPLAY="$XVFB_DISPLAY" xdpyinfo >/dev/null 2>&1; then
                export DISPLAY="$XVFB_DISPLAY"
                return 0
            fi
            sleep 0.1
        done
        xvfb_stop
        cat "/tmp/ink-ribbon-xvfb-$n.log" 2>/dev/null >&2 || true
    done
    echo "FAIL no private X display could be started" >&2
    XVFB_DISPLAY=""
    return 1
}

xvfb_stop() {
    if [ -n "$XVFB_PID" ]; then
        kill "$XVFB_PID" 2>/dev/null || true
        wait "$XVFB_PID" 2>/dev/null || true
        XVFB_PID=""
    fi
}

# Stop the display and reap any process this harness named, on every exit path
# including Ctrl-C and a killed terminal.
xvfb_trap_all() {
    trap 'test_reap' EXIT INT TERM HUP
}
