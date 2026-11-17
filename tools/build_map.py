#!/usr/bin/env python3
"""Rebuild the reference map in the running editor through real UI input.

The saved level is the plan. Every coordinate is scaled by half, snapped to the
4-unit grid, and mapped through the editor canvas transform, then emitted as
xdotool commands or run against the live window. The run always starts with
File > New, so rerunning the same plan produces the same document.

usage:
  python3 tools/build_map.py --window 2097200 --plan levels/re2-library-source.lvl
  python3 tools/build_map.py --emit --plan levels/re2-library-source.lvl

The canvas transform is the shipped 1280x800 layout: 190 px left panel, 300 px
right panel, a 23 px menu bar, zoom 1 and a zero camera centre. A click at
screen (585 + wx, 411.5 + wy) lands on world (wx, wy).
"""
from __future__ import annotations

import argparse
import math
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass, field

GRID = 4.0
SCALE = 0.5
SOURCE_CENTRE = (600.0, 340.0)
CANVAS_CX = 585.0
CANVAS_CY = 411.5
DISPLAY_SIZE = (1280, 800)

# Offsets from the right-click point into the ImGui popup, measured on the
# real 1280x800 window. Menu rows are 23 px apart; a submenu opens 155 px to
# the right and its first row lines up with the parent row.
MENU_EDGE = 5
MENU_ROW0 = 12
MENU_ROW_H = 23
MENU_ITEM_X = 25
SUBMENU_X = 215
FIXED_CLICK = (400, 300)


@dataclass(frozen=True)
class Point:
    x: float
    y: float


@dataclass
class Wall:
    id: int
    shape: int
    a: Point
    b: Point


@dataclass
class Shape:
    id: int
    closed: bool
    fill: int
    walls: list[Wall] = field(default_factory=list)


@dataclass
class Door:
    color: int
    wall: int
    t: float
    width: float


@dataclass
class Label:
    x: float
    y: float
    size: float
    text: str


@dataclass
class Stairs:
    x: float
    y: float
    direction: int
    steps: int
    width: float
    length: float


class Plan:
    def __init__(self, path: str):
        self.path = path
        self.shapes: dict[int, Shape] = {}
        self.walls: dict[int, Wall] = {}
        self.doors: list[Door] = []
        self.labels: list[Label] = []
        self.stairs: list[Stairs] = []
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                parts = line.split()
                if not parts:
                    continue
                kind = parts[0]
                if kind == "shape":
                    sid, closed, fill = int(parts[1]), int(parts[2]), int(parts[3])
                    self.shapes[sid] = Shape(sid, closed == 1, fill)
                elif kind == "wall":
                    wid, sid = int(parts[1]), int(parts[2])
                    a = Point(float(parts[3]), float(parts[4]))
                    b = Point(float(parts[5]), float(parts[6]))
                    wall = Wall(wid, sid, a, b)
                    self.walls[wid] = wall
                    self.shapes.setdefault(sid, Shape(sid, False, 0)).walls.append(wall)
                elif kind == "door":
                    self.doors.append(
                        Door(int(parts[1]), int(parts[2]), float(parts[3]), float(parts[4]))
                    )
                elif kind == "label":
                    self.labels.append(
                        Label(float(parts[1]), float(parts[2]), float(parts[3]),
                              " ".join(parts[4:]))
                    )
                elif kind == "stairs":
                    self.stairs.append(
                        Stairs(float(parts[1]), float(parts[2]), int(parts[3]),
                               int(parts[4]), float(parts[5]), float(parts[6]))
                    )


def snap(value: float) -> float:
    """Nearest grid line, ties toward +infinity, matching the half-scale plan."""
    return math.floor(value / GRID + 0.5) * GRID


def world(p: Point) -> Point:
    return Point(snap((p.x - SOURCE_CENTRE[0]) * SCALE),
                 snap((p.y - SOURCE_CENTRE[1]) * SCALE))


def screen(p: Point) -> tuple[int, int]:
    return (int(math.floor(p.x + CANVAS_CX + 0.5)),
            int(math.floor(p.y + CANVAS_CY + 0.5)))


