#!/usr/bin/env bash
# Build, launch the editor on a private X display, optionally drive it, capture the window.
# usage: tools/shot.sh OUT.png [DRIVE_SCRIPT]
# Set INK_KEEP_DISPLAY=1 to use the current $DISPLAY instead of starting Xvfb.
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/xvfb.sh

out=${1:-/tmp/ink-ribbon.png}
drive=${2:-}

./fibs build editor
fibs_config=$(./fibs config --get)
editor_bin=".fibs/dist/$fibs_config/editor"
xvfb_start
meanwhile() {
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    xvfb_stop
    _release_display
    test_reap
    _run_unregister
}
trap meanwhile EXIT INT TERM HUP

( exec -a "$EDITOR_TAG" "$editor_bin" ${EDITOR_ARGS:-} ) >/tmp/ink-ribbon.log 2>&1 &
pid=$!

win=""
for _ in $(seq 1 80); do
    win=$(xdotool search --name "Ink Ribbon Level Editor" 2>/dev/null | head -1 || true)
    [ -n "$win" ] && break
    sleep 0.1
done
[ -n "$win" ] || { echo "editor window not found" >&2; cat /tmp/ink-ribbon.log >&2; exit 1; }
sleep 0.8
if [ -n "$drive" ]; then
    eval "$(xdotool getwindowgeometry --shell "$win" | tr -d '\r')"
    WIN=$win WX=$X WY=$Y DISPLAY="$XVFB_DISPLAY" bash "$drive"
    sleep 0.4
fi
import -window "$win" "$out"
echo "$out"
