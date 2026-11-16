#!/usr/bin/env bash
# End-to-end editor check. Drives the real X11 window and samples pixels.
# usage: tools/verify.sh
set -euo pipefail
cd "$(dirname "$0")/.."

out=/tmp/ink-ribbon-verify
rm -f "$out"-*.png /tmp/ink-ribbon-verify.lvl
build_log=/tmp/ink-ribbon-build.log

echo "== build =="
make -B -j4 >"$build_log" 2>&1
if grep -E "warning:|error:" "$build_log"; then
    echo "FAIL build warnings"
    exit 1
fi
echo "PASS build without warnings"

echo "== core tests =="
if ! make -s test >/tmp/ink-ribbon-test.log 2>&1; then
    tail -20 /tmp/ink-ribbon-test.log
    echo "FAIL core tests"
    exit 1
fi
grep -q "ALL CORE TESTS PASSED" /tmp/ink-ribbon-test.log
echo "PASS core tests"

gcc -std=gnu11 -Isrc tools/level_info.c src/level.c src/geom.c -lm -o /tmp/ink-level-info

./build/editor --level levels/re2-library.lvl >/tmp/ink-ribbon-verify.log 2>&1 &
pid=$!
cleanup() {
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}
trap cleanup EXIT

win=""
for _ in $(seq 1 60); do
    win=$(xdotool search --name "Ink Ribbon Level Editor" 2>/dev/null | head -1 || true)
    [ -n "$win" ] && break
    sleep 0.1
done
if [ -z "$win" ]; then
    echo "FAIL editor window did not appear"
    cat /tmp/ink-ribbon-verify.log
    exit 1
fi
eval "$(xdotool getwindowgeometry --shell "$win" | tr -d '\r')"
WX=$X
WY=$Y
xdotool windowactivate --sync "$win"
sleep 0.5

shot() { import -window "$win" "$1"; }
click() { xdotool mousemove --sync $((WX + $1)) $((WY + $2)) click 1; sleep 0.18; }

count_hex() {
    local file=$1 hex=$2
    convert "$file" -crop 790x777+190+23 +repage -format %c histogram:info:- 2>/dev/null |
        awk -v h="#$hex" 'toupper($0) ~ toupper(h) && !found { gsub(":", "", $1); print $1; found = 1 } END { if (!found) print 0 }'
}

shot "$out-start.png"
blue0=$(count_hex "$out-start.png" 2b3f50)
red0=$(count_hex "$out-start.png" 4a2028)
white0=$(count_hex "$out-start.png" f2f2f2)
if [ "$blue0" -lt 20000 ] || [ "$red0" -lt 15000 ]; then
    echo "FAIL demo fills blue=$blue0 red=$red0"
    exit 1
fi
echo "PASS demo fills blue=$blue0 red=$red0 white=$white0"

echo "== draw a closed orthogonal loop =="
xdotool key --clearmodifiers 2
click 600 330
click 700 330
click 700 430
click 600 430
click 600 330
sleep 0.4
shot "$out-draw.png"
blue1=$(count_hex "$out-draw.png" 2b3f50)
if [ $((blue1 - blue0)) -lt 2000 ]; then
    echo "FAIL closed loop did not add a fill blue=$blue0 -> $blue1"
    exit 1
fi
echo "PASS closed loop added a fill (+$((blue1 - blue0)) blue px)"

echo "== delete the new fill, then undo =="
xdotool key --clearmodifiers 1
click 650 380
xdotool key --clearmodifiers Delete
sleep 0.4
shot "$out-delete.png"
blue2=$(count_hex "$out-delete.png" 2b3f50)
if [ "$blue2" -ge "$blue1" ]; then
    echo "FAIL deleting the fill did not remove blue pixels $blue1 -> $blue2"
    exit 1
fi
echo "PASS delete removed the fill"

xdotool keydown ctrl; sleep 0.1
xdotool key z; sleep 0.1
xdotool keyup ctrl
sleep 0.5
shot "$out-undo.png"
blue3=$(count_hex "$out-undo.png" 2b3f50)
if [ "$blue3" -lt "$blue1" ]; then
    echo "FAIL undo did not restore the fill $blue2 -> $blue3"
    exit 1
fi
echo "PASS undo restored the fill"

echo "== place a door with a click, then undo =="
xdotool key --clearmodifiers 4
click 547 420
xdotool mousemove --sync $((WX + 900)) $((WY + 600))
sleep 0.3
shot "$out-door.png"
white1=$(count_hex "$out-door.png" f2f2f2)
if [ $((white1 - white0)) -lt 30 ]; then
    echo "FAIL click placement did not add a white door $white0 -> $white1"
    exit 1
