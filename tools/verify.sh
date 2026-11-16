#!/usr/bin/env bash
# End-to-end editor check. Drives the real X11 window and samples pixels.
# usage: tools/verify.sh
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/xvfb.sh

out=/tmp/ink-ribbon-verify
rm -f "$out"-*.png /tmp/ink-ribbon-verify.lvl /tmp/ink-ribbon-allgone.lvl
rm -f /tmp/ink-ribbon-restored.lvl /tmp/ink-ribbon-group.lvl /tmp/ink-ribbon-group-deleted.lvl
rm -f /tmp/ink-ribbon-lclose.lvl
build_log=/tmp/ink-ribbon-build.log

echo "== build =="
if ! make -B -j4 >"$build_log" 2>&1; then
    tail -40 "$build_log"
    echo "FAIL build"
    exit 1
fi
if grep -E "warning:|error:" "$build_log"; then
    echo "FAIL build warnings"
    exit 1
fi
echo "PASS build without warnings"

if grep -qE 'Prototype|place_mode|fill_mode|render_mode|RENDER_OVERLAY|PLACE_DRAG' src/editor.c; then
    echo "FAIL prototype settings still referenced in the editor"
    exit 1
fi
echo "PASS prototype settings removed"

echo "== core tests =="
if ! make -s test >/tmp/ink-ribbon-test.log 2>&1; then
    tail -20 /tmp/ink-ribbon-test.log
    echo "FAIL core tests"
    exit 1
fi
if ! grep -q "ALL CORE TESTS PASSED" /tmp/ink-ribbon-test.log; then
    tail -20 /tmp/ink-ribbon-test.log
    echo "FAIL core test marker"
    exit 1
fi
echo "PASS core tests ($(grep -c '^PASS' /tmp/ink-ribbon-test.log) checks)"

gcc -std=gnu11 -Isrc tools/level_info.c src/level.c src/geom.c -lm -o /tmp/ink-level-info

