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
open_level() {
    local path=$1
    xdotool keydown ctrl; sleep 0.1; xdotool key o; sleep 0.1; xdotool keyup ctrl
    sleep 0.6
    xdotool keydown ctrl; sleep 0.1; xdotool key a; sleep 0.1; xdotool keyup ctrl
    sleep 0.1
    xdotool type --clearmodifiers --delay 20 "$path"; sleep 0.3
    xdotool key --clearmodifiers Return; sleep 1.0
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
set_grid() {
    xdotool mousemove --sync $((WX + 63)) $((WY + 269)); xdotool click 1; sleep 0.5
    xdotool mousemove --sync $((WX + 63)) $((WY + $1)); sleep 0.4
    xdotool click 1; sleep 0.5
}
grid_flush() {
    python3 - "$1" "$2" <<'PYEOF'
import sys
path, grid = sys.argv[1], float(sys.argv[2])
rows = [line.split() for line in open(path) if line.startswith("stairs ")]
if len(rows) != 1:
    print("expected one stair, found", len(rows))
    raise SystemExit(1)
x, y, w, h = float(rows[0][1]), float(rows[0][2]), float(rows[0][5]), float(rows[0][6])
edges = [x, y, x + w, y + h]
bad = [v for v in edges if abs(v / grid - round(v / grid)) > 1e-6]
print(f"x {x:g}..{x + w:g} y {y:g}..{y + h:g}")
raise SystemExit(1 if bad else 0)
PYEOF
}
stair_edge_drag() {
    python3 - "$1" "$2" "$3" <<'PYEOF'
import sys
before, after, grid = sys.argv[1], sys.argv[2], float(sys.argv[3])
def box(path):
    r = [line.split() for line in open(path) if line.startswith("stairs ")][0]
    return float(r[1]), float(r[2]), float(r[5]), float(r[6])
x0, y0, w0, h0 = box(before)
x1, y1, w1, h1 = box(after)
if abs(x1 - x0) > 1e-6 or abs(y1 - y0) > 1e-6 or abs(h1 - h0) > 1e-6:
    print("an edge that should stay fixed moved")
    raise SystemExit(1)
if w1 <= w0:
    print("the dragged edge did not move out")
    raise SystemExit(1)
if abs((x1 + w1) / grid - round((x1 + w1) / grid)) > 1e-6:
    print("the dragged edge is off the grid")
    raise SystemExit(1)
print(f"right edge {x0 + w0:g} -> {x1 + w1:g}, opposite edge {x0:g} stayed")
PYEOF
}
# Every wall line in two saved levels, with its delta and length change.
wall_moves() {
    python3 - "$1" "$2" <<'PYEOF'
import sys

def walls(path):
    out = {}
    for line in open(path):
        parts = line.split()
        if len(parts) != 7 or parts[0] != "wall":
            continue
        wid = int(parts[1])
        x1, y1, x2, y2 = (float(v) for v in parts[3:7])
        out[wid] = (x1, y1, x2, y2)
    return out

before = walls(sys.argv[1])
after = walls(sys.argv[2])
if set(before) != set(after):
    print("wallset")
    raise SystemExit
deltas = set()
reshaped = 0
for wid, a in before.items():
    b = after[wid]
    dx1, dy1 = round(b[0] - a[0], 3), round(b[1] - a[1], 3)
    dx2, dy2 = round(b[2] - a[2], 3), round(b[3] - a[3], 3)
    if (dx1, dy1) != (dx2, dy2):
        reshaped += 1
        continue
    deltas.add((dx1, dy1))
print(reshaped, len(deltas), " ".join(f"{d[0]:g},{d[1]:g}" for d in sorted(deltas)))
PYEOF
}

shot "$out-start.png"
blue0=$(count_hex "$out-start.png" 2b3f50)
red0=$(count_hex "$out-start.png" 4a2028)
if [ "$blue0" -lt 20000 ] || [ "$red0" -lt 15000 ]; then
    echo "FAIL demo fills blue=$blue0 red=$red0"; exit 1
fi
if ! python3 - levels/re2-library.lvl <<'PYEOF'
import sys
want = {"wall": 42, "shape": 11, "room": 6, "door": 10, "label": 6, "stairs": 4}
have = {key: 0 for key in want}
for line in open(sys.argv[1]):
    tag = line.split(" ", 1)[0]
    if tag in have:
        have[tag] += 1
if have != want:
    print("demo counts", have)
    raise SystemExit(1)
PYEOF
then
    echo "FAIL the demo level was overwritten or corrupted"; exit 1
fi
echo "PASS demo fills blue=$blue0 red=$red0 and keeps its record counts"

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

echo "== marquee selects walls, doors, and the rest as one movable group =="
open_level levels/re2-library.lvl
save_as /tmp/ink-ribbon-group-before.lvl
xdotool key --clearmodifiers 1
xdotool mousemove --sync $((WX + 215)) $((WY + 215))
xdotool mousedown 1; sleep 0.2
xdotool mousemove --sync $((WX + 950)) $((WY + 700)); sleep 0.3
xdotool mouseup 1; sleep 0.5
shot "$out-group-selected.png"
edges=$(convert "$out-group-selected.png" -format %c histogram:info:- 2>/dev/null |
    awk 'toupper($0) ~ /#5AA0C8/ { gsub(":", "", $1); print $1 }' | head -1)
if [ "${edges:-0}" -lt 1000 ]; then
    echo "FAIL marquee did not select the walls (accent px=${edges:-0})"; exit 1
fi
drag 540 262 610 262
save_as /tmp/ink-ribbon-group-after.lvl
moves=$(wall_moves /tmp/ink-ribbon-group-before.lvl /tmp/ink-ribbon-group-after.lvl)
reshaped=$(printf '%s' "$moves" | awk '{ print $1 }')
deltas=$(printf '%s' "$moves" | awk '{ print $2 }')
delta=$(printf '%s' "$moves" | awk '{ print $3 }')
case "$reshaped$deltas" in
    *[!0-9]*|"")
        echo "FAIL could not read the wall deltas: $moves"; exit 1
        ;;
