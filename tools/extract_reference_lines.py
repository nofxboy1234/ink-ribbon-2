#!/usr/bin/env python3
"""Extract axis-aligned reference linework from the 1F and 2F map images.

The editor keeps two plans per floor: a hidden collision plan made of closed
room loops, and this visible reference plan.  This script turns the reference
screenshots into a JSON list of horizontal/vertical segments in reference
pixels.  ``gen_floor_maps.py`` emits those as detail walls.

usage:
  python3 tools/extract_reference_lines.py
  python3 tools/extract_reference_lines.py --out tools/floor-reference-lines.json
"""
from __future__ import annotations

import argparse
import json
import os

import cv2
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF_DIR = "/home/dylan/Downloads/re2-map/2D-map/23112026"

# Reference-image bounds that hold the floor plan, plus the label rectangles
# used to drop letter strokes.  Labels are text, not architecture.
FLOORS = {
    "1f": {
        "bounds": (120, 175, 935, 710),
        "ui": [(0, 840, 1246, 931), (1090, 250, 1246, 570),
               (555, 360, 820, 440)],
        "cursor": (510, 0, 535, 931),
        "labels": [
            (130, 245, 250, 270), (270, 240, 350, 275),
            (355, 248, 420, 275), (132, 430, 250, 456),
            (262, 408, 350, 432), (350, 476, 430, 500),
            (465, 380, 555, 405), (575, 200, 640, 232),
            (575, 275, 640, 310), (675, 300, 760, 328),
            (785, 348, 850, 370), (835, 310, 900, 335),
            (670, 398, 760, 422), (785, 412, 850, 432),
            (855, 420, 920, 442), (635, 158, 710, 182),
            (465, 650, 520, 672),
        ],
    },
    "2f": {
        "bounds": (180, 175, 995, 540),
        "ui": [(0, 840, 1294, 909), (1100, 250, 1294, 570)],
        "cursor": (570, 0, 592, 909),
        "labels": [
            (235, 205, 320, 230), (255, 276, 320, 300),
            (190, 370, 325, 395), (335, 320, 405, 345),
            (185, 460, 280, 485), (315, 480, 400, 505),
            (520, 395, 600, 422), (680, 260, 790, 285),
            (885, 260, 940, 286), (650, 382, 825, 405),
            (745, 435, 825, 460), (655, 460, 735, 485),
            (875, 442, 950, 465),
        ],
    },
}


def extract(floor: str) -> list[list[float]]:
    path = os.path.join(REF_DIR, floor + ".png")
    img = cv2.imread(path)
    if img is None:
        raise SystemExit("missing reference image " + path)
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    edges = cv2.Canny(cv2.GaussianBlur(gray, (3, 3), 0), 8, 30)
    # The reference draws many room walls as low-saturation gray lines that
    # Canny misses in the dark fills. Add those bright, gray pixels.
    b, g, r = cv2.split(img)
    mx = np.maximum(np.maximum(b, g), r)
    mn = np.minimum(np.minimum(b, g), r)
    graylines = ((mx.astype(np.int16) - mn.astype(np.int16) < 12) &
                 (mx > 40)).astype(np.uint8) * 255
    edges = cv2.bitwise_or(edges, graylines)
    cfg = FLOORS[floor]
    x0, y0, x1, y1 = cfg["bounds"]
    for a, b, c, d in cfg["ui"]:
        edges[max(0, b):d, max(0, a):c] = 0
    cx0, cy0, cx1, cy1 = cfg["cursor"]
    edges[max(0, cy0):cy1, max(0, cx0):cx1] = 0

    lines = cv2.HoughLinesP(edges, 1, np.pi / 1800, threshold=14,
                            minLineLength=8, maxLineGap=5)
    segs: list[list[float]] = []
    if lines is not None:
        for raw in lines:
            ax, ay, bx, by = [int(v) for v in np.ravel(raw)]
            if abs(ax - bx) <= 2:
                segs.append(["v", (ax + bx) / 2, min(ay, by), max(ay, by)])
            elif abs(ay - by) <= 2:
                segs.append(["h", (ay + by) / 2, min(ax, bx), max(ax, bx)])
    h_img, w_img = gray.shape
    segs = [s for s in segs
            if not (s[0] == "v" and s[3] - s[2] > 0.7 * h_img)
            and not (s[0] == "h" and s[3] - s[2] > 0.7 * w_img)]
    bounded = []
    for s in segs:
        if s[0] == "h":
            if y0 - 4 <= s[1] <= y1 + 4 and x0 - 4 <= s[2] <= x1 + 4 and s[3] <= x1 + 4:
                bounded.append(s)
        else:
            if x0 - 4 <= s[1] <= x1 + 4 and y0 - 4 <= s[2] <= y1 + 4 and s[3] <= y1 + 4:
                bounded.append(s)
    segs = bounded
    # Drop short letter strokes that sit inside a known label box.
    kept = []
    for s in segs:
        orient, c, lo, hi = s
        if hi - lo < 14:
            continue
        mx = (lo + hi) / 2
        if orient == "h":
            px, py = mx, c
        else:
            px, py = c, mx
        in_label = any(lx0 <= px <= lx1 and ly0 <= py <= ly1
                       for lx0, ly0, lx1, ly1 in cfg["labels"])
        if in_label and hi - lo < 28:
            continue
        kept.append(s)

    def merge(entries: list[list[float]]) -> list[list[float]]:
        out: list[list[float]] = []
        for key, lo, hi in sorted(entries):
            placed = False
            for s in out:
                if abs(s[0] - key) <= 2.5 and not (hi < s[1] - 12 or lo > s[2] + 12):
                    s[1] = min(s[1], lo)
                    s[2] = max(s[2], hi)
                    s[0] = (s[0] + key) / 2
                    placed = True
                    break
            if not placed:
                out.append([key, lo, hi])
        return out

    horizontal = merge([[s[1], s[2], s[3]] for s in kept if s[0] == "h"])
    vertical = merge([[s[1], s[2], s[3]] for s in kept if s[0] == "v"])
    # Close small extraction gaps at corners: if a wall end is within a few
    # pixels of a perpendicular wall, extend it to meet that wall.
    for _ in range(3):
        for h in horizontal:
            hy, hx0, hx1 = h
            for v in vertical:
                vx, vy0, vy1 = v
                if not (vy0 - 3 <= hy <= vy1 + 3):
                    continue
                if abs(vx - hx0) <= 10 and vx < hx0:
                    h[1] = vx
                if abs(vx - hx1) <= 10 and vx > hx1:
                    h[2] = vx
        for v in vertical:
            vx, vy0, vy1 = v
            for h in horizontal:
                hy, hx0, hx1 = h
                if not (hx0 - 3 <= vx <= hx1 + 3):
                    continue
                if abs(hy - vy0) <= 10 and hy < vy0:
                    v[1] = hy
                if abs(hy - vy1) <= 10 and hy > vy1:
                    v[2] = hy
    result = [["h", round(c, 1), round(lo, 1), round(hi, 1)]
              for c, lo, hi in horizontal if hi - lo >= 12]
    result += [["v", round(c, 1), round(lo, 1), round(hi, 1)]
               for c, lo, hi in vertical if hi - lo >= 12]
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=os.path.join(
        ROOT, "tools", "floor-reference-lines.json"))
    args = ap.parse_args()
    data = {floor: extract(floor) for floor in ("1f", "2f")}
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1)
    for floor, segs in data.items():
        print(f"{floor}: {len(segs)} reference segments -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
