#!/usr/bin/env bash
# Build, launch the editor, optionally drive it, capture the window.
# usage: tools/shot.sh OUT.png [DRIVE_SCRIPT]
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-/tmp/ink-ribbon.png}
drive=${2:-}
make -s
./build/editor >/tmp/ink-ribbon.log 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true' EXIT
win=""
for _ in $(seq 1 50); do
    win=$(xdotool search --name "Ink Ribbon Level Editor" 2>/dev/null | head -1 || true)
    [ -n "$win" ] && break
    sleep 0.1
done
[ -n "$win" ] || { echo "editor window not found" >&2; cat /tmp/ink-ribbon.log >&2; exit 1; }
sleep 0.6
if [ -n "$drive" ]; then
    eval "$(xdotool getwindowgeometry --shell "$win" | tr -d '\r')"
    WIN=$win WX=$X WY=$Y bash "$drive"
    sleep 0.4
fi
import -window "$win" "$out"
echo "$out"
