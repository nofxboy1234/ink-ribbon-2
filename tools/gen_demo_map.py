#!/usr/bin/env python3
"""Generate the reference-inspired demo level.

The level is a floor plan in world units, one unit per pixel at zoom 1.
Run from the repo root: python3 tools/gen_demo_map.py
"""
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "levels" / "re2-library.lvl"

GRID = 8
STAIR_STEP_DEPTH = 10
STAIR_WIDTH = 44

walls = []
shapes = []
doors = []
rooms = []
labels = []
stairs = []


def begin_shape():
    shapes.append({"walls": [], "closed": False})


def end_shape(closed=False):
    shapes[-1]["closed"] = closed


def wall(a, b):
    if not shapes:
        begin_shape()
    walls.append((a, b))
    shapes[-1]["walls"].append(len(walls))
    return len(walls)


def box(x0, y0, x1, y1):
    begin_shape()
    first = wall((x0, y0), (x1, y0))
    wall((x1, y0), (x1, y1))
    wall((x1, y1), (x0, y1))
    wall((x0, y1), (x0, y0))
    end_shape(closed=True)
    return first


def door(wall_index, t, width, color):
    doors.append((wall_index, t, width, color))


def door_at(a, b, point, width, color):
    for i, (wa, wb) in enumerate(walls, start=1):
        if (wa, wb) not in ((a, b), (b, a)):
            continue
        ax, ay = wa
        bx, by = wb
        px, py = point
        if ax == bx:
            t = (py - ay) / (by - ay)
        else:
            t = (px - ax) / (bx - ax)
        door(i, round(t, 4), width, color)
        return
    raise SystemExit(f"door host wall not found: {a} -> {b}")


def room(points, color):
    # A room is a closed line shape carrying its own fill, so its walls draw
    # bright and the area inside is painted.
    begin_shape()
    for i in range(len(points)):
        wall(points[i], points[(i + 1) % len(points)])
    end_shape(closed=True)
    shapes[-1]["fill"] = color


def label(x, y, size, text):
    labels.append((x, y, size, text))


def snap_up(value):
    return -(-value // GRID) * GRID


def snap_near(value):
    return int(round(value / GRID)) * GRID


def stair(x, y, direction, steps):
    """Place a stair near (x, y), the old centre, with a grid-flush box."""
    run = steps * STAIR_STEP_DEPTH
    width = snap_up(run if direction == "h" else STAIR_WIDTH)
    length = snap_up(STAIR_WIDTH if direction == "h" else run)
    stairs.append((snap_near(x - width / 2), snap_near(y - length / 2),
                   direction, steps, width, length))


# Left building shell.
box(40, 40, 520, 640)
# West storage, L shaped.
begin_shape()
wall((90, 120), (240, 120))
wall((240, 120), (240, 180))
wall((240, 180), (300, 180))
wall((300, 180), (300, 430))
wall((300, 430), (90, 430))
wall((90, 430), (90, 120))
end_shape(closed=True)
# Top right red room.
box(340, 60, 500, 160)
# Library.
box(330, 250, 470, 430)
# Small dark room above the library.
box(170, 180, 240, 260)
# Clock tower.
box(250, 500, 390, 610)
# Main hall.
box(540, 120, 830, 620)
# East storage.
box(860, 90, 1130, 380)
# Balcony.
box(1000, 380, 1160, 560)
# Corridor divider above the library.
begin_shape()
wall((330, 160), (500, 160))
wall((500, 160), (500, 250))
end_shape()
begin_shape()
wall((330, 430), (330, 520))
wall((330, 520), (500, 520))
end_shape()

# Painted areas. The reference uses dark desaturated fills under thin walls.
room([(45, 45), (335, 45), (335, 165), (305, 165), (305, 245),
      (245, 245), (245, 185), (175, 185), (175, 335), (85, 335),
      (85, 125), (45, 125)], "blue")
room([(90, 120), (240, 120), (240, 180), (300, 180), (300, 430), (90, 430)], "red")
room([(345, 65), (495, 65), (495, 155), (345, 155)], "red")
room([(335, 165), (495, 165), (495, 245), (335, 245)], "blue")
room([(330, 430), (500, 430), (500, 515), (330, 515)], "blue")
room([(335, 255), (465, 255), (465, 425), (335, 425)], "blue")

# Doors. White doors are the common type, the blue and red ones mark locked routes.
door_at((540, 120), (540, 620), (540, 260), 44, "white")
door_at((830, 120), (830, 620), (830, 260), 44, "white")
door_at((860, 380), (860, 90), (860, 240), 40, "white")
door_at((1000, 380), (1000, 560), (1000, 450), 34, "white")
door_at((330, 250), (330, 430), (330, 340), 34, "white")
door_at((90, 430), (300, 430), (180, 430), 36, "blue")
door_at((300, 180), (300, 430), (300, 250), 34, "blue")
door_at((340, 60), (340, 160), (340, 110), 32, "red")
door_at((500, 60), (500, 160), (500, 110), 32, "white")
door_at((250, 500), (390, 500), (320, 500), 40, "white")

# Text labels.
label(96, 300, 19, "West Storage Room")
label(356, 300, 19, "Library")
label(600, 380, 19, "Main Hall")
label(892, 215, 18, "East Storage Room")
label(1030, 440, 18, "Balcony")
label(278, 540, 18, "Clock Tower")

# Stairs: the run follows the step count, with the box snapped to the grid.
stair(420, 110, "h", 10)
stair(430, 495, "h", 8)
stair(1090, 165, "v", 9)
stair(700, 90, "h", 12)

lines = ["ink-level 3"]
fill_index = {"none": 0, "blue": 1, "red": 2}
for sid, shape in enumerate(shapes, start=1):
    lines.append(f"shape {sid} {1 if shape['closed'] else 0} {fill_index[shape.get('fill', 'none')]}")
for i, (a, b) in enumerate(walls, start=1):
    sid = next(sid for sid, shape in enumerate(shapes, start=1) if i in shape["walls"])
    lines.append(f"wall {i} {sid} {a[0]:.3f} {a[1]:.3f} {b[0]:.3f} {b[1]:.3f}")
for points, color in rooms:
    nums = " ".join(f"{v:.3f}" for p in points for v in p)
    lines.append(f"room {1 if color == 'red' else 0} {len(points)} {nums}")
for wall_index, t, width, color in doors:
    ci = {"white": 0, "red": 1, "blue": 2}[color]
    lines.append(f"door {ci} {wall_index} {t:.4f} {width:.3f}")
for x, y, size, text in labels:
    lines.append(f"label {x:.3f} {y:.3f} {size:.3f} {text}")
for x, y, direction, steps, width, length in stairs:
    lines.append(f"stairs {x:.3f} {y:.3f} {0 if direction == 'h' else 1} "
                 f"{steps} {width:.3f} {length:.3f}")

OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text("\n".join(lines) + "\n")
print(f"wrote {OUT} with {len(shapes)} shapes, {len(walls)} walls, {len(rooms)} rooms, {len(doors)} doors, "
      f"{len(labels)} labels, {len(stairs)} stairs")
