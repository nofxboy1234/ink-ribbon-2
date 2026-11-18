#!/usr/bin/env bash
# End-to-end check for the six-tool empty-line editor.
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/xvfb.sh

out=/tmp/ink-ribbon-six
rm -f "$out"-*.png "$out"-*.lvl /tmp/ink-ribbon-empty-*.lvl
echo "== build =="
if ! make -B -j4 >/tmp/ink-ribbon-build.log 2>&1; then tail -30 /tmp/ink-ribbon-build.log; echo "FAIL build"; exit 1; fi
if grep -E "warning:|error:" /tmp/ink-ribbon-build.log; then echo "FAIL build warnings"; exit 1; fi
echo "PASS build without warnings"
if ! make -s test >/tmp/ink-ribbon-test.log 2>&1; then tail -10 /tmp/ink-ribbon-test.log; echo "FAIL core tests"; exit 1; fi
grep -q "ALL CORE TESTS PASSED" /tmp/ink-ribbon-test.log || { echo "FAIL core tests"; exit 1; }
echo "PASS core tests ($(grep -c '^PASS' /tmp/ink-ribbon-test.log) checks)"

# A legacy level: a red-filled closed shape, a standalone room polygon, and a
# red door. The empty look must load it, ignore the fill and the room, and
# draw the door white.
fixture=/tmp/ink-ribbon-empty-legacy.lvl
cat >"$fixture" <<'LVL'
ink-level 3
shape 1 1 2
wall 1 1 0.000 0.000 200.000 0.000
wall 2 1 200.000 0.000 200.000 200.000
wall 3 1 200.000 200.000 0.000 200.000
wall 4 1 0.000 200.000 0.000 0.000
room 1 4 0.000 0.000 200.000 0.000 200.000 200.000 0.000 200.000
door 1 1 0.500 40.000
LVL

xvfb_start
( exec -a "$EDITOR_TAG" ./build/editor --level "$fixture" ) >/tmp/ink-ribbon-six.log 2>&1 &
pid=$!
cleanup() { kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; xvfb_stop; _release_display; test_reap; _run_unregister; }
trap cleanup EXIT INT TERM HUP
win=""
for _ in $(seq 1 80); do win=$(xdotool search --name "Ink Ribbon Level Editor" 2>/dev/null | head -1 || true); [ -n "$win" ] && break; sleep 0.1; done
[ -n "$win" ] || { echo "FAIL window"; exit 1; }
eval "$(xdotool getwindowgeometry --shell "$win" | tr -d '\r')"
WX=$X; WY=$Y
[ "$WIDTH" = "1280" ] && [ "$HEIGHT" = "800" ] || { echo "FAIL window is ${WIDTH}x${HEIGHT}, not 1280x800"; exit 1; }
xdotool windowfocus "$win" 2>/dev/null || true; sleep 0.8