fi
echo "PASS click placement added a white door (+$((white1 - white0)) white px)"
xdotool keydown ctrl; sleep 0.1
xdotool key z; sleep 0.1
xdotool keyup ctrl
sleep 0.4

echo "== label text survives clicking away =="
xdotool key --clearmodifiers 5
click 450 620
sleep 0.3
xdotool type --clearmodifiers --delay 20 "VERIFY LABEL"
sleep 0.3
click 1100 700
sleep 0.4
shot "$out-label.png"
echo "PASS label typed and deactivated"

echo "== save as =="
xdotool keydown ctrl; sleep 0.1
xdotool keydown shift; sleep 0.1
xdotool key s; sleep 0.1
xdotool keyup shift; sleep 0.1
xdotool keyup ctrl
sleep 0.5
xdotool type --clearmodifiers --delay 20 "/tmp/ink-ribbon-verify.lvl"
sleep 0.2
xdotool key --clearmodifiers Return
sleep 0.6
if [ ! -f /tmp/ink-ribbon-verify.lvl ]; then
    echo "FAIL save as did not write the level"
    exit 1
fi
walls=$(grep -c '^wall' /tmp/ink-ribbon-verify.lvl)
if [ "$walls" -ne 46 ]; then
    echo "FAIL saved wall count $walls, expected 46"
    exit 1
fi
/tmp/ink-level-info /tmp/ink-ribbon-verify.lvl
if ! grep -q '^label .*VERIFY LABEL' /tmp/ink-ribbon-verify.lvl; then
    echo "FAIL label text was lost when the input lost focus"
    exit 1
fi
echo "PASS save wrote 46 walls and the edited label"

echo "== reload through --level =="
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
./build/editor --level /tmp/ink-ribbon-verify.lvl >/tmp/ink-ribbon-verify2.log 2>&1 &
pid=$!
sleep 0.8
win=""
for _ in $(seq 1 60); do
    win=$(xdotool search --name "Ink Ribbon Level Editor" 2>/dev/null | head -1 || true)
    [ -n "$win" ] && break
    sleep 0.1
done
if [ -z "$win" ]; then
    echo "FAIL reload window did not appear"
    exit 1
fi
eval "$(xdotool getwindowgeometry --shell "$win" | tr -d '\r')"
WX=$X
WY=$Y
sleep 0.4
shot "$out-reload.png"
blue4=$(count_hex "$out-reload.png" 2b3f50)
if [ "$blue4" -lt "$blue1" ]; then
    echo "FAIL reload lost the fill blue=$blue1 -> $blue4"
    exit 1
fi
echo "PASS reload restored the level (blue=$blue4)"

echo "== prototype overlay changes the render =="
click 25 735
sleep 0.4
shot "$out-overlay.png"
convert "$out-reload.png" -crop 790x777+190+23 +repage /tmp/ink-cut-canvas.png 2>/dev/null
convert "$out-overlay.png" -crop 790x777+190+23 +repage /tmp/ink-overlay-canvas.png 2>/dev/null
metric=$(compare -metric AE /tmp/ink-cut-canvas.png /tmp/ink-overlay-canvas.png null: 2>&1 || true)
diff_px=$(printf '%s\n' "$metric" | awk '{ print $1 }')
case "$diff_px" in
    ''|*[!0-9]*)
        echo "FAIL overlay comparison returned no metric: $diff_px"
        exit 1
        ;;
esac
if [ "$diff_px" -lt 100 ]; then
    echo "FAIL overlay render did not change pixels (AE=$diff_px)"
    exit 1
fi
echo "PASS overlay render differs by $diff_px px"

echo "== l-shaped close when the last point is not aligned =="
xdotool key --clearmodifiers 2
sleep 0.2
click 600 650
click 700 650
click 700 730
click 600 650
sleep 0.4
shot "$out-lclose.png"
blue5=$(count_hex "$out-lclose.png" 2b3f50)
if [ $((blue5 - blue4)) -lt 2000 ]; then
    echo "FAIL l-shaped close did not create a fill $blue4 -> $blue5"
    exit 1
fi
echo "PASS l-shaped close created a fill (+$((blue5 - blue4)) blue px)"

echo "ALL EDITOR CHECKS PASSED"
echo "screenshots: $out-*.png"
