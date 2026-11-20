#!/usr/bin/env python3
"""Generate the re2 main-hall map from the reference screenshot geometry.

Coordinates are reference-image pixels (814x476). The editor preview level is
centred on (393, 246.5) so the startup fit leaves the camera at the origin and
Ctrl+0 renders the plan 1:1 over the reference.

usage:
  python3 tools/gen_ref_map.py --out /tmp/preview.lvl --centered
  python3 tools/gen_ref_map.py --out levels/re2-library-source.lvl
"""
from __future__ import annotations

import argparse

CENTRE = (393.0, 246.5)

# Each entry is a wall loop with a closed flag. Coordinates read off the
# reference screenshot. The look is line-only, so a room's walls are the
# visible outline and no fills are written.
SHAPES: list[tuple[bool, list[tuple[float, float]]]] = [
    # Outer shell of the west building, with the stepped bottom-left corner.
    (True, [(48, 64), (323, 64), (323, 415), (150, 415), (150, 404), (48, 404)]),
    # Main hall: outer rectangle and the inner open-topped U.
    (True, [(323, 111), (519, 111), (519, 429), (323, 429)]),
    (True, [(350, 111), (494, 111), (494, 381), (350, 381)]),
    # West storage room, L shaped.
    (True, [(72, 124), (116, 124), (116, 188), (172, 188), (172, 376), (72, 376)]),
    # Dark room wrapped around the west storage room: its right wall stops at
    # the corridor, and the west storage outline supplies the rest.
    (False, [(72, 124), (72, 88), (158, 88), (158, 139)]),
    (False, [(157, 139), (180, 139)]),
    (False, [(48, 243), (72, 243)]),
    (False, [(180, 64), (180, 96)]),
    # Red room across the top, with the small stair landing cut into it.
    (True, [(244, 64), (323, 64), (323, 139), (180, 139), (180, 115),
            (244, 115)]),
    (True, [(265, 84), (302, 84), (302, 115), (265, 115)]),
    (True, [(180, 96), (210, 96), (210, 115), (180, 115)]),
    # Library block. The left wall opens between the two vestibule walls that
    # join the west storage room, so the ring is drawn as one open polyline.
    (False, [(180, 223), (180, 149), (323, 149), (323, 344), (180, 344),
             (180, 252)]),
    # The little joining vestibule between West Storage and Library.
    (False, [(172, 223), (180, 223)]),
    (False, [(172, 252), (180, 252)]),
    (False, [(180, 336), (200, 336)]),
    (False, [(302, 336), (323, 336)]),
    (True, [(200, 171), (302, 171), (302, 228), (200, 228)]),
    (True, [(200, 247), (302, 247), (302, 336), (200, 336)]),
    # Clock tower, with the lower step shared with the outer shell.
    (True, [(179, 344), (323, 344), (323, 415), (149, 415), (149, 376),
            (179, 376)]),
    # The small clock-tower room outlined inside the lower hall.
    (True, [(229, 363), (265, 363), (265, 397), (229, 397)]),
    # East wing outer boundary including the stair alcove and the balcony.
    (True, [(519, 122), (632, 122), (632, 103), (703, 103), (703, 97),
            (738, 97), (738, 193), (703, 193), (703, 291), (594, 291),
            (594, 233), (519, 233)]),
    # Interior lines of the east wing.
    (False, [(632, 122), (632, 153), (646, 153), (646, 291)]),
    (False, [(646, 193), (703, 193)]),
    (False, [(594, 233), (646, 233)]),
    (False, [(703, 103), (703, 158)]),
    (False, [(720, 117), (720, 158)]),
]

# White door bars in the reference: (x, y) of the bar centre, horizontal flag,
# width in reference pixels.
DOORS: list[tuple[float, float, bool, float]] = [
    (518, 134, False, 10),
    (632, 143, False, 10),
    (322, 179, False, 10),
    (322, 403, False, 10),
    (149, 388, False, 10),
    (660, 193, True, 10),
    (628, 234, True, 10),
    # The three blue doors in the reference, drawn white as requested.
    (72, 138, False, 10),
    (180, 73, False, 10),
    (172, 237, False, 10),
]

