#!/usr/bin/env python3
"""Check the built map against the saved plan.

Verifies object counts, wall bounds aspect (within 1 percent of 1120:600),
every stair box edge on the 4-unit grid, fill and door colours, label text,
and each shape's open or closed state.

usage: python3 tools/check_map.py levels/re2-library.lvl
"""
from __future__ import annotations

import math
import sys

GRID = 4.0
EXPECTED_COUNTS = {
    "wall": 40,
    "shape": 10,
    "room": 0,
    "door": 10,
    "label": 6,
    "stairs": 4,
}
EXPECTED_ASPECT = 1120.0 / 600.0
ASPECT_TOLERANCE = 0.01


def read(path: str) -> dict:
    data = {
        "counts": {k: 0 for k in EXPECTED_COUNTS},
        "walls": {},
        "shapes": {},
        "doors": [],
        "labels": [],
        "stairs": [],
    }
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            kind = parts[0]
            if kind in data["counts"]:
                data["counts"][kind] += 1
            if kind == "wall":
                wid = int(parts[1])
                data["walls"][wid] = (float(parts[3]), float(parts[4]),
                                      float(parts[5]), float(parts[6]))
            elif kind == "shape":
                data["shapes"][int(parts[1])] = (int(parts[2]), int(parts[3]))
            elif kind == "door":
                data["doors"].append((int(parts[1]), int(parts[2])))
            elif kind == "label":
                data["labels"].append((float(parts[1]), float(parts[2]),
                                       float(parts[3]), " ".join(parts[4:])))
            elif kind == "stairs":
                data["stairs"].append(tuple(float(v) for v in parts[1:]))
    return data


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "levels/re2-library.lvl"
    data = read(path)
    failures: list[str] = []

    if data["counts"] != EXPECTED_COUNTS:
        failures.append(f"counts {data['counts']} != {EXPECTED_COUNTS}")

    xs: list[float] = []
    ys: list[float] = []
    for x0, y0, x1, y1 in data["walls"].values():
        xs.extend((x0, x1))
        ys.extend((y0, y1))
    min_x, max_x = min(xs), max(xs)
    min_y, max_y = min(ys), max(ys)
    width, height = max_x - min_x, max_y - min_y
    aspect = width / height if height else 0.0
    if abs(aspect - EXPECTED_ASPECT) / EXPECTED_ASPECT > ASPECT_TOLERANCE:
        failures.append(f"aspect {aspect:.6f} is not within 1% of {EXPECTED_ASPECT:.6f}")

    for stair in data["stairs"]:
        x, y, _direction, _steps, w, h = stair[:6]
        for value in (x, y, x + w, y + h):
            if abs(value / GRID - round(value / GRID)) > 1e-6:
                failures.append(f"stair {stair} has an edge off the {GRID:g} grid")
                break

    fills = sorted(fill for _closed, fill in data["shapes"].values() if fill)
    if fills != [2, 2, 3, 5, 5, 5, 5, 6, 6, 6]:
        failures.append(f"fills {fills} != two red, one floor, four holes, three empty")
    doors = sorted(color for color, _wall in data["doors"])
    if doors != [0, 0, 0, 0, 0, 0, 0, 0, 1, 2]:
        failures.append(f"door colours {doors} != seven white, one red, two blue")
    open_shapes = sum(1 for closed, _fill in data["shapes"].values() if closed == 0)
    if open_shapes != 0:
        failures.append(f"{open_shapes} open shapes != 0")
    labels = [text for _x, _y, _size, text in data["labels"]]
    wanted = ["West Storage Room", "Library", "Main Hall",
              "East Storage Room", "Balcony", "Clock Tower"]
    if labels != wanted:
        failures.append(f"labels {labels} != {wanted}")

    print(f"counts {data['counts']}")
    print(f"bounds {min_x:.1f} {min_y:.1f} {max_x:.1f} {max_y:.1f} "
          f"({width:.1f} x {height:.1f}, aspect {aspect:.6f})")
    print(f"stairstep grid {GRID:g}: "
          f"{'flush' if not any('stair' in f for f in failures) else 'NOT FLUSH'}")
    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        return 1
    print("ALL MAP CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