shot() { import -window "$win" "$1"; }
px() { convert "$1" -format "%[hex:p{$2,$3}]" info: 2>/dev/null; }
maxima() { convert "$1" -crop "$2" +repage -colorspace gray -format "%[fx:maxima]" info: 2>/dev/null; }
click() { xdotool mousemove --sync $((WX + $1)) $((WY + $2)) click 1; sleep 0.25; }
hover() { xdotool mousemove --sync $((WX + $1)) $((WY + $2)); sleep 0.4; }
drag() { xdotool mousemove --sync $((WX + $1)) $((WY + $2)); xdotool mousedown 1; sleep 0.15; xdotool mousemove --sync $((WX + $3)) $((WY + $4)); sleep 0.3; xdotool mouseup 1; sleep 0.4; }
key() { xdotool key --clearmodifiers "$1"; sleep 0.3; }
new_level() { click 35 11; click 30 35; sleep 0.5; }
_save_once() {
    local path=$1
    xdotool keydown ctrl; sleep 0.1; xdotool keydown shift; sleep 0.1; xdotool key s; sleep 0.1
    xdotool keyup shift; sleep 0.1; xdotool keyup ctrl; sleep 0.6
    xdotool keydown ctrl; sleep 0.1; xdotool key a; sleep 0.1; xdotool keyup ctrl; sleep 0.1
    xdotool type --clearmodifiers --delay 20 "$path"; sleep 0.3
    xdotool key --clearmodifiers Return; sleep 0.8
    [ -f "$path" ]
}
save_as() { local p=$1; if _save_once "$p"; then return 0; fi; rm -f "$p"; xdotool key Escape; sleep 0.4; _save_once "$p" || { echo "FAIL save $p"; exit 1; }; }
_open_once() {
    local path=$1
    xdotool keydown ctrl; sleep 0.1; xdotool key o; sleep 0.1; xdotool keyup ctrl; sleep 0.6
    xdotool keydown ctrl; sleep 0.1; xdotool key a; sleep 0.1; xdotool keyup ctrl; sleep 0.1
    xdotool type --clearmodifiers --delay 20 "$path"; sleep 0.3
    xdotool key --clearmodifiers Return; sleep 0.8
}
open_level() { _open_once "$1"; sleep 0.3; }
echo "== the empty look ignores legacy fills, rooms, and door colours =="
shot "$out-empty.png"
inside=$(px "$out-empty.png" 585 411)
[ "$inside" = "040404" ] || { echo "FAIL a legacy fill painted the room: $inside"; exit 1; }
wallmax=$(maxima "$out-empty.png" "40x12+280+57")
doorwhite=$(maxima "$out-empty.png" "130x25+520+51")
python3 - "$wallmax" "$doorwhite" <<'PYEOF'
import sys
wall, door = float(sys.argv[1]), float(sys.argv[2])
if not (0.05 < wall < 0.45):
    print(f"walls are not faint: max {wall:.3f}"); raise SystemExit(1)
if door < 0.85:
    print(f"the door is not white: max {door:.3f}"); raise SystemExit(1)
PYEOF
echo "PASS empty room ($inside), faint wall (max $wallmax), white door (max $doorwhite)"
save_as "$out-empty-saved.lvl"
grep -q "^shape 1 1 0$" "$out-empty-saved.lvl" || { echo "FAIL the saved fill column is not 0"; exit 1; }
grep -q "^door 0 1 " "$out-empty-saved.lvl" || { echo "FAIL the saved door is not white"; exit 1; }
if grep -q "^room " "$out-empty-saved.lvl"; then echo "FAIL the room record survived the save"; exit 1; fi
echo "PASS save writes fill 0, door colour 0, and no room record"

echo "== six tools on keys 1 to 6 =="
for k in 1 2 3 4 5 6; do
    key "$k"
    shot "$out-tool-$k.png"
done
python3 - "$out" <<'PYEOF'
import subprocess, sys
base = sys.argv[1]
def hexat(path, x, y):
    out = subprocess.check_output(["convert", path, "-format",
                                   f"%[hex:p{{{x},{y}}}]", "info:"], stderr=subprocess.DEVNULL)
    return out.decode().strip()
def lum(h):
    return int(h[0:2], 16) + int(h[2:4], 16) + int(h[4:6], 16)
for k in range(1, 7):
    path = f"{base}-tool-{k}.png"
    for row in range(1, 7):
        y = 76 + (row - 1) * 26
        active = lum(hexat(path, 165, y)) > 140
        want = row == k
        if active != want:
            print(f"key {k}: row {row} active={active}, want {want}")
            raise SystemExit(1)
print("six tool buttons, one active per key")
PYEOF
echo "PASS six tools activate in order on keys 1 to 6"
key 6; key 7; shot "$out-tool-7.png"
python3 - "$out" <<'PYEOF'
import subprocess, sys
path = sys.argv[1] + "-tool-7.png"
out = subprocess.check_output(["convert", path, "-format", "%[hex:p{165,205}]",
                               "info:"], stderr=subprocess.DEVNULL).decode().strip()
if int(out[0:2], 16) + int(out[2:4], 16) + int(out[4:6], 16) <= 140:
    print("key 7 changed the active tool"); raise SystemExit(1)
PYEOF
echo "PASS key 7 is not a tool"

echo "== rectangles union into one shape with no interior lines =="
new_level
key 3
drag 385 315 585 515
save_as "$out-rect-1.lvl"
python3 - "$out-rect-1.lvl" <<'PYEOF'
import sys
lines = [l.split() for l in open(sys.argv[1])]
walls = [l for l in lines if l[0] == "wall"]
shapes = [l for l in lines if l[0] == "shape"]
if len(shapes) != 1 or len(walls) != 4:
    print(f"first rectangle is {len(shapes)} shapes and {len(walls)} walls")
    raise SystemExit(1)