esac
if [ "$deltas" -eq 0 ]; then
    echo "FAIL group drag moved no walls"; exit 1
fi
if [ "$reshaped" -ne 0 ] || [ "$deltas" -ne 1 ]; then
    echo "FAIL group drag reshaped $reshaped walls with $deltas distinct deltas"; exit 1
fi
door_before=$(grep -c '^door' /tmp/ink-ribbon-group-before.lvl)
door_after=$(grep -c '^door' /tmp/ink-ribbon-group-after.lvl)
if [ "$door_before" -ne "$door_after" ] || [ "$door_before" -eq 0 ]; then
    echo "FAIL doors changed count $door_before -> $door_after"; exit 1
fi
if ! python3 - /tmp/ink-ribbon-group-before.lvl /tmp/ink-ribbon-group-after.lvl <<'PYEOF'
import sys

def load(path):
    walls, doors = {}, {}
    for line in open(path):
        parts = line.split()
        if parts[0] == "wall":
            walls[int(parts[1])] = tuple(float(v) for v in parts[3:7])
        elif parts[0] == "door":
            doors[int(parts[2])] = (int(parts[1]), round(float(parts[3]), 3))
    return walls, doors

before_w, before_d = load(sys.argv[1])
after_w, after_d = load(sys.argv[2])
dx = round(after_w[1][0] - before_w[1][0], 3)
dy = round(after_w[1][1] - before_w[1][1], 3)
if (dx, dy) == (0, 0):
    raise SystemExit(1)
for wid, wall in after_w.items():
    old = before_w[wid]
    if abs(wall[0] - old[0] - dx) > 0.01 or abs(wall[1] - old[1] - dy) > 0.01:
        raise SystemExit(1)
    if abs(wall[2] - old[2] - dx) > 0.01 or abs(wall[3] - old[3] - dy) > 0.01:
        raise SystemExit(1)
for wid, (host, t) in after_d.items():
    if before_d[wid] != (host, t):
        raise SystemExit(1)
PYEOF
then
    echo "FAIL walls and doors did not translate together"; exit 1
fi
echo "PASS group drag moved every wall by ($delta) with no reshape and every door followed"

echo "== two edges of one shape select and drag together =="
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
xdotool keydown shift; xdotool mousemove --sync $((WX + 550)) $((WY + 500)); xdotool click 1; xdotool keyup shift
sleep 0.4
shot "$out-edges.png"
accent=$(convert "$out-edges.png" -format %c histogram:info:- 2>/dev/null |
    awk 'toupper($0) ~ /#5AA0C8/ { gsub(":", "", $1); print $1 }' | head -1)
amber=$(convert "$out-edges.png" -format %c histogram:info:- 2>/dev/null |
    awk 'toupper($0) ~ /#E0A33C/ { gsub(":", "", $1); print $1 }' | head -1)
vertex_a=$(pixel_hex "$out-edges.png" 450 300)
vertex_b=$(pixel_hex "$out-edges.png" 650 500)
side_edge=$(pixel_hex "$out-edges.png" 450 400)
if [ "${amber:-0}" -lt 20 ]; then
    echo "FAIL selected vertices are not amber (amber px=${amber:-0})"; exit 1
fi
if [ "$vertex_a" != "E0A33C" ] && [ "$vertex_b" != "E0A33C" ]; then
    echo "FAIL neither selected endpoint is amber ($vertex_a, $vertex_b)"; exit 1
fi
if [ "$side_edge" = "5AA0C8" ]; then
    echo "FAIL an unselected edge is drawn as selected"; exit 1
