#!/usr/bin/env python3
"""Rebuild the reference map in the running editor through real UI input.

The plan in ``levels/re2-library-source.lvl`` is written in reference-image
pixels. This driver maps it to editor world coordinates, then emits or runs
xdotool clicks. It always starts with File > New, turns snapping off, and draws
every shape with the Line tool, so rerunning the same plan produces the same
document. Door, Stairs, and Label use the current tool panels rather than the
old right-click menu.

usage:
  python3 tools/build_map.py --window 2097200 --plan levels/re2-library-source.lvl
  python3 tools/build_map.py --emit --plan levels/re2-library-source.lvl
"""
from __future__ import annotations

import argparse
import math
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass, field

# The plan is in reference pixels. A world point is the reference point minus
# this centre, so File > New (which fits an empty level to 0,0 at zoom 1) maps
# reference (0,0) to screen (192,165) on the shipped 1280x800 layout.
SOURCE_CENTRE = (393.0, 246.5)
CANVAS_CX = 585.0
CANVAS_CY = 411.5
DISPLAY_SIZE = (1280, 800)

# Left panel controls for the current tool. Ctrl-clicking the numeric part of
# a slider opens its text entry.
TOOL_SLIDER = (50, 350)
STAIR_H_BUTTON = (95, 350)
STAIR_V_BUTTON = (95, 375)
STAIR_STEPS = (50, 402)
STAIR_FLIP = (32, 429)


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
    bottom: int = 0


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
                               int(parts[4]), float(parts[5]), float(parts[6]),
                               int(parts[7]) if len(parts) > 7 else 0)
                    )


def world(p: Point) -> Point:
    return Point(p.x - SOURCE_CENTRE[0], p.y - SOURCE_CENTRE[1])


def screen(p: Point) -> tuple[int, int]:
    return (int(math.floor(p.x + CANVAS_CX + 0.5)),
            int(math.floor(p.y + CANVAS_CY + 0.5)))


def wall_point(wall: Wall, t: float) -> Point:
    return Point(wall.a.x + (wall.b.x - wall.a.x) * t,
                 wall.a.y + (wall.b.y - wall.a.y) * t)


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
                          str(x + self.ox), str(y + self.oy)], 0.10)

    def click(self, x: int, y: int, wait: float = 0.18, hover: float = 0.0) -> None:
        self.move(x, y)
        if hover > 0.0:
            self.pause(hover)
        self.add("click", ["xdotool", "click", "1"], wait)

    def key(self, spec: str, wait: float = 0.22) -> None:
        self.add(f"key {spec}", ["xdotool", "key", "--clearmodifiers", spec], wait)

    def key_down(self, spec: str) -> None:
        self.add(f"keydown {spec}", ["xdotool", "keydown", spec], 0.10)

    def key_up(self, spec: str) -> None:
        self.add(f"keyup {spec}", ["xdotool", "keyup", spec], 0.10)

    def type_text(self, text: str) -> None:
        self.add(f"type {text!r}", ["xdotool", "type", "--clearmodifiers",
                                    "--delay", "20", text], 0.15)

    def ctrl_click(self, x: int, y: int) -> None:
        self.key_down("ctrl")
        self.move(x, y)
        self.add("ctrl click", ["xdotool", "click", "1"], 0.18)
        self.key_up("ctrl")

    def set_slider(self, x: int, y: int, value: float) -> None:
        # Ctrl-click opens the slider's text entry with the value selected, so
        # typing replaces it. Do not send ctrl+a: it cancels the entry here.
        self.ctrl_click(x, y)
        self.pause(0.25)
        self.type_text(f"{value:g}")
        self.key("Return")
        self.pause(0.15)

    def drag(self, a: tuple[int, int], b: tuple[int, int]) -> None:
        self.move(*a)
        self.add("press", ["xdotool", "mousedown", "1"], 0.12)
        self.move(*b)
        self.add("release", ["xdotool", "mouseup", "1"], 0.30)

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
    macro.comment("draw every shape with the Line tool and close each loop")
    for shape in plan.shapes.values():
        if not shape.walls:
            continue
        macro.key("2")
        clicks = [shape.walls[0].a] + [w.b for w in shape.walls]
        for point in clicks:
            macro.click(*screen(world(point)))
        if shape.closed:
            macro.click(*screen(world(clicks[0])))
        else:
            macro.key("Escape")
        macro.pause(0.08)


def place_doors(plan: Plan, macro: Macro) -> None:
    macro.comment("set the door width, then hover each host wall and click")
    macro.key("4")
    if plan.doors:
        macro.set_slider(*TOOL_SLIDER, plan.doors[0].width)
    for door in plan.doors:
        wall = plan.walls[door.wall]
        p = world(wall_point(wall, door.t))
        sx, sy = screen(p)
        macro.move(sx, sy)
        macro.pause(0.12)
        macro.add("door click", ["xdotool", "click", "1"], 0.28)
        macro.key("4")
    macro.pause(0.1)


def place_stairs(plan: Plan, macro: Macro) -> None:
    macro.comment("set direction, steps and fade end, then drag each stair footprint")
    flip = False
    for stair in plan.stairs:
        macro.key("5")
        if stair.direction == 0:
            macro.click(*STAIR_H_BUTTON)
        else:
            macro.click(*STAIR_V_BUTTON)
        macro.set_slider(*STAIR_STEPS, stair.steps)
        if bool(stair.bottom) != flip:
            macro.click(*STAIR_FLIP)
            flip = bool(stair.bottom)
        a = world(Point(stair.x, stair.y))
        b = world(Point(stair.x + stair.width, stair.y + stair.length))
        macro.drag(screen(a), screen(b))
        macro.pause(0.1)


def place_labels(plan: Plan, macro: Macro) -> None:
    macro.comment("place labels with the Label tool and type their text")
    for label in plan.labels:
        macro.key("6")
        macro.set_slider(*TOOL_SLIDER, label.size)
        macro.click(*screen(world(Point(label.x, label.y))))
        macro.type_text(label.text)
        macro.key("Return")
        macro.key("Escape")
        macro.pause(0.08)


def save(plan: Plan, macro: Macro, out_path: str) -> None:
    macro.comment("save the new document over the startup level")
    macro.key("Escape")
    macro.key("ctrl+shift+s", wait=0.6)
    macro.key("ctrl+a")
    macro.type_text(out_path)
    macro.key("Return", wait=0.9)
    macro.key("ctrl+s", wait=0.6)
    # Clean canvas for the verification screenshot: one pixel per world unit,
    # no grid, and the Line tool so the Select endpoint handles are gone.
    macro.key("Escape")
    macro.key("ctrl+0", wait=0.4)
    macro.key("g", wait=0.3)
    macro.key("2", wait=0.3)


def build_macro(plan: Plan, origin: tuple[int, int], out_path: str,
                only: set[str] | None = None) -> Macro:
    macro = Macro(origin)
    macro.comment("new level, then snapping off so the plan keeps its pixels")
    macro.click(35, 11)
    macro.click(30, 35, wait=0.6)
    macro.key("s", wait=0.35)
    stages = [
        ("shapes", draw_shapes),
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
    parser.add_argument("--only", help="comma list of shapes,doors,stairs,labels,save")
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