PYEOF
# No key press: the tool must already be back on Select, so this empty-canvas
# drag is a marquee and must not change a single record.
drag 200 200 300 300
save_as "$out-rect-2.lvl"
python3 - "$out-rect-1.lvl" "$out-rect-2.lvl" <<'PYEOF'
import sys
def records(path):
    return sorted(" ".join(line.split()) for line in open(path)
                  if line.startswith(("wall ", "shape ")))
if records(sys.argv[1]) != records(sys.argv[2]):
    print("a drag after the rectangle action drew or moved a rectangle")
    raise SystemExit(1)
PYEOF
echo "PASS the tool returned to Select after the rectangle action"
key 3
drag 505 415 705 615
save_as "$out-union.lvl"
shot "$out-union.png"
python3 - "$out-union.lvl" <<'PYEOF'
import sys
lines = [l.split() for l in open(sys.argv[1])]
walls = [(float(l[3]), float(l[4]), float(l[5]), float(l[6]))
         for l in lines if l[0] == "wall"]
shapes = [l for l in lines if l[0] == "shape"]
if len(shapes) != 1 or len(walls) != 8:
    print(f"union is {len(shapes)} shapes and {len(walls)} walls, want 1 and 8")
    raise SystemExit(1)
expected = {
    (-200.0, -96.0, 0.0, -96.0), (0.0, -96.0, 0.0, 0.0),
    (0.0, 0.0, 120.0, 0.0), (120.0, 0.0, 120.0, 200.0),
    (120.0, 200.0, -80.0, 200.0), (-80.0, 200.0, -80.0, 104.0),
    (-80.0, 104.0, -200.0, 104.0), (-200.0, 104.0, -200.0, -96.0),
}
def norm(w):
    a = (w[0], w[1])
    b = (w[2], w[3])
    return (a, b) if a <= b else (b, a)
have = {norm(w) for w in walls}
expected = {norm(w) for w in expected}
if have != expected:
    print("union walls are not the two-rectangle outline")
    for w in sorted(have - expected): print(" extra", w)
    for w in sorted(expected - have): print(" missing", w)
    raise SystemExit(1)
# The saved walls are written in loop order, so their starts are the polygon.
n = len(walls)
for i in range(n):
    if (walls[i][2], walls[i][3]) != (walls[(i + 1) % n][0], walls[(i + 1) % n][1]):
        print(f"wall {i} does not meet the next wall")
        raise SystemExit(1)
pts = [(w[0], w[1]) for w in walls]
twice = 0.0
for i in range(n):
    x0, y0 = pts[i]; x1, y1 = pts[(i + 1) % n]
    twice += x0 * y1 - x1 * y0
if abs(twice) / 2 != 200 * 200 + 200 * 200 - 80 * 104:
    print("union area is not the sum of the rectangles minus their overlap")
    raise SystemExit(1)
# No wall may sit strictly inside the union: every wall midpoint is on the
# outline set, and its two sides disagree about membership.
for x0, y0, x1, y1 in walls:
    mx, my = (x0 + x1) / 2, (y0 + y1) / 2
    nx, ny = (my - y0), (x0 - mx)  # unit normal in world units
    ln = (nx * nx + ny * ny) ** 0.5
    nx, ny = nx / ln, ny / ln
    def inside(px, py):
        def in_rect(rx0, ry0, rx1, ry1):
            return rx0 <= px <= rx1 and ry0 <= py <= ry1
        return in_rect(-200, -96, 0, 104) or in_rect(-80, 0, 120, 200)
    side_a = inside(mx + nx * 0.5, my + ny * 0.5)
    side_b = inside(mx - nx * 0.5, my - ny * 0.5)
    if side_a == side_b:
        print(f"wall {(x0, y0, x1, y1)} is not on the union boundary")
        raise SystemExit(1)
print("one shape, eight boundary walls, exact union area")
PYEOF
echo "PASS two overlapping rectangles are one eight-wall shape with no interior lines"
python3 tools/check_map.py "$out-union.lvl"
echo "PASS level structure, doors, stairs, and labels"