def area_centroid(points: list[Point]) -> Point:
    area = 0.0
    cx = 0.0
    cy = 0.0
    for i, p in enumerate(points):
        q = points[(i + 1) % len(points)]
        cross = p.x * q.y - q.x * p.y
        area += cross
        cx += (p.x + q.x) * cross
        cy += (p.y + q.y) * cross
    area *= 0.5
    if abs(area) < 1e-6:
        raise ValueError("degenerate polygon")
    return Point(cx / (6.0 * area), cy / (6.0 * area))


def inside(points: list[Point], p: Point) -> bool:
    result = False
    for i, a in enumerate(points):
        b = points[(i + 1) % len(points)]
        if (a.y > p.y) != (b.y > p.y):
            x = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x
            if p.x < x:
                result = not result
    return result


def shape_polygon(shape: Shape) -> list[Point]:
    points = [shape.walls[0].a]
    points.extend(w.b for w in shape.walls)
    if len(points) > 1 and points[0] == points[-1]:
        points.pop()
    return points


def wall_point(wall: Wall, t: float) -> Point:
    return Point(wall.a.x + (wall.b.x - wall.a.x) * t,
                 wall.a.y + (wall.b.y - wall.a.y) * t)


def clip_wall(wall: Wall, rect: tuple[float, float, float, float]):
    """Same axis-aligned clip the editor uses; returns (t0, t1) or None."""
    x0, y0, x1, y1 = rect
    t0, t1 = 0.0, 1.0
    dx = wall.b.x - wall.a.x
    dy = wall.b.y - wall.a.y
    if abs(dx) < 1e-4:
        if wall.a.x < x0 or wall.a.x > x1:
            return None
    else:
        a = (x0 - wall.a.x) / dx
        b = (x1 - wall.a.x) / dx
        if a > b:
            a, b = b, a
        t0 = max(t0, a)
        t1 = min(t1, b)
    if abs(dy) < 1e-4:
        if wall.a.y < y0 or wall.a.y > y1:
            return None
    else:
        a = (y0 - wall.a.y) / dy
        b = (y1 - wall.a.y) / dy
        if a > b:
            a, b = b, a
        t0 = max(t0, a)
        t1 = min(t1, b)
    if t1 - t0 <= 1e-4:
        return None
    return t0, t1


def point_segment_dist(p: Point, a: Point, b: Point) -> float:
    dx = b.x - a.x
    dy = b.y - a.y
    length2 = dx * dx + dy * dy
    if length2 <= 0.0:
        return math.hypot(p.x - a.x, p.y - a.y)
    t = max(0.0, min(1.0, ((p.x - a.x) * dx + (p.y - a.y) * dy) / length2))
    return math.hypot(p.x - (a.x + dx * t), p.y - (a.y + dy * t))


class Macro:
    """A list of xdotool commands, each with a post-command pause."""

    def __init__(self, origin: tuple[int, int]):
        self.ox, self.oy = origin
        self.steps: list[tuple[str, list[str] | None, float]] = []

    def add(self, description: str, args: list[str] | None, wait: float) -> None:
        self.steps.append((description, args, wait))

    def comment(self, text: str) -> None:
        self.steps.append((text, None, 0.0))

    def pause(self, seconds: float) -> None:
        self.add("wait", None, seconds)

    def move(self, x: int, y: int) -> None:
        self.add("move", ["xdotool", "mousemove", "--sync",
                          str(x + self.ox), str(y + self.oy)], 0.08)

    def click(self, x: int, y: int, wait: float = 0.28, hover: float = 0.0) -> None:
        self.move(x, y)
        if hover > 0.0:
            self.pause(hover)
        self.add("click", ["xdotool", "click", "1"], wait)

    def right_click(self, x: int, y: int) -> None:
        self.move(x, y)
        self.add("right click", ["xdotool", "click", "3"], 0.5)

    def key(self, spec: str, wait: float = 0.3) -> None:
        self.add(f"key {spec}", ["xdotool", "key", "--clearmodifiers", spec], wait)

    def key_down(self, spec: str) -> None:
        self.add(f"keydown {spec}", ["xdotool", "keydown", spec], 0.12)

    def key_up(self, spec: str) -> None:
        self.add(f"keyup {spec}", ["xdotool", "keyup", spec], 0.12)

    def type_text(self, text: str) -> None:
        self.add(f"type {text!r}", ["xdotool", "type", "--clearmodifiers",
                                    "--delay", "25", text], 0.25)

    def drag(self, a: tuple[int, int], b: tuple[int, int], shift: bool = False) -> None:
        if shift:
            self.key_down("shift")
        self.move(*a)
        self.add("press", ["xdotool", "mousedown", "1"], 0.15)
        self.move(*b)
        self.add("release", ["xdotool", "mouseup", "1"], 0.35)
        if shift:
            self.key_up("shift")

    def emit(self) -> str:
        lines: list[str] = []
        for description, args, wait in self.steps:
            if args is None and wait == 0.0:
                lines.append(f"# {description}")
                continue
            if args is None:
                lines.append(f"sleep {wait:g}")
                continue
            lines.append(" ".join(shlex.quote(arg) for arg in args))
            if wait > 0.0:
                lines.append(f"sleep {wait:g}")
        return "\n".join(lines)

    def run(self) -> None:
        for description, args, wait in self.steps:
            if args is None:
                if wait > 0.0:
                    time.sleep(wait)
                continue
            subprocess.run(args, check=True)
            if wait > 0.0:
                time.sleep(wait)


