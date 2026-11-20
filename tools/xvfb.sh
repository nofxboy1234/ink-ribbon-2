#!/usr/bin/env bash
# Private X display helper so editor tests never touch the real desktop, and
# never interfere with another test run that is already going.
#
#   . tools/xvfb.sh
#   xvfb_trap_all
#   xvfb_start
#   DISPLAY="$XVFB_DISPLAY" .fibs/dist/<config>/editor
#   xvfb_stop
#
# Every process this harness starts carries its own run id in argv[0], so a
# cleanup targets only its own run. A run also leaves a marker file holding its
# shell pid, and claims its X display with a lock directory. A later run reaps
# the processes and the display lock of any run whose marker is stale.

RUN_DIR="/tmp/ink-ribbon-runs"
RUN_ID="$$-${RANDOM}${RANDOM}"
XVFB_TAG="ink-ribbon-xvfb-${RUN_ID}"
EDITOR_TAG="ink-ribbon-editor-${RUN_ID}"
RUN_MARKER="${RUN_DIR}/${RUN_ID}"

XVFB_DISPLAY=""
XVFB_PID=""
XVFB_LOCK=""
XVFB_WIDTH="${XVFB_WIDTH:-1400}"
XVFB_HEIGHT="${XVFB_HEIGHT:-900}"

_run_register() {
    mkdir -p "$RUN_DIR"
    printf '%s\n' "$$" >"$RUN_MARKER"
}

_run_unregister() {
    rm -f "$RUN_MARKER"
}

# Kill only the processes this run started.
test_reap() {
    pkill -f "^${EDITOR_TAG}( |$)" 2>/dev/null || true
    pkill -f "^${XVFB_TAG}( |$)" 2>/dev/null || true
}

# Processes of this run that are still alive. Other runs do not count.
test_leftovers() {
    pgrep -af "^(${EDITOR_TAG}|${XVFB_TAG})( |$)" 2>/dev/null || true
}

_run_alive() {
    local marker="$RUN_DIR/$1"
    local pid
    [ -e "$marker" ] || return 1
    pid=$(cat "$marker" 2>/dev/null || true)
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

# Reap runs that are gone, and free the displays they held.
test_reap_stale() {
    pkill -f '^ink-ribbon-editor( |$)' 2>/dev/null || true
    pkill -f '^ink-ribbon-xvfb( |$)' 2>/dev/null || true
    [ -d "$RUN_DIR" ] || return 0
    local entry id
    for entry in "$RUN_DIR"/*; do
        [ -e "$entry" ] || continue
        id=$(basename "$entry")
        [ "$id" = "$RUN_ID" ] && continue
        case "$id" in
            display-*) continue ;;
        esac
        if _run_alive "$id"; then
            continue
        fi
        pkill -f "^ink-ribbon-editor-${id}( |$)" 2>/dev/null || true
        pkill -f "^ink-ribbon-xvfb-${id}( |$)" 2>/dev/null || true
        rm -f "$entry"
    done
    local lock owner
    for lock in "$RUN_DIR"/display-*; do
        [ -d "$lock" ] || continue
        owner=$(cat "$lock/owner" 2>/dev/null || true)
        if [ -n "$owner" ] && _run_alive "$owner"; then
            continue
        fi
        case "$lock" in
            "$RUN_DIR"/display-*) rm -rf "$lock" ;;
        esac
    done
    return 0
}

_release_display() {
    if [ -n "$XVFB_LOCK" ]; then
        case "$XVFB_LOCK" in
            "$RUN_DIR"/display-*) rm -rf "$XVFB_LOCK" ;;
        esac
        XVFB_LOCK=""
    fi
}

_claim_display() {
    local lock="$RUN_DIR/display-$1"
    if mkdir "$lock" 2>/dev/null; then
        printf '%s\n' "$RUN_ID" >"$lock/owner"
        XVFB_LOCK="$lock"
        return 0
    fi
    return 1
}

xvfb_start() {
    if [ "${INK_KEEP_DISPLAY:-0}" = "1" ] && [ -n "${DISPLAY:-}" ]; then
        XVFB_DISPLAY="${DISPLAY}"
        XVFB_PID=""
        return 0
    fi
    _run_register
    test_reap_stale
    local n i
    for n in $(seq 99 120); do
        if ! _claim_display "$n"; then
            continue
        fi
        XVFB_DISPLAY=":$n"
        ( exec -a "$XVFB_TAG" Xvfb "$XVFB_DISPLAY" -screen 0 \
            "${XVFB_WIDTH}x${XVFB_HEIGHT}x24" -nolisten tcp ) \
            >"/tmp/ink-ribbon-xvfb-$n.log" 2>&1 &
        XVFB_PID=$!
        for i in $(seq 1 50); do
            if ! kill -0 "$XVFB_PID" 2>/dev/null; then
                break
            fi
            if DISPLAY="$XVFB_DISPLAY" xdpyinfo >/dev/null 2>&1; then
                export DISPLAY="$XVFB_DISPLAY"
                return 0
            fi
            sleep 0.1
        done
        xvfb_stop
        _release_display
        XVFB_DISPLAY=""
        cat "/tmp/ink-ribbon-xvfb-$n.log" 2>/dev/null >&2 || true
    done
    echo "FAIL no private X display could be started" >&2
    return 1
}

xvfb_stop() {
    if [ -n "$XVFB_PID" ]; then
        kill "$XVFB_PID" 2>/dev/null || true
        wait "$XVFB_PID" 2>/dev/null || true
        XVFB_PID=""
    fi
}

# Stop this run's processes and drop its marker and display lock on every exit
# path, including Ctrl-C, SIGTERM, and a closed terminal.
xvfb_trap_all() {
    trap '_run_exit' EXIT INT TERM HUP
}

_run_exit() {
    test_reap
    xvfb_stop
    _release_display
    _run_unregister
}