# The union must survive save and load.
open_level "$out-union.lvl"
save_as "$out-union-reload.lvl"
python3 - "$out-union.lvl" "$out-union-reload.lvl" <<'PYEOF'
import sys
def records(path):
    return sorted(" ".join(line.split()) for line in open(path)
                  if line.startswith(("wall ", "shape ")))
if records(sys.argv[1]) != records(sys.argv[2]):
    print("the union changed across save, load, and save")
    raise SystemExit(1)
PYEOF
echo "PASS the union round-trips through save and load"

echo "== hover a wall and click a white door =="
# A fresh grid-level rectangle, so the door coordinates are the same every run
# and do not depend on the camera fitted to the union.
new_level
key 3
drag 385 315 585 515
key 4
hover 485 315
shot "$out-door-ghost.png"
ghost=$(maxima "$out-door-ghost.png" "80x30+445+300")
python3 - "$ghost" <<'PYEOF'
import sys
if float(sys.argv[1]) < 0.35:
    print("no door ghost on the hovered wall"); raise SystemExit(1)
PYEOF
click 485 315
save_as "$out-door-1.lvl"
python3 - "$out-door-1.lvl" <<'PYEOF'
import sys
lines = [l.split() for l in open(sys.argv[1])]
doors = [l for l in lines if l[0] == "door"]
walls = {l[1]: l for l in lines if l[0] == "wall"}
if len(doors) != 1:
    print(f"{len(doors)} doors after a click, want 1"); raise SystemExit(1)
door = doors[0]
if door[1] != "0" or float(door[4]) != 32.0:
    print("the door is not white at the panel width", door); raise SystemExit(1)
wall = walls[door[2]]
a = (float(wall[3]), float(wall[4])); b = (float(wall[5]), float(wall[6]))
t = float(door[3])
cx, cy = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
if abs(cx - (-104)) > 0.01 or abs(cy - (-96)) > 0.01:
    print(f"door centre {(cx, cy)} is not the snapped hover point (-104, -96)")
    raise SystemExit(1)
PYEOF
echo "PASS hover ghost, click placed a white 32-unit door at the projected point"
shot "$out-door-white.png"
doorwhite=$(maxima "$out-door-white.png" "50x20+455+305")
python3 - "$doorwhite" <<'PYEOF'
import sys
if float(sys.argv[1]) < 0.85:
    print("the placed door is not white"); raise SystemExit(1)
PYEOF
# Auto-select: a click on another wall must not add a second door.
click 485 515
save_as "$out-door-2.lvl"
python3 - "$out-door-2.lvl" <<'PYEOF'
import sys
doors = [l for l in open(sys.argv[1]) if l.startswith("door ")]
if len(doors) != 1:
    print(f"a click after placing the door added another door ({len(doors)})")
    raise SystemExit(1)
PYEOF
echo "PASS the tool returned to Select after placing the door"

# Q lock: Q toggles, so one press keeps the Door tool across two placements and
# a second press lets the next action hand back to Select.
key 4
key q
sleep 0.4
shot "$out-q-held.png"
qheld=$(convert "$out-q-held.png" -crop 160x14+12+222 +repage -channel B -separate \
    +channel -format "%[fx:maxima]" info: 2>/dev/null)
click 385 415
click 585 415
key q
sleep 0.4
click 485 515
save_as "$out-door-3.lvl"
python3 - "$out-door-3.lvl" "$qheld" <<'PYEOF'
import sys
doors = [l.split() for l in open(sys.argv[1]) if l.startswith("door ")]
if len(doors) < 3:
    print(f"{len(doors)} doors after the Q sequence, want at least 3"); raise SystemExit(1)
if any(d[1] != "0" for d in doors):
    print("a door is not white"); raise SystemExit(1)
if len({d[2] for d in doors}) < 3:
    print("two doors share a wall; the hover points were wrong"); raise SystemExit(1)
if float(sys.argv[2]) < 0.65:
    print("the Q lock indicator did not light while Q was held"); raise SystemExit(1)
PYEOF
echo "PASS Q lock lights, holds the Door tool, and the next unheld action switches"

echo "== stairs drag with a persisted flip =="
key 5
click 22 422
drag 300 620 500 700
save_as "$out-stairs-1.lvl"
xdotool key --clearmodifiers Escape; sleep 0.3
shot "$out-stairs-flip.png"
python3 - "$out-stairs-1.lvl" <<'PYEOF'
import sys
stairs = [l.split() for l in open(sys.argv[1]) if l.startswith("stairs ")]
if len(stairs) != 1:
    print(f"{len(stairs)} stairs after the drag, want 1"); raise SystemExit(1)