fi
save_as /tmp/ink-ribbon-edges-before.lvl
drag 550 300 550 340
save_as /tmp/ink-ribbon-edges-after.lvl
if ! python3 - /tmp/ink-ribbon-edges-before.lvl /tmp/ink-ribbon-edges-after.lvl <<'PYEOF'
import sys

def horizontals(path):
    out = {}
    for line in open(path):
        parts = line.split()
        if parts[0] != "wall":
            continue
        y1, y2 = float(parts[4]), float(parts[6])
        if abs(y1 - y2) < 0.001:
            out[int(parts[1])] = round(y1, 3)
    return out

before = horizontals(sys.argv[1])
after = horizontals(sys.argv[2])
if set(before) != set(after):
    raise SystemExit(1)
moved = [wid for wid in before if abs(before[wid] - after[wid]) > 0.001]
if len(moved) != 2:
    raise SystemExit(1)
shifts = {round(after[wid] - before[wid], 3) for wid in moved}
if len(shifts) != 1 or abs(next(iter(shifts))) < 1:
    raise SystemExit(1)
PYEOF
then
    echo "FAIL dragging two selected edges did not move both"; exit 1
fi
edges_accent=$accent
echo "PASS two edges selected (accent px=$accent, amber px=$amber) and both moved on drag"

echo "== stairs sit on the grid and their edges resize =="
new_level
set_grid 322
xdotool key --clearmodifiers 6
click 550 400
save_as /tmp/ink-ribbon-stairs8.lvl
if ! stairs8=$(grid_flush /tmp/ink-ribbon-stairs8.lvl 8); then
    echo "FAIL stairs are not flush on grid 8 ($stairs8)"; exit 1
fi
xdotool key --clearmodifiers 1
read -r stair_rx stair_ry < <(python3 - /tmp/ink-ribbon-stairs8.lvl <<'PYEOF'
import sys
r = [line.split() for line in open(sys.argv[1]) if line.startswith("stairs ")][0]
x, y, w, h = float(r[1]), float(r[2]), float(r[5]), float(r[6])
print(int(round(x + w + 585)), int(round(y + h / 2 + 411.5)))
PYEOF
)
drag "$stair_rx" "$stair_ry" "$((stair_rx + 40))" "$stair_ry"
save_as /tmp/ink-ribbon-stairs8b.lvl
if ! stair_change=$(stair_edge_drag /tmp/ink-ribbon-stairs8.lvl /tmp/ink-ribbon-stairs8b.lvl 8); then
    echo "FAIL stair edge drag ($stair_change)"; exit 1
fi
set_grid 359
new_level
xdotool key --clearmodifiers 6
click 550 400
save_as /tmp/ink-ribbon-stairs32.lvl
if ! stairs32=$(grid_flush /tmp/ink-ribbon-stairs32.lvl 32); then
    echo "FAIL stairs are not flush on grid 32 ($stairs32)"; exit 1
fi
set_grid 322
echo "PASS stairs flush on grid 8 ($stairs8) and 32 ($stairs32); $stair_change"

echo "== a stair edge meets a wall edge flush =="
new_level
set_grid 322
xdotool key --clearmodifiers 2
click 685 300
click 685 500
xdotool key --clearmodifiers Escape
sleep 0.3
xdotool key --clearmodifiers 6
click 585 400
save_as /tmp/ink-ribbon-flush-before.lvl
read -r flush_rx flush_ry < <(python3 - /tmp/ink-ribbon-flush-before.lvl <<'PYEOF'
import sys
r = [line.split() for line in open(sys.argv[1]) if line.startswith("stairs ")][0]
x, y, w, h = float(r[1]), float(r[2]), float(r[5]), float(r[6])
print(int(round(x + w + 585)), int(round(y + h / 2 + 411.5)))
PYEOF
)
xdotool key --clearmodifiers 1
drag "$flush_rx" "$flush_ry" 685 404
save_as /tmp/ink-ribbon-flush-after.lvl
if ! flush=$(python3 - /tmp/ink-ribbon-flush-after.lvl <<'PYEOF'
import sys
walls = []
stairs = None
for line in open(sys.argv[1]):
    q = line.split()
    if q[0] == "wall":
        walls.append((float(q[3]), float(q[4]), float(q[5]), float(q[6])))
    elif q[0] == "stairs":
        stairs = (float(q[1]), float(q[2]), float(q[5]), float(q[6]))
wall_x = {w[0] for w in walls if w[0] == w[2]} | {w[2] for w in walls if w[0] == w[2]}
x, y, w, h = stairs
right = x + w
if right not in wall_x:
    print(f"stair right {right:g} is not on a wall edge {sorted(wall_x)}")
    raise SystemExit(1)
print(f"stair right edge {right:g} equals the wall edge")
PYEOF
); then
    echo "FAIL stair edge does not meet the wall ($flush)"; exit 1
fi
echo "PASS $flush"

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