xvfb_start
./build/editor --level levels/re2-library.lvl >/tmp/ink-ribbon-verify.log 2>&1 &
pid=$!
cleanup() {
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    xvfb_stop
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
xdotool windowfocus "$win" 2>/dev/null || true
sleep 0.5

shot() { import -window "$win" "$1"; }
click() { xdotool mousemove --sync $((WX + $1)) $((WY + $2)) click 1; sleep 0.18; }
drag() {
    xdotool mousemove --sync $((WX + $1)) $((WY + $2))
    xdotool mousedown 1
    sleep 0.15
    xdotool mousemove --sync $((WX + $3)) $((WY + $4))
    sleep 0.2
    xdotool mouseup 1
    sleep 0.4
}
shift_click() {
    xdotool keydown shift
    sleep 0.1
    click "$1" "$2"
    xdotool keyup shift
    sleep 0.2
}
undo() {
    xdotool keydown ctrl; sleep 0.1
    xdotool key z; sleep 0.1
    xdotool keyup ctrl
    sleep 0.5
}
save_as() {
    local path=$1
    xdotool keydown ctrl; sleep 0.1
    xdotool keydown shift; sleep 0.1
    xdotool key s; sleep 0.1
    xdotool keyup shift; sleep 0.1
    xdotool keyup ctrl
    sleep 0.6
    xdotool keydown ctrl; sleep 0.1
    xdotool key a; sleep 0.1
    xdotool keyup ctrl
    sleep 0.1
    xdotool type --clearmodifiers --delay 20 "$path"
    sleep 0.3
    xdotool key --clearmodifiers Return
    sleep 0.8
    if [ ! -f "$path" ]; then
        shot /tmp/ink-ribbon-save-fail.png
        echo "FAIL save as did not write $path"
        exit 1
    fi
}
count_hex() {
    local file=$1 hex=$2
    convert "$file" -crop 790x777+190+23 +repage -format %c histogram:info:- 2>/dev/null |
        awk -v h="#$hex" 'toupper($0) ~ toupper(h) && !found { gsub(":", "", $1); print $1; found = 1 } END { if (!found) print 0 }'
}
count_records() { grep -c "$2" "$1" || true; }

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
undo
shot "$out-undo.png"
blue3=$(count_hex "$out-undo.png" 2b3f50)
if [ "$blue3" -lt "$blue1" ]; then
    echo "FAIL undo did not restore the fill $blue2 -> $blue3"
    exit 1
fi
echo "PASS undo restored the fill"

echo "== place a door with a click =="
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

echo "== label text survives clicking away =="
xdotool key --clearmodifiers 5
click 700 560
sleep 0.3
xdotool type --clearmodifiers --delay 20 "VERIFY LABEL"
sleep 0.3
click 1100 700
sleep 0.4
shot "$out-label.png"
echo "PASS label typed and deactivated"

echo "== cancel a label drag with Escape =="
xdotool key --clearmodifiers 1
sleep 0.2
xdotool mousemove --sync $((WX + 710)) $((WY + 570))
xdotool mousedown 1
sleep 0.15
xdotool mousemove --sync $((WX + 810)) $((WY + 620))
sleep 0.2
xdotool key Escape
sleep 0.2
xdotool mouseup 1
sleep 0.4
shot "$out-cancel.png"

echo "== save and check snapping =="
save_as /tmp/ink-ribbon-verify.lvl
walls=$(count_records /tmp/ink-ribbon-verify.lvl '^wall')
doors=$(count_records /tmp/ink-ribbon-verify.lvl '^door')
if [ "$walls" -ne 46 ] || [ "$doors" -ne 11 ]; then
    echo "FAIL saved counts walls=$walls doors=$doors, expected 46 and 11"
    exit 1
fi
if ! grep -q '^label .*VERIFY LABEL' /tmp/ink-ribbon-verify.lvl; then
    echo "FAIL label text was lost when the input lost focus"
    exit 1
fi
if ! grep -q '^label 784.000 576.000 19.000 VERIFY LABEL' /tmp/ink-ribbon-verify.lvl; then
    echo "FAIL cancelled drag still moved the label"
    exit 1
fi
label_line=$(grep 'VERIFY LABEL' /tmp/ink-ribbon-verify.lvl)
lx=$(printf '%s\n' "$label_line" | awk '{ print $2 }')
ly=$(printf '%s\n' "$label_line" | awk '{ print $3 }')
if ! awk -v x="$lx" -v y="$ly" 'BEGIN { exit !(int(x) % 8 == 0 && int(y) % 8 == 0) }'; then
    echo "FAIL placed label is off the grid: $lx, $ly"
    exit 1
fi
door_line=$(grep '^door' /tmp/ink-ribbon-verify.lvl | tail -1)
wid=$(printf '%s\n' "$door_line" | awk '{ print $3 }')
dt=$(printf '%s\n' "$door_line" | awk '{ print $4 }')
door_snap=$(awk -v wid="$wid" -v t="$dt" '
    $1 == "wall" && $2 == wid {
        x1 = $3; y1 = $4; x2 = $5; y2 = $6;
        cx = x1 + t * (x2 - x1);
        cy = y1 + t * (y2 - y1);
        c = (x1 == x2) ? cy : cx;
        d = c - 8 * int(c / 8 + 0.5);
        if (d < 0) d = -d;
        print (d <= 1.0) ? 1 : 0;
    }' /tmp/ink-ribbon-verify.lvl)
if [ "$door_snap" != 1 ]; then
    echo "FAIL placed door centre is off the grid"
    exit 1
fi
echo "PASS save wrote 46 walls, 11 doors, a grid label, and a grid door"
/tmp/ink-level-info /tmp/ink-ribbon-verify.lvl

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

echo "== marquee all, then drag walls, rooms, and doors together =="
save_as /tmp/ink-ribbon-before-drag.lvl
drag 975 760 200 60
shot "$out-drag-all.png"
drag 547 510 667 550
save_as /tmp/ink-ribbon-after-drag.lvl
drag_check=$(awk '
    FNR == NR {
        if ($1 == "wall") { bw[$2] = $3 " " $4 " " $5 " " $6; back[$2] = 1 }
        if ($1 == "label") { nl++; bl[nl] = $2 " " $3 }
        if ($1 == "door") { nd++; bd[nd] = $3 " " $4 " " $5 }
        next
    }
    $1 == "wall" && back[$2] {
        split(bw[$2], a, " ")
        wdx = $3 - a[1]; wdy = $4 - a[2]
        if (wdx != 0 || wdy != 0) {
            wmoved++
            if (first == 0) { first = 1; fx = wdx; fy = wdy }
            else if (wdx != fx || wdy != fy) bad = 1
            if (($5 - $3) != (a[3] - a[1]) || ($6 - $4) != (a[4] - a[2])) bad = 1
        }
    }
    $1 == "label" {
        li++
        split(bl[li], b, " ")
        ldx = $2 - b[1]; ldy = $3 - b[2]
        if (ldx != 0 || ldy != 0) {
            lmoved++
            if (ldx != fx || ldy != fy) bad = 1
        }
    }
    $1 == "door" {
        di++
        split(bd[di], b, " ")
        if ($3 != b[1] || $4 != b[2] || $5 != b[3]) bad = 1
    }
    END {
        grid = (fx % 8 == 0) && (fy % 8 == 0)
        if (wmoved > 0 && lmoved > 0 && !bad && grid) print "ok"
        else print "walls=" wmoved " labels=" lmoved " bad=" bad " grid=" grid " d=" fx "," fy
    }' /tmp/ink-ribbon-before-drag.lvl /tmp/ink-ribbon-after-drag.lvl)
if [ "$drag_check" != "ok" ]; then
    echo "FAIL group drag did not move walls, rooms, and doors together ($drag_check)"
    exit 1
fi
echo "PASS group drag moved walls and their doors with the rest of the selection"
undo

echo "== marquee select all, delete, undo =="
xdotool key --clearmodifiers 1
drag 200 100 960 760
shot "$out-marquee.png"
shift_click 650 270
shift_click 320 400
shift_click 547 361
shot "$out-shift-existing.png"
xdotool key --clearmodifiers Delete
sleep 0.4
save_as /tmp/ink-ribbon-allgone.lvl
for tag in wall room door label stairs; do
    n=$(count_records /tmp/ink-ribbon-allgone.lvl "^$tag")
    if [ "$n" -ne 0 ]; then
        echo "FAIL marquee delete left $n $tag records"
        exit 1
    fi
done
echo "PASS marquee selected all and shift-click preserved the selected walls, rooms, and doors"
undo
save_as /tmp/ink-ribbon-restored.lvl
walls=$(count_records /tmp/ink-ribbon-restored.lvl '^wall')
rooms=$(count_records /tmp/ink-ribbon-restored.lvl '^room')
doors=$(count_records /tmp/ink-ribbon-restored.lvl '^door')
labels=$(count_records /tmp/ink-ribbon-restored.lvl '^label')
stairs=$(count_records /tmp/ink-ribbon-restored.lvl '^stairs')
if [ "$walls" -ne 46 ] || [ "$rooms" -ne 7 ] || [ "$doors" -ne 11 ] ||
   [ "$labels" -ne 7 ] || [ "$stairs" -ne 4 ]; then
    echo "FAIL undo after marquee delete restored $walls/$rooms/$doors/$labels/$stairs"
    exit 1
fi
echo "PASS undo restored the level after marquee delete"

echo "== shift select and group drag with snap =="
xdotool key --clearmodifiers 1
shift_click 590 440
shift_click 440 390
shot "$out-multi.png"
drag 590 440 630 460
save_as /tmp/ink-ribbon-group.lvl
if ! grep -q '^label 664.000 408.000 19.000 Main Hall' /tmp/ink-ribbon-group.lvl; then
    echo "FAIL Main Hall label did not land on its snapped group position"
    exit 1
fi
if ! grep -q '^label 420.000 328.000 19.000 Library' /tmp/ink-ribbon-group.lvl; then
    echo "FAIL Library label did not move with the group"
    exit 1
fi
echo "PASS group drag moved both labels onto the grid"
xdotool key --clearmodifiers Delete
sleep 0.4
save_as /tmp/ink-ribbon-group-deleted.lvl
labels=$(count_records /tmp/ink-ribbon-group-deleted.lvl '^label')
if [ "$labels" -ne 5 ]; then
    echo "FAIL shift-select delete left $labels labels, expected 5"
    exit 1
fi
echo "PASS shift selection deleted both labels in one step"
undo

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
save_as /tmp/ink-ribbon-lclose.lvl
walls_l=$(count_records /tmp/ink-ribbon-lclose.lvl '^wall')
rooms_l=$(count_records /tmp/ink-ribbon-lclose.lvl '^room')
points=$(grep '^room' /tmp/ink-ribbon-lclose.lvl | tail -1 | awk '{ print $3 }')
if [ "$walls_l" -ne 50 ] || [ "$rooms_l" -ne 8 ] || [ "$points" -ne 4 ]; then
    echo "FAIL l-close saved walls=$walls_l rooms=$rooms_l points=$points, expected 50/8/4"
    exit 1
fi
echo "PASS l-shaped close saved 4 new walls and a 4 point room"

echo "ALL EDITOR CHECKS PASSED"
echo "screenshots: $out-*.png"