def find_window() -> tuple[str, int, int]:
    found = subprocess.run(["xdotool", "search", "--name", "Ink Ribbon Level Editor"],
                           capture_output=True, text=True, check=True)
    windows = [w for w in found.stdout.split() if w]
    if not windows:
        raise SystemExit("no Ink Ribbon Level Editor window on $DISPLAY")
    window = windows[0]
    geometry = subprocess.run(["xdotool", "getwindowgeometry", "--shell", window],
                              capture_output=True, text=True, check=True).stdout
    values: dict[str, str] = {}
    for line in geometry.splitlines():
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip()
    x = int(values.get("X", 0))
    y = int(values.get("Y", 0))
    width = int(values.get("WIDTH", 0))
    height = int(values.get("HEIGHT", 0))
    if (width, height) != DISPLAY_SIZE:
        raise SystemExit(f"window is {width}x{height}, want {DISPLAY_SIZE[0]}x{DISPLAY_SIZE[1]}")
    return window, x, y


def draw_shapes(plan: Plan, macro: Macro) -> None:
    macro.comment("draw every shape, closing each loop on its first vertex")
    for shape in plan.shapes.values():
        if not shape.walls:
            continue
        clicks = [shape.walls[0].a] + [w.b for w in shape.walls]
        for point in clicks:
            macro.click(*screen(world(point)))
        if not shape.closed:
            macro.key("Escape")
        macro.pause(0.15)


def apply_fills(plan: Plan, macro: Macro) -> None:
    macro.comment("fill closed shapes through the right-click menu")
    for shape in plan.shapes.values():
        if shape.fill == 0 or not shape.closed:
            continue
        polygon = [world(p) for p in shape_polygon(shape)]
        centroid = area_centroid(polygon)
        if not inside(polygon, centroid):
            raise SystemExit(f"shape {shape.id}: centroid is outside its polygon")
        cx, cy = screen(centroid)
        macro.right_click(cx, cy)
        macro.click(cx + MENU_ITEM_X, cy + MENU_ROW0, hover=0.2)
        sub_row = 0 if shape.fill == 2 else 1  # Red, Blue, None
        macro.click(cx + SUBMENU_X, cy + MENU_ROW0 + MENU_ROW_H * sub_row,
                    wait=0.45, hover=0.3)


def door_rect(door: Door, plan: Plan) -> tuple[float, float, float, float]:
    wall = plan.walls[door.wall]
    wall = Wall(wall.id, wall.shape, world(wall.a), world(wall.b))
    centre = wall_point(wall, door.t)
    half = door.width * SCALE * 0.5
    if abs(wall.b.x - wall.a.x) > abs(wall.b.y - wall.a.y):
        return (snap(centre.x - half), wall.a.y, snap(centre.x + half), wall.a.y)
    return (wall.a.x, snap(centre.y - half), wall.a.x, snap(centre.y + half))


