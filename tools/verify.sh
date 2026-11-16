#!/usr/bin/env bash
# End-to-end editor check. Runs on a private Xvfb display.
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/xvfb.sh

out=/tmp/ink-ribbon-verify
rm -f "$out"-*.png /tmp/ink-ribbon-shape.lvl /tmp/ink-ribbon-reshape.lvl
rm -f /tmp/ink-ribbon-grid32.lvl
build_log=/tmp/ink-ribbon-build.log

echo "== build =="
if ! make -B -j4 >"$build_log" 2>&1; then
    tail -40 "$build_log"; echo "FAIL build"; exit 1
fi
if grep -E "warning:|error:" "$build_log"; then echo "FAIL build warnings"; exit 1; fi
echo "PASS build without warnings"

echo "== core tests =="
if ! make -s test >/tmp/ink-ribbon-test.log 2>&1; then
    tail -20 /tmp/ink-ribbon-test.log; echo "FAIL core tests"; exit 1
fi
if ! grep -q "ALL CORE TESTS PASSED" /tmp/ink-ribbon-test.log; then
    tail -20 /tmp/ink-ribbon-test.log; echo "FAIL core test marker"; exit 1
fi
echo "PASS core tests ($(grep -c '^PASS' /tmp/ink-ribbon-test.log) checks)"

gcc -std=gnu11 -Isrc tools/level_info.c src/level.c src/geom.c -lm -o /tmp/ink-level-info

xvfb_start
( exec -a "$EDITOR_TAG" ./build/editor --level levels/re2-library.lvl ) >/tmp/ink-ribbon-verify.log 2>&1 &
pid=$!
cleanup() {
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    xvfb_stop; _release_display; test_reap; _run_unregister
}
trap cleanup EXIT INT TERM HUP

win=""
for _ in $(seq 1 80); do
    win=$(xdotool search --name "Ink Ribbon Level Editor" 2>/dev/null | head -1 || true)
    [ -n "$win" ] && break
    sleep 0.1
done
[ -n "$win" ] || { echo "FAIL editor window did not appear"; cat /tmp/ink-ribbon-verify.log; exit 1; }
eval "$(xdotool getwindowgeometry --shell "$win" | tr -d '\r')"
WX=$X; WY=$Y
xdotool windowfocus "$win" 2>/dev/null || true
sleep 0.5

shot() { import -window "$win" "$1"; }
click() { xdotool mousemove --sync $((WX + $1)) $((WY + $2)) click 1; sleep 0.18; }
drag() {
    xdotool mousemove --sync $((WX + $1)) $((WY + $2))
    xdotool mousedown 1; sleep 0.15
    xdotool mousemove --sync $((WX + $3)) $((WY + $4)); sleep 0.2
    xdotool mouseup 1; sleep 0.4
}
undo() {
    xdotool keydown ctrl; sleep 0.1; xdotool key z; sleep 0.1; xdotool keyup ctrl; sleep 0.5
}
new_level() {
    xdotool mousemove --sync $((WX + 35)) $((WY + 11)); xdotool click 1; sleep 0.4
    xdotool mousemove --sync $((WX + 30)) $((WY + 35)); xdotool click 1; sleep 0.6
}
_save_as_once() {
    local path=$1
    xdotool keydown ctrl; sleep 0.1; xdotool keydown shift; sleep 0.1
    xdotool key s; sleep 0.1; xdotool keyup shift; sleep 0.1; xdotool keyup ctrl
    sleep 0.6
    xdotool keydown ctrl; sleep 0.1; xdotool key a; sleep 0.1; xdotool keyup ctrl
    sleep 0.1
    xdotool type --clearmodifiers --delay 20 "$path"; sleep 0.3
    xdotool key --clearmodifiers Return; sleep 0.8
    [ -f "$path" ]
}
save_as() {
    local path=$1
    if _save_as_once "$path"; then return 0; fi
    rm -f "$path"
    xdotool key --clearmodifiers Escape; sleep 0.4
    if _save_as_once "$path"; then return 0; fi
    shot /tmp/ink-ribbon-save-fail.png
    echo "FAIL save as did not write $path"; exit 1
}
count_hex() {
    local file=$1 hex=$2
    convert "$file" -crop 790x777+190+23 +repage -format %c histogram:info:- 2>/dev/null |
        awk -v h="#$hex" 'toupper($0) ~ toupper(h) && !found { gsub(":", "", $1); print $1; found = 1 } END { if (!found) print 0 }'
}
count_records() { grep -c "$2" "$1" || true; }
pixel_hex() {
    convert "$1" -format "%[hex:p{$2,$3}]" info: 2>/dev/null
}