s = stairs[0]
if s[3] != "0" or s[4] != "10" or s[7] != "1":
    print("the stair ignored the panel direction, steps, or flip", s)
    raise SystemExit(1)
PYEOF
echo "PASS a stair drag uses the panel direction, steps, and flip"
# Auto-select: a drag after the stair action must marquee, not add a stair.
drag 620 620 700 700
save_as "$out-stairs-2.lvl"
python3 - "$out-stairs-2.lvl" <<'PYEOF'
import sys
stairs = [l for l in open(sys.argv[1]) if l.startswith("stairs ")]
if len(stairs) != 1:
    print(f"a drag after the stair action added a stair ({len(stairs)})")
    raise SystemExit(1)
PYEOF
echo "PASS the tool returned to Select after the stair action"
# Flip the selected stair back from the inspector and prove the flag saves.
click 400 660
click 1048 168
save_as "$out-stairs-3.lvl"
shot "$out-stairs-unflip.png"
python3 - "$out-stairs-3.lvl" <<'PYEOF'
import sys
stairs = [l.split() for l in open(sys.argv[1]) if l.startswith("stairs ")]
if len(stairs) != 1 or stairs[0][7] != "0":
    print("the inspector Flip did not persist", stairs)
    raise SystemExit(1)
PYEOF
echo "PASS the stair flip flag persists in the level"

echo "== losing focus during a line draft must not freeze =="
new_level
key 2
click 400 400
xdotool mousemove --sync $((WX + 520)) $((WY + 300)); sleep 0.3
python3 tools/focus_drop.py >/dev/null; sleep 0.4
kill -0 "$pid" 2>/dev/null || { echo "FAIL editor died"; exit 1; }
rss=$(ps -o rss= -p "$pid" | tr -d ' ')
xdotool windowfocus "$win" 2>/dev/null || true; sleep 1
shot "$out-focus-a.png"; sleep 1; shot "$out-focus-b.png"
xdotool key --clearmodifiers Escape; sleep 0.6; shot "$out-focus-c.png"
metric=$(compare -metric AE "$out-focus-b.png" "$out-focus-c.png" null: 2>&1 || true)
if [ "${metric%% *}" = "0" ]; then echo "FAIL no redraw after focus"; exit 1; fi
echo "PASS focus loss survived (rss=${rss} KB)"

echo "== Ctrl+D duplicates a selected shape =="
new_level
key 3
drag 385 315 585 515
key 1
click 485 415
xdotool keydown ctrl; sleep 0.1; xdotool keydown d; sleep 0.1
xdotool keyup d; sleep 0.1; xdotool keyup ctrl; sleep 0.5
save_as "$out-dup.lvl"
python3 - "$out-dup.lvl" <<'PYEOF'
import sys
lines = [l.split() for l in open(sys.argv[1])]
walls = [l for l in lines if l[0] == "wall"]
shapes = [l for l in lines if l[0] == "shape"]
if len(walls) != 8 or len(shapes) != 2:
    print(f"duplicate is {len(walls)} walls and {len(shapes)} shapes")
    raise SystemExit(1)
copy = [w for w in walls if w[2] == shapes[1][1]]
xs = [float(c) for w in copy for c in (w[3], w[5])]
ys = [float(c) for w in copy for c in (w[4], w[6])]
if min(xs) != -192.0 or max(xs) != 8.0 or min(ys) != -88.0 or max(ys) != 112.0:
    print(f"the copy is not offset by one grid cell: {(min(xs), min(ys), max(xs), max(ys))}")
    raise SystemExit(1)
if shapes[1][2] != "1":
    print("the copied shape is not closed")
    raise SystemExit(1)
print("eight walls, two closed shapes, the copy offset by (8, 8)")
PYEOF
echo "PASS Ctrl+D duplicates the selected shape at one grid offset"

