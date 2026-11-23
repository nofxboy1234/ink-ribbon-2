#!/usr/bin/env python3
"""Compare a floor's visible editor linework with its reference image.

Builds a mask of the walls the editor actually draws (detail walls plus
visible collision walls, skipping nav-only walls), rasterises it in reference
pixels, and measures how much of the reference linework it covers.

usage:
  python3 tools/floor_parity.py 1f
  python3 tools/floor_parity.py all
"""
from __future__ import annotations

import argparse
import os
import sys

import cv2
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF_DIR = "/home/dylan/Downloads/re2-map/2D-map/23112026"
CENTRE = (393.0, 246.5)
OFFSETS = {"1f": (83.0, 121.0), "2f": (140.0, 121.0), "3f": (0.0, 0.0)}
PATHS = {
    "1f": os.path.join(ROOT, "levels", "re2-library-1f.lvl"),
    "2f": os.path.join(ROOT, "levels", "re2-library-2f.lvl"),
    "3f": os.path.join(ROOT, "levels", "re2-library.lvl"),
}
BOUNDS = {"1f": (120, 175, 935, 710), "2f": (180, 175, 995, 540),
          "3f": (0, 0, 1246, 931)}


def reference_mask(floor: str) -> np.ndarray:
    img = cv2.imread(os.path.join(REF_DIR, floor + ".png"))
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    edges = cv2.Canny(cv2.GaussianBlur(gray, (3, 3), 0), 8, 30)
    b, g, r = cv2.split(img)
    mx = np.maximum(np.maximum(b, g), r)
    mn = np.minimum(np.minimum(b, g), r)
    grayline = ((mx.astype(np.int16) - mn.astype(np.int16) < 12) &
                (mx > 40)).astype(np.uint8) * 255
    mask = cv2.bitwise_or(edges, grayline)
    x0, y0, x1, y1 = BOUNDS[floor]
    outside = np.ones_like(mask) * 255
    outside[y0:y1, x0:x1] = 0
    mask[outside > 0] = 0
    kernel = np.ones((3, 1), np.uint8)
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel)
    return mask


def model_mask(floor: str) -> np.ndarray:
    img = cv2.imread(os.path.join(REF_DIR, floor + ".png"))
    canvas = np.zeros(img.shape[:2], np.uint8)
    cx, cy = CENTRE
    ox, oy = OFFSETS[floor]
    for line in open(PATHS[floor], encoding="utf-8"):
        p = line.split()
        if not p or p[0] != "wall":
            continue
        flag = int(p[7]) if len(p) > 7 else 0
        if flag == 2:
            continue
        x1, y1, x2, y2 = (float(v) for v in p[3:7])
        a = (int(round(x1 + cx + ox)), int(round(y1 + cy + oy)))
        b = (int(round(x2 + cx + ox)), int(round(y2 + cy + oy)))
        cv2.line(canvas, a, b, 255, 3)
    return canvas


def compare(floor: str) -> tuple[float, float, str]:
    ref = reference_mask(floor)
    model = model_mask(floor)
    # Reference coverage: how much of the reference linework is near a model
    # line. Precision: how much of the model linework is near a reference line.
    dist_ref = cv2.distanceTransform(255 - ref, cv2.DIST_L2, 3)
    dist_model = cv2.distanceTransform(255 - model, cv2.DIST_L2, 3)
    ref_px = ref > 0
    model_px = model > 0
    coverage = float((dist_model[ref_px] <= 4.0).mean()) if ref_px.any() else 0.0
    precision = float((dist_ref[model_px] <= 4.0).mean()) if model_px.any() else 0.0
    img = cv2.imread(os.path.join(REF_DIR, floor + ".png"))
    overlay = img.copy()
    overlay[ref_px] = (0, 0, 255)
    overlay[model_px] = (0, 255, 0)
    out = os.path.join(ROOT, "docs", "screenshots", f"floor-{floor}-parity.png")
    cv2.imwrite(out, overlay)
    return coverage, precision, out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("floor", choices=["1f", "2f", "3f", "all"])
    args = ap.parse_args()
    floors = ["1f", "2f", "3f"] if args.floor == "all" else [args.floor]
    for floor in floors:
        coverage, precision, out = compare(floor)
        print(f"{floor}: reference coverage {coverage:.3f}, "
              f"model precision {precision:.3f}, {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