shot "$out-start.png"
blue0=$(count_hex "$out-start.png" 2b3f50)
red0=$(count_hex "$out-start.png" 4a2028)
if [ "$blue0" -lt 20000 ] || [ "$red0" -lt 15000 ]; then
    echo "FAIL demo fills blue=$blue0 red=$red0"; exit 1
fi
echo "PASS demo fills blue=$blue0 red=$red0"

echo "== a closed wall chain becomes one filled shape =="
new_level
xdotool key --clearmodifiers 2
click 450 300
click 650 300
click 650 500
click 450 500
click 450 300
sleep 0.4
shot "$out-shape.png"
blue1=$(count_hex "$out-shape.png" 2b3f50)
if [ "$blue1" -lt 5000 ]; then
    echo "FAIL closed shape did not fill (blue=$blue1)"; exit 1
fi
save_as /tmp/ink-ribbon-shape.lvl
walls=$(count_records /tmp/ink-ribbon-shape.lvl '^wall')
shapes=$(count_records /tmp/ink-ribbon-shape.lvl '^shape')
rooms=$(count_records /tmp/ink-ribbon-shape.lvl '^room')
if [ "$walls" -ne 4 ] || [ "$shapes" -ne 1 ] || [ "$rooms" -ne 0 ]; then
    echo "FAIL closed chain stored walls=$walls shapes=$shapes rooms=$rooms"; exit 1
fi
if ! grep -q '^shape 1 1 1' /tmp/ink-ribbon-shape.lvl; then
    echo "FAIL shape is not closed with a blue fill"; exit 1
fi
echo "PASS closed chain is one shape with a blue fill"

echo "== fill None removes the fill, Blue restores it =="
xdotool key --clearmodifiers 1
click 550 300
click 1037 152
sleep 0.3
shot "$out-none.png"
blue2=$(count_hex "$out-none.png" 2b3f50)
if [ "$blue2" -ge "$blue1" ]; then
    echo "FAIL None fill still paints ($blue1 -> $blue2 blue px)"; exit 1
fi
click 1127 152
sleep 0.3
shot "$out-blue.png"
blue3=$(count_hex "$out-blue.png" 2b3f50)
if [ "$blue3" -lt "$blue1" ]; then
    echo "FAIL Blue fill did not come back ($blue2 -> $blue3)"; exit 1
fi
echo "PASS None and Blue fill controls work"

echo "== plus shape fills only its inside =="
new_level
xdotool key --clearmodifiers 2
for xy in "560 625" "600 625" "600 670" "660 670" "660 705" "600 705" "600 745" "560 745" "560 705" "500 705" "500 670" "560 670" "560 625"; do
    click $xy
done
sleep 0.4
shot "$out-plus.png"
inside=$(pixel_hex "$out-plus.png" 580 640)
corner=$(pixel_hex "$out-plus.png" 515 640)
if [ "$inside" != "2B3F50" ]; then
    echo "FAIL plus inside is not blue (pixel $inside)"; exit 1
fi
if [ "$corner" = "2B3F50" ]; then
    echo "FAIL plus fill covers its bounding rectangle"; exit 1
fi
echo "PASS plus fill is inside only (inside=$inside corner=$corner)"