def place_doors(plan: Plan, macro: Macro) -> None:
    macro.comment("marquee each door span, then right-click for a coloured door")
    for door in plan.doors:
        rect = door_rect(door, plan)
        a = screen(Point(rect[0], rect[1]))
        b = screen(Point(rect[2], rect[3]))
        macro.drag(a, b, shift=True)
        macro.right_click(*FIXED_CLICK)
        macro.click(FIXED_CLICK[0] + MENU_ITEM_X, FIXED_CLICK[1] + MENU_ROW0,
                    hover=0.2)
        macro.click(FIXED_CLICK[0] + SUBMENU_X,
                    FIXED_CLICK[1] + MENU_ROW0 + MENU_ROW_H * door.color,
                    wait=0.45, hover=0.3)


def stair_rect(stair: Stairs) -> tuple[float, float, float, float]:
    a = world(Point(stair.x, stair.y))
    b = world(Point(stair.x + stair.width, stair.y + stair.length))
    return (a.x, a.y, b.x, b.y)


def place_stairs(plan: Plan, macro: Macro) -> None:
    macro.comment("drag each stair box from its bottom edge to its top edge")
    for stair in plan.stairs:
        x0, y0, x1, y1 = stair_rect(stair)
        # Bottom-left to top-right: the wide axis picks the direction, and the
        # run stays the dominant extent at half scale.
        macro.drag(screen(Point(x0, y1)), screen(Point(x1, y0)), shift=True)
        macro.right_click(*FIXED_CLICK)
        # Fill (from the shell under the click) or Door (from a touched wall)
        # owns row 0, so Stairs is always row 1 on this map.
        macro.click(FIXED_CLICK[0] + MENU_ITEM_X,
                    FIXED_CLICK[1] + MENU_ROW0 + MENU_ROW_H, wait=0.55, hover=0.25)


def set_label_size(macro: Macro, size: float) -> None:
    macro.key_down("ctrl")
    macro.click(95, 249)
    macro.key_up("ctrl")
    macro.type_text(f"{size:g}")
    macro.key("Return")


def place_labels(plan: Plan, macro: Macro) -> None:
    macro.comment("place labels with the Label tool and type their text")
    macro.key("3")
    current = None
    for label in plan.labels:
        size = label.size * SCALE
        if current != size:
            set_label_size(macro, size)
            current = size
        macro.click(*screen(world(Point(label.x, label.y))))
        macro.type_text(label.text)
        macro.key("Return")
        macro.pause(0.1)


def save(plan: Plan, macro: Macro, out_path: str) -> None:
    macro.comment("save the new document over the startup level")
    macro.key("Escape")
    macro.key("ctrl+shift+s", wait=0.6)
    macro.key("ctrl+a")
    macro.type_text(out_path)
    macro.key("Return", wait=0.9)
    macro.key("ctrl+s", wait=0.6)


def build_macro(plan: Plan, origin: tuple[int, int], out_path: str,
                only: set[str] | None = None) -> Macro:
    macro = Macro(origin)
    macro.comment("new level, grid 4, snap stays on")
    macro.click(35, 11)
    macro.click(30, 35, wait=0.6)
    macro.click(50, 300)
    macro.click(50, 326, wait=0.5)
    stages = [
        ("shapes", draw_shapes),
        ("fills", apply_fills),
        ("doors", place_doors),
        ("stairs", place_stairs),
        ("labels", place_labels),
    ]
    for name, stage in stages:
        if only is None or name in only:
            stage(plan, macro)
    if only is None or "save" in only:
        save(plan, macro, out_path)
    return macro


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", default="levels/re2-library-source.lvl")
    parser.add_argument("--out", default="levels/re2-library.lvl")
    parser.add_argument("--window", help="window id; defaults to the editor title")
    parser.add_argument("--origin", default="0,0",
                        help="window origin for --emit, default 0,0")
    parser.add_argument("--emit", action="store_true", help="print xdotool commands")
    parser.add_argument("--only", help="comma list of shapes,fills,doors,stairs,labels,save")
    args = parser.parse_args()
    only = set(args.only.split(",")) if args.only else None

    plan = Plan(args.plan)
    if args.emit:
        ox, oy = (int(v) for v in args.origin.split(","))
        print(build_macro(plan, (ox, oy), args.out, only).emit())
        return 0

    window, ox, oy = find_window()
    subprocess.run(["xdotool", "windowfocus", window], check=False)
    time.sleep(0.6)
    macro = build_macro(plan, (ox, oy), args.out, only)
    macro.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
