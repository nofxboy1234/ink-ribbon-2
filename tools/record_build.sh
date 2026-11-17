#!/usr/bin/env bash
# Rebuild the reference map through the editor UI on a private Xvfb display and
# record the whole build fullscreen at 1280x800, 30 fps, with the cursor.
#
#   tools/record_build.sh [OUT.mp4]
#   INK_NO_RECORD=1 tools/record_build.sh   # drive the UI without ffmpeg
set -euo pipefail
cd "$(dirname "$0")/.."
export XVFB_WIDTH=1280 XVFB_HEIGHT=800
. tools/xvfb.sh

out=${1:-/home/dylan/Downloads/ink-proto/build-map.mp4}
plan=${INK_PLAN:-levels/re2-library-source.lvl}
target=${INK_TARGET:-levels/re2-library.lvl}
record=1
[ "${INK_NO_RECORD:-0}" = "1" ] && record=0
mkdir -p "$(dirname "$out")"

make -s

xvfb_start
win=""
pid=""
ffpid=""
cleanup() {
    kill "$ffpid" 2>/dev/null || true
    wait "$ffpid" 2>/dev/null || true
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    xvfb_stop
    _release_display
    test_reap
    _run_unregister
}
trap cleanup EXIT INT TERM HUP

if [ "$record" = "1" ]; then
    ffmpeg -hide_banner -loglevel warning -y \
        -f x11grab -framerate 30 -video_size 1280x800 -draw_mouse 1 \
        -i "$XVFB_DISPLAY" \
        -c:v libx264 -preset veryfast -crf 22 -pix_fmt yuv420p \
        -movflags +faststart "$out" >/tmp/ink-ribbon-ffmpeg.log 2>&1 &
    ffpid=$!
    sleep 1
fi

( exec -a "$EDITOR_TAG" ./build/editor ) >/tmp/ink-ribbon-build-editor.log 2>&1 &
pid=$!
for _ in $(seq 1 100); do
    win=$(xdotool search --name "Ink Ribbon Level Editor" 2>/dev/null | head -1 || true)
    [ -n "$win" ] && break
    sleep 0.1
done
[ -n "$win" ] || { echo "FAIL no editor window" >&2; cat /tmp/ink-ribbon-build-editor.log >&2; exit 1; }
xdotool windowmove "$win" 0 0 2>/dev/null || true
xdotool windowfocus "$win" 2>/dev/null || true
sleep 0.8

driver_args=(--plan "$plan" --out "$target")
[ -n "${INK_ONLY:-}" ] && driver_args+=(--only "$INK_ONLY")
python3 tools/build_map.py "${driver_args[@]}"
sleep 1.5
import -window "$win" /tmp/ink-ribbon-build-final.png || true

if [ "$record" = "1" ]; then
    kill -INT "$ffpid"
    wait "$ffpid" || true
    ffpid=""
    ffprobe -v error -select_streams v:0 \
        -show_entries stream=width,height,r_frame_rate:format=duration,size \
        -of default=noprint_wrappers=1 "$out"
fi
echo "final screenshot /tmp/ink-ribbon-build-final.png"