echo "== dragging a line reshapes the shape =="
new_level
xdotool key --clearmodifiers 2
click 450 300
click 650 300
click 650 500
click 450 500
click 450 300
sleep 0.3
xdotool key --clearmodifiers 1
click 550 300
drag 550 500 550 460
save_as /tmp/ink-ribbon-reshape.lvl
walls=$(count_records /tmp/ink-ribbon-reshape.lvl '^wall')
if [ "$walls" -ne 4 ]; then
    echo "FAIL reshape lost walls ($walls)"; exit 1
fi
if ! grep -q '^shape 1 1 1' /tmp/ink-ribbon-reshape.lvl; then
    echo "FAIL reshaped shape is not closed and filled"; exit 1
fi
geom=$(awk '
    $1 == "wall" {
        x1 = $4; y1 = $5; x2 = $6; y2 = $7
        if (y1 == y2) { h[n++] = y1 }
        else { v[m++] = x1 }
    }
    END {
        if (n != 2 || m != 2) { print "bad counts"; exit }
        if (h[0] > h[1]) { t = h[0]; h[0] = h[1]; h[1] = t }
        if (v[0] > v[1]) { t = v[0]; v[0] = v[1]; v[1] = t }
        printf "%d %d %d %d\n", h[0], h[1], v[0], v[1]
    }' /tmp/ink-ribbon-reshape.lvl)
if [ "$geom" != "-112 48 -136 64" ]; then
    echo "FAIL reshape geometry is '$geom', expected '-112 48 -136 64'"; exit 1
fi
echo "PASS dragged line moved, neighbours stretched, opposite line stayed"

echo "== the Grid setting changes the drawn grid =="
grid_pixels() {
    convert "$1" -crop 780x50+195+40 +repage -format %c histogram:info:- 2>/dev/null |
        awk 'toupper($0) !~ /#0B0C0E/ { gsub(":", "", $1); n += $1 } END { print n + 0 }'
}
shot "$out-grid8.png"
g8=$(grid_pixels "$out-grid8.png")
xdotool mousemove --sync $((WX + 63)) $((WY + 269)); xdotool click 1; sleep 0.5
xdotool mousemove --sync $((WX + 63)) $((WY + 359)); sleep 0.4
xdotool click 1; sleep 0.5
shot "$out-grid32.png"
g32=$(grid_pixels "$out-grid32.png")
if [ "$g32" -le 0 ] || [ "$g8" -lt $((g32 * 2)) ]; then
    echo "FAIL drawn grid ignores the Grid setting (8=$g8 32=$g32)"; exit 1
fi
echo "PASS grid size 8 draws $g8 px and 32 draws $g32"

echo "== losing focus during a wall chain must not freeze the editor =="
xdotool key --clearmodifiers 2
click 400 400
click 500 400
xdotool mousemove --sync $((WX + 520)) $((WY + 300)); sleep 0.3
python3 tools/focus_drop.py >/dev/null
sleep 0.4
if ! kill -0 "$pid" 2>/dev/null; then echo "FAIL editor died on focus loss"; exit 1; fi
rss=$(ps -o rss= -p "$pid" | tr -d ' ')
if [ -z "$rss" ] || [ "$rss" -gt 1200000 ]; then
    echo "FAIL focus loss memory ${rss:-unknown} KB"; exit 1
fi
xdotool windowfocus "$win" 2>/dev/null || true
sleep 1
shot "$out-focus-a.png"; sleep 1; shot "$out-focus-b.png"
xdotool key --clearmodifiers 1; sleep 0.8; shot "$out-focus-c.png"
metric=$(compare -metric AE "$out-focus-b.png" "$out-focus-c.png" null: 2>&1 || true)
frozen=$(printf '%s\n' "$metric" | awk '{ print $1 }')
if [ "$frozen" -eq 0 ]; then echo "FAIL no redraw after focus return"; exit 1; fi
echo "PASS focus loss survived (rss=${rss} KB, ${frozen} px changed)"

echo "== no leftover test processes =="
cleanup
overflow=$(test_leftovers)
if [ -n "$overflow" ]; then echo "FAIL leftover processes:"; echo "$overflow"; exit 1; fi
echo "PASS no leftover editor or display processes"

echo "ALL EDITOR CHECKS PASSED"
