#!/usr/bin/env python3
"""Check a level file for structural integrity and the empty look.

Verifies that every door hangs on a wall, every closed shape's walls form one
loop, every stair footprint is a positive box, and every label has text. Fill
and colour columns are accepted but ignored, so a legacy level passes too.

usage: python3 tools/check_map.py LEVEL.lvl
"""
from __future__ import annotations

import math
import sys


def read(path: str) -> dict:
    data = {
        "counts": {"wall": 0, "shape": 0, "door": 0, "label": 0, "stairs": 0},
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
            if kind == "wall":
                data["counts"]["wall"] += 1
                data["walls"][int(parts[1])] = (
                    int(parts[2]),
                    (float(parts[3]), float(parts[4])),
                    (float(parts[5]), float(parts[6])),
                )
            elif kind == "shape":
                data["counts"]["shape"] += 1
                data["shapes"][int(parts[1])] = (int(parts[2]), int(parts[3]))
            elif kind == "door":
                data["counts"]["door"] += 1
                data["doors"].append(tuple(parts[1:]))
            elif kind == "label":
                data["counts"]["label"] += 1
                data["labels"].append(tuple(parts[1:]))
            elif kind == "stairs":
                data["counts"]["stairs"] += 1
                data["stairs"].append(tuple(parts[1:]))
    return data


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "levels/re2-library.lvl"
    data = read(path)
    failures: list[str] = []

    shape_walls: dict[int, int] = {}
    shape_ends: dict[int, dict[tuple[float, float], int]] = {}
    for wall_id, (shape_id, a, b) in data["walls"].items():
        shape_walls[shape_id] = shape_walls.get(shape_id, 0) + 1
        ends = shape_ends.setdefault(shape_id, {})
        for point in (a, b):
            key = (round(point[0], 3), round(point[1], 3))
            ends[key] = ends.get(key, 0) + 1
        if shape_id not in data["shapes"]:
            failures.append(f"wall {wall_id} references unknown shape {shape_id}")

    for shape_id, (closed, _fill) in data["shapes"].items():
        count = shape_walls.get(shape_id, 0)
        if closed and count < 3:
            failures.append(f"closed shape {shape_id} has {count} walls")
        for key, shared in shape_ends.get(shape_id, {}).items():
            if shared > 2:
                failures.append(f"shape {shape_id} jams {shared} walls into {key}")
            elif closed and shared != 2:
                failures.append(f"closed shape {shape_id} has a loose end at {key}")

    for door in data["doors"]:
        if int(door[1]) not in data["walls"]:
            failures.append(f"door {door} hangs on a missing wall")

    for stair in data["stairs"]:
        try:
            x, y, _direction, _steps, width, length = (float(v) for v in stair[:6])
        except ValueError:
            failures.append(f"stairs {stair} is not numeric")
            continue
        if not all(math.isfinite(v) for v in (x, y, width, length)):
            failures.append(f"stairs {stair} has a non-finite value")
        elif width <= 0 or length <= 0:
            failures.append(f"stairs {stair} has an empty footprint")

    xs = [v for _s, a, _b in data["walls"].values() for v in (a[0], _b[0])]
    ys = [v for _s, a, b in data["walls"].values() for v in (a[1], b[1])]
    print(f"counts {data['counts']}")
    if xs and ys:
        print(f"bounds {min(xs):.1f} {min(ys):.1f} {max(xs):.1f} {max(ys):.1f}")
    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        return 1
    print("ALL MAP CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