echo "== shift+drag cuts a section out of a wall =="
new_level
key 3
drag 385 315 585 515
# A door inside the section that will be cut, and one outside it.
key 4; hover 465 315; click 465 315
key 4; hover 561 315; click 561 315
key 1
xdotool keydown shift; sleep 0.2
drag 445 315 525 315
xdotool keyup shift; sleep 0.3
shot "$out-span.png"
span=$(px "$out-span.png" 485 315)
[ "${span^^}" = "5AA0C8" ] || { echo "FAIL the span highlight is $span, not the accent colour"; exit 1; }
key Delete
save_as "$out-span.lvl"
python3 - "$out-span.lvl" <<'PYEOF'
import sys
lines = [l.split() for l in open(sys.argv[1])]
walls = [l for l in lines if l[0] == "wall"]
shapes = [l for l in lines if l[0] == "shape"]
doors = [l for l in lines if l[0] == "door"]
if len(walls) != 5 or len(shapes) != 1 or len(doors) != 1:
    print(f"cut level is {len(walls)} walls, {len(shapes)} shapes, {len(doors)} doors")
    raise SystemExit(1)
top = [w for w in walls
       if float(w[4]) == -96.0 and float(w[6]) == -96.0]
if len(top) != 2:
    print(f"the top wall is {len(top)} pieces, want 2")
    raise SystemExit(1)
pieces = sorted((min(float(w[3]), float(w[5])), max(float(w[3]), float(w[5])))
                for w in top)
if pieces[0][1] >= pieces[1][0]:
    print(f"the top wall has no gap: {pieces}")
    raise SystemExit(1)
if abs(pieces[0][1] + 140.0) > 0.01 or abs(pieces[1][0] + 64.0) > 0.01:
    print(f"the gap is not from -140 to -64: {pieces}")
    raise SystemExit(1)
by_id = {w[1]: w for w in walls}
door = doors[0]
w = by_id[door[2]]
ax, ay, bx, by = float(w[3]), float(w[4]), float(w[5]), float(w[6])
t = float(door[3])
cx, cy = ax + (bx - ax) * t, ay + (by - ay) * t
if abs(cx + 24.0) > 0.01 or abs(cy + 96.0) > 0.01:
    print(f"the surviving door moved to {(cx, cy)}, not (-24, -96)")
    raise SystemExit(1)
print("five walls, one shape, the top wall split with a 76 unit gap, one door "
      "inside gone and one outside at (-24, -96)")
PYEOF
echo "PASS shift+drag marks a section, Delete splits the wall, the inside door goes and the outside door keeps its centre"

echo "== shift+drag with Snap off cuts a free-form span =="
new_level
key 3
drag 385 315 585 515
click 21 316
key 1
xdotool keydown shift; sleep 0.2
drag 445 315 525 315
xdotool keyup shift; sleep 0.3
shot "$out-span-free.png"
span=$(px "$out-span-free.png" 485 315)
[ "${span^^}" = "5AA0C8" ] || { echo "FAIL the free span highlight is $span"; exit 1; }
key Delete
save_as "$out-span-free.lvl"
python3 - "$out-span-free.lvl" <<'PYEOF'
import sys
lines = [l.split() for l in open(sys.argv[1])]
walls = [l for l in lines if l[0] == "wall"]
shapes = [l for l in lines if l[0] == "shape"]
if len(walls) != 5 or len(shapes) != 1:
    print(f"free cut level is {len(walls)} walls and {len(shapes)} shapes")
    raise SystemExit(1)
top = [w for w in walls
       if float(w[4]) == -96.0 and float(w[6]) == -96.0]
pieces = sorted((min(float(w[3]), float(w[5])), max(float(w[3]), float(w[5])))
                for w in top)
if len(top) != 2 or pieces[0][1] >= pieces[1][0]:
    print(f"the free cut left {len(top)} top pieces: {pieces}")
    raise SystemExit(1)
if abs(pieces[0][1] + 140.0) > 0.01 or abs(pieces[1][0] + 60.0) > 0.01:
    print(f"the free gap is not the drag's -140 to -60: {pieces}")
    raise SystemExit(1)
print("free-form drag left a gap from -140 to -60 with no grid snap")
PYEOF
echo "PASS Snap off keeps the free-form span endpoints"

echo "== no leftovers =="
cleanup
overflow=$(test_leftovers)
[ -z "$overflow" ] || { echo "FAIL leftover:"; echo "$overflow"; exit 1; }
echo "PASS no leftover processes"
echo "ALL SIX-TOOL CHECKS PASSED"