# (x0, y0, x1, y1, direction, steps, bottom). The reference draws the top and
# east stairs as two flights around a landing, so each flight is its own run.
STAIRS: list[tuple[float, float, float, float, int, int, int]] = [
    (244, 84, 265, 115, 1, 5, 0),
    (302, 84, 322, 115, 1, 5, 0),
    (302, 286, 323, 315, 1, 9, 1),
    (250, 316, 302, 336, 0, 14, 1),
    (703, 117, 720, 146, 1, 5, 0),
    (720, 117, 737, 157, 1, 13, 0),
]

# Label text top-left in reference pixels and font size.
LABELS: list[tuple[float, float, float, str]] = [
    (50, 248, 13, "West Storage Room"),
    (220, 248, 13, "Library"),
    (396, 272, 13, "Main Hall"),
    (530, 181, 10, "East Storage Room"),
    (655, 233, 11, "Balcony"),
    (188, 373, 12, "Clock Tower"),
]


def wall_point(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="/tmp/preview.lvl")
    ap.add_argument("--centered", action="store_true",
                    help="subtract the map centre so the editor fits it at the origin")
    args = ap.parse_args()

    ox, oy = CENTRE if args.centered else (0.0, 0.0)

    def pt(p):
        return (p[0] - ox, p[1] - oy)

    walls = []          # (shape_id, a, b)
    shapes = []         # (shape_id, closed)
    for sid, (closed, loop) in enumerate(SHAPES, start=1):
        shapes.append((sid, closed))
        for i in range(len(loop) - (0 if closed else 1)):
            a = pt(loop[i])
            b = pt(loop[(i + 1) % len(loop)])
            walls.append((sid, a, b))

    doors = []
    for x, y, horizontal, width in DOORS:
        best = None
        for wi, (_sid, a, b) in enumerate(walls, start=1):
            if horizontal:
                if abs(a[1] - b[1]) > 1e-6 or abs(a[1] - (y - oy)) > 1.0:
                    continue
                lo, hi = sorted((a[0], b[0]))
                if not (lo - 1 <= x - ox <= hi + 1):
                    continue
                t = (x - ox - a[0]) / (b[0] - a[0])
            else:
                if abs(a[0] - b[0]) > 1e-6 or abs(a[0] - (x - ox)) > 1.0:
                    continue
                lo, hi = sorted((a[1], b[1]))
                if not (lo - 1 <= y - oy <= hi + 1):
                    continue
                t = (y - oy - a[1]) / (b[1] - a[1])
            if -0.001 <= t <= 1.001:
                best = (wi, min(max(t, 0.0), 1.0))
                break
        if best is None:
            raise SystemExit(f"no wall hosts door at {(x, y)}")
        doors.append((best[0], best[1], width))

    lines = ["ink-level 3"]
    for sid, closed in shapes:
        lines.append(f"shape {sid} {1 if closed else 0} 0")
    for wi, (sid, a, b) in enumerate(walls, start=1):
        lines.append(f"wall {wi} {sid} {a[0]:.3f} {a[1]:.3f} {b[0]:.3f} {b[1]:.3f}")
    for wi, t, width in doors:
        lines.append(f"door 0 {wi} {t:.4f} {width:.3f}")
    for x0, y0, x1, y1, direction, steps, bottom in STAIRS:
        lines.append(f"stairs {min(x0,x1)-ox:.3f} {min(y0,y1)-oy:.3f} {direction} "
                     f"{steps} {abs(x1-x0):.3f} {abs(y1-y0):.3f} {bottom}")
    for x, y, size, text in LABELS:
        lines.append(f"label {x-ox:.3f} {y-oy:.3f} {size:.3f} {text}")

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {args.out}: {len(shapes)} shapes, {len(walls)} walls, "
          f"{len(doors)} doors, {len(STAIRS)} stairs, {len(LABELS)} labels")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
