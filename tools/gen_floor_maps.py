#!/usr/bin/env python3
"""Generate the 1F and 2F reference floor plans.

The plans are traced in the reference screenshot's own pixel coordinates.
Every floor uses the same world scale: a 3F reference pixel.  The measured
registration against the 3F plan is

    3F = 1F - (83, 121)
    3F = 2F - (140, 121)

so the west stairwell, the main-hall stairwell, the library stair and the east
stairwell land on the same world rectangles on every floor.  World coordinates
subtract the same CENTRE that ``gen_ref_map.py`` uses for 3F.

usage:
  python3 tools/gen_floor_maps.py                 # write the two .lvl files
  python3 tools/gen_floor_maps.py --source        # also write *-source.lvl
  python3 tools/gen_floor_maps.py --check         # print stair registrations
"""
from __future__ import annotations

import argparse

CENTRE = (393.0, 246.5)

# Floor reference pixels -> 3F reference pixels.  A point is (x - dx, y - dy).
OFFSETS = {
    "1f": (83.0, 121.0),
    "2f": (140.0, 121.0),
}

# Each floor is a list of closed wall loops in its own reference pixels.  Room
# outlines repeat shared walls; the editor draws them as faint lines and the
# nav grid treats a cell as walkable when any closed loop contains it.
FLOORS: dict[str, dict] = {
    "1f": {
        "shapes": [
            # West wing.  Two narrow corridors separate Operations from the
            # vault and Records from West Office, as in the reference.
            [(132, 189), (250, 189), (250, 335), (175, 335), (175, 300),
             (132, 300)],
            [(132, 300), (175, 300), (175, 335), (132, 335)],
            [(250, 189), (275, 189), (275, 335), (250, 335)],
            [(275, 189), (327, 189), (327, 335), (275, 335)],
            [(280, 194), (320, 194), (320, 250), (280, 250)],
            [(275, 300), (305, 300), (305, 335), (275, 335)],
            [(327, 189), (426, 189), (426, 335), (327, 335)],
            [(327, 300), (350, 300), (350, 335), (327, 335)],
            [(405, 300), (426, 300), (426, 335), (405, 335)],
            [(132, 335), (245, 335), (245, 528), (132, 528)],
            [(245, 335), (275, 335), (275, 442), (245, 442)],
            [(275, 335), (352, 335), (352, 442), (275, 442)],
            [(352, 335), (434, 335), (434, 442), (352, 442)],
            [(352, 442), (426, 442), (426, 528), (352, 528)],
            # Main hall and its balcony railing.
            [(434, 235), (577, 235), (577, 552), (434, 552)],
            [(455, 410), (526, 410), (526, 530), (455, 530)],
            # Entrance corridor and hall, one closed loop with the doorway in
            # the main-hall wall supplied by a door record.
            [(461, 552), (461, 627), (370, 627), (370, 696),
             (643, 696), (643, 627), (550, 627), (550, 552)],
            # East wing.  The interrogation and observation rooms share one
            # column; a corridor runs between them and the press room.
            [(577, 232), (641, 232), (641, 278), (577, 278)],
            [(577, 278), (641, 278), (641, 334), (577, 334)],
            [(577, 334), (641, 334), (641, 382), (577, 382)],
            [(641, 232), (673, 232), (673, 382), (641, 382)],
            [(673, 254), (756, 254), (756, 383), (673, 383)],
            [(787, 254), (925, 254), (925, 382), (787, 382)],
            [(787, 337), (847, 337), (847, 382), (787, 382)],
            [(641, 382), (786, 382), (786, 442), (641, 442)],
            [(786, 392), (848, 392), (848, 442), (786, 442)],
            [(848, 382), (925, 382), (925, 442), (848, 442)],
            [(640, 113), (773, 113), (773, 194), (640, 194)],
            [(710, 113), (773, 113), (773, 194), (710, 194)],
            [(640, 194), (756, 194), (756, 232), (640, 232)],
            [(773, 169), (925, 169), (925, 254), (773, 254)],
            [(756, 194), (773, 194), (773, 254), (756, 254)],
            [(756, 254), (787, 254), (787, 382), (756, 382)],
        ],
        # (x, y, horizontal, width) at the centre of the white door bar.
        "doors": [
            (250, 254, False, 10),
            (275, 254, False, 10),
            (327, 254, False, 10),
            (434, 412, False, 10),
            (245, 400, False, 10),
            (275, 400, False, 10),
            (352, 388, False, 10),
            (426, 499, False, 10),
            (505, 552, True, 76),
            (577, 420, False, 10),
            (641, 255, False, 10),
            (641, 305, False, 10),
            (577, 360, False, 10),
            (673, 300, False, 10),
            (756, 318, False, 10),
            (787, 300, False, 10),
            (787, 359, False, 10),
            (925, 318, False, 10),
            (641, 410, False, 10),
            (786, 416, False, 10),
            (848, 410, False, 10),
            (710, 150, False, 10),
        ],
        # (x0, y0, x1, y1, direction, steps, bottom).  bottom=0 puts the
        # bottom of the run at the max end, so walking toward the min end
        # climbs.  Main-hall, west and east runs match 3F.
        "stairs": [
            (327, 205, 348, 236, 1, 8, 0),
            (385, 205, 405, 236, 1, 8, 0),
            (434, 300, 465, 355, 1, 9, 0),
            (547, 300, 578, 355, 1, 9, 0),
            (835, 197, 907, 222, 0, 8, 0),
            (865, 222, 902, 240, 1, 4, 0),
        ],
        "labels": [
            (136, 258, 10, "Operations Room"),
            (272, 256, 7, "Safety Deposit"),
            (365, 262, 9, "Dark Room"),
            (142, 442, 9, "Records Room"),
            (270, 418, 9, "West Office"),
            (352, 488, 9, "Reception"),
            (475, 390, 14, "Main Hall"),
            (577, 210, 6, "Interrogation"),
            (592, 224, 6, "Room"),
            (577, 284, 6, "Observation"),
            (592, 298, 6, "Room"),
            (680, 312, 9, "Press Room"),
            (790, 358, 7, "Bathroom"),
            (846, 320, 9, "Break Room"),
            (676, 408, 9, "East Office"),
            (787, 420, 7, "Watchman's"),
            (868, 435, 7, "Fire Escape"),
            (642, 168, 9, "Boiler Room"),
            (475, 660, 12, "Entrance"),
        ],
    },
    "2f": {
        "shapes": [
            # West wing.
            [(242, 186), (321, 186), (321, 264), (242, 264)],
            [(190, 186), (242, 186), (242, 264), (190, 264)],
            [(321, 186), (382, 186), (382, 264), (321, 264)],
            [(321, 186), (462, 186), (462, 264), (321, 264)],
            [(258, 264), (321, 264), (321, 336), (258, 336)],
            [(190, 264), (258, 264), (258, 357), (321, 357), (321, 440),
             (190, 440)],
            [(321, 264), (462, 264), (462, 440), (321, 440)],
            [(190, 440), (320, 440), (320, 529), (190, 529)],
            [(320, 440), (462, 440), (462, 529), (320, 529)],
            # Main hall balcony.  The second loop is the open atrium outline.
            [(462, 232), (659, 232), (659, 552), (462, 552)],
            [(522, 292), (602, 292), (602, 360), (634, 360),
             (640, 360), (640, 470), (634, 470), (634, 520), (640, 520),
             (640, 552), (489, 552), (489, 520), (495, 520),
             (495, 470), (489, 470), (489, 360), (522, 360)],
            # East wing.
            [(685, 233), (802, 233), (802, 336), (685, 336)],
            [(802, 232), (984, 232), (984, 350), (802, 350)],
            [(659, 336), (802, 336), (802, 350), (659, 350)],
            [(882, 350), (984, 350), (984, 382), (882, 382)],
            [(659, 350), (820, 350), (820, 440), (659, 440)],
            [(680, 350), (750, 350), (750, 382), (680, 382)],
            [(750, 414), (882, 414), (882, 528), (750, 528)],
            [(659, 440), (750, 440), (750, 490), (659, 490)],
            [(882, 382), (984, 382), (984, 528), (882, 528)],
            [(659, 232), (685, 232), (685, 382), (659, 382)],
            [(802, 350), (882, 350), (882, 382), (802, 382)],
            [(820, 350), (882, 350), (882, 440), (820, 440)],
            [(659, 490), (750, 490), (750, 528), (659, 528)],
        ],
        "doors": [
            (321, 240, False, 10),
            (258, 300, False, 10),
            (321, 380, False, 10),
            (462, 380, False, 10),
            (462, 480, False, 10),
            (659, 310, False, 10),
            (685, 300, False, 10),
            (802, 300, False, 10),
            (659, 390, False, 10),
            (820, 390, False, 10),
            (750, 480, False, 10),
            (882, 420, False, 10),
            (882, 500, False, 10),
            (320, 480, False, 10),
        ],
        "stairs": [
            (490, 284, 521, 359, 1, 9, 0),
            (602, 284, 635, 359, 1, 9, 0),
            (382, 190, 408, 236, 1, 8, 0),
            (382, 440, 439, 452, 0, 8, 1),
            (842, 235, 879, 280, 1, 8, 0),
            (882, 440, 930, 468, 0, 6, 0),
        ],
        "labels": [
            (245, 216, 11, "Shower Room"),
            (265, 285, 11, "Armory"),
            (192, 379, 8, "S.T.A.R.S. Office"),
            (345, 330, 13, "Library"),
            (195, 470, 11, "Linen Room"),
            (325, 490, 12, "Lounge"),
            (530, 405, 14, "Main Hall"),
            (695, 270, 11, "Chief's Office"),
            (900, 270, 13, "Roof"),
            (663, 390, 9, "Private Collection Room"),
            (755, 445, 11, "Art Room"),
            (662, 470, 9, "Waiting Room"),
            (885, 450, 9, "Fire Escape"),
        ],
    },
}


def floor_to_3f(floor: str, p: tuple[float, float]) -> tuple[float, float]:
    dx, dy = OFFSETS[floor]
    return (p[0] - dx, p[1] - dy)


def world(floor: str, p: tuple[float, float]) -> tuple[float, float]:
    x, y = floor_to_3f(floor, p)
    return (x - CENTRE[0], y - CENTRE[1])


def floor_walls(floor: str, shapes):
    walls = []  # (shape_id, a, b)
    shape_records = []
    for sid, loop in enumerate(shapes, start=1):
        shape_records.append((sid, True))
        for i in range(len(loop)):
            a = loop[i]
            b = loop[(i + 1) % len(loop)]
            walls.append((sid, world(floor, a), world(floor, b)))
    return shape_records, walls


def host_door(walls, floor: str, x, y, horizontal, tol=1.5):
    target = world(floor, (x, y))
    best = None
    for wi, (_sid, a, b) in enumerate(walls, start=1):
        if horizontal:
            if abs(a[1] - b[1]) > 1e-6:
                continue
            if abs(a[1] - target[1]) > tol:
                continue
            lo, hi = sorted((a[0], b[0]))
            if not (lo - tol <= target[0] <= hi + tol):
                continue
            t = (target[0] - a[0]) / (b[0] - a[0])
        else:
            if abs(a[0] - b[0]) > 1e-6:
                continue
            if abs(a[0] - target[0]) > tol:
                continue
            lo, hi = sorted((a[1], b[1]))
            if not (lo - tol <= target[1] <= hi + tol):
                continue
            t = (target[1] - a[1]) / (b[1] - a[1])
        if -0.05 <= t <= 1.05:
            best = (wi, min(max(t, 0.0), 1.0))
            break
    if best is None:
        raise SystemExit(f"{floor}: no wall hosts door at {(x, y)}")
    return best



def auto_connect_doors(walls, explicit_doors, floor):
    """Open one 16-unit doorway on every shared wall between two room loops.

    The reference draws a door bar on most partitions, and the nav grid needs
    a physical opening at each one.  The explicit list above places the doors
    that are clearly visible; this pass fills the remaining shared partitions
    so the whole floor is connected without inventing walls.
    """
    doors = []
    for x, y, horizontal, width in explicit_doors:
        wi, t = host_door(walls, floor, x, y, horizontal)
        doors.append([wi, t, max(width, 16.0), 0])
    def covered(p, horizontal):
        for wi, t, _w, _h in doors:
            _sid, a, b = walls[wi - 1]
            qx = a[0] + (b[0] - a[0]) * t
            qy = a[1] + (b[1] - a[1]) * t
            if horizontal:
                if abs(qy - p[1]) < 0.5 and abs(qx - p[0]) < 16.0:
                    return True
            else:
                if abs(qx - p[0]) < 0.5 and abs(qy - p[1]) < 16.0:
                    return True
        return False
    # group collinear walls with their shape id
    vertical = {}
    horizontal = {}
    for wi, (sid, a, b) in enumerate(walls, start=1):
        if abs(a[0] - b[0]) < 1e-6:
            vertical.setdefault(round(a[0], 3), []).append(
                (min(a[1], b[1]), max(a[1], b[1]), sid, wi))
        elif abs(a[1] - b[1]) < 1e-6:
            horizontal.setdefault(round(a[1], 3), []).append(
                (min(a[0], b[0]), max(a[0], b[0]), sid, wi))
    def overlaps(entries, horizontal_flag):
        entries = sorted(entries)
        for i in range(len(entries)):
            for j in range(i + 1, len(entries)):
                a0, a1, sa, _wa = entries[i]
                b0, b1, sb, _wb = entries[j]
                if sa == sb:
                    continue
                lo = max(a0, b0)
                hi = min(a1, b1)
                if hi - lo < 24.0:
                    continue
                mid = (lo + hi) * 0.5
                p = (mid, 0.0) if horizontal_flag else (0.0, mid)
                if covered(p, horizontal_flag):
                    continue
                doors.append([_wa, 0.0, 16.0, 1])
                # remember the point through the temporary host entry
                _sid, wa, wb = walls[_wa - 1]
                if horizontal_flag:
                    t = (mid - wa[0]) / (wb[0] - wa[0])
                else:
                    t = (mid - wa[1]) / (wb[1] - wa[1])
                doors[-1][1] = t
                return True
        return False
    # Keep adding one door per shared run until no uncovered run remains.
    for _ in range(200):
        added = False
        for x, entries in vertical.items():
            if not any(e[2] != entries[0][2] for e in entries):
                continue
            cand = []
            for i in range(len(entries)):
                for j in range(i + 1, len(entries)):
                    a0, a1, sa, _ = entries[i]
                    b0, b1, sb, _ = entries[j]
                    if sa == sb:
                        continue
                    lo, hi = max(a0, b0), min(a1, b1)
                    if hi - lo >= 24.0:
                        cand.append(((lo + hi) * 0.5, _))
            for mid, wi in cand:
                if covered((x, mid), False):
                    continue
                wa, wb = walls[wi - 1][1], walls[wi - 1][2]
                t = (mid - wa[1]) / (wb[1] - wa[1])
                doors.append([wi, t, 16.0, 1])
                added = True
                break
            if added:
                break
        if added:
            continue
        for y, entries in horizontal.items():
            cand = []
            for i in range(len(entries)):
                for j in range(i + 1, len(entries)):
                    a0, a1, sa, _ = entries[i]
                    b0, b1, sb, _ = entries[j]
                    if sa == sb:
                        continue
                    lo, hi = max(a0, b0), min(a1, b1)
                    if hi - lo >= 24.0:
                        cand.append(((lo + hi) * 0.5, _))
            for mid, wi in cand:
                if covered((mid, y), True):
                    continue
                wa, wb = walls[wi - 1][1], walls[wi - 1][2]
                t = (mid - wa[0]) / (wb[0] - wa[0])
                doors.append([wi, t, 16.0, 1])
                added = True
                break
            if added:
                break
        if not added:
            break
    return doors


def build_floor(floor: str) -> list[str]:
    data = FLOORS[floor]
    shapes, walls = floor_walls(floor, data["shapes"])
    lines = ["ink-level 3"]
    for sid, closed in shapes:
        lines.append(f"shape {sid} {1 if closed else 0} 0")
    for wi, (sid, a, b) in enumerate(walls, start=1):
        lines.append(f"wall {wi} {sid} {a[0]:.3f} {a[1]:.3f} {b[0]:.3f} {b[1]:.3f}")
    for wi, t, width, hidden in auto_connect_doors(walls, data["doors"], floor):
        lines.append(f"door 0 {wi} {t:.4f} {width:.3f} {hidden}")
    for x0, y0, x1, y1, direction, steps, bottom in data["stairs"]:
        p0 = world(floor, (min(x0, x1), min(y0, y1)))
        p1 = world(floor, (max(x0, x1), max(y0, y1)))
        lines.append(
            f"stairs {p0[0]:.3f} {p0[1]:.3f} {direction} {steps} "
            f"{p1[0] - p0[0]:.3f} {p1[1] - p0[1]:.3f} {bottom} 3 0.000 0.680"
        )
    for x, y, size, text in data["labels"]:
        p = world(floor, (x, y))
        lines.append(f"label {p[0]:.3f} {p[1]:.3f} {size:.3f} {text}")
    return lines


def build_source(floor: str) -> list[str]:
    """The same document, but in 3F reference pixels (for build_map.py)."""
    data = FLOORS[floor]
    shapes, walls = floor_walls(floor, data["shapes"])
    lines = ["ink-level 3"]
    for sid, closed in shapes:
        lines.append(f"shape {sid} {1 if closed else 0} 0")
    for wi, (sid, a, b) in enumerate(walls, start=1):
        # floor_walls already applied the floor transform; add CENTRE back.
        lines.append(
            f"wall {wi} {sid} {a[0] + CENTRE[0]:.3f} {a[1] + CENTRE[1]:.3f} "
            f"{b[0] + CENTRE[0]:.3f} {b[1] + CENTRE[1]:.3f}"
        )
    for x, y, horizontal, width in data["doors"]:
        wi, t = host_door(walls, floor, x, y, horizontal)
        lines.append(f"door 0 {wi} {t:.4f} {max(width, 16.0):.3f} 0")
    for x0, y0, x1, y1, direction, steps, bottom in data["stairs"]:
        p0 = world(floor, (min(x0, x1), min(y0, y1)))
        p1 = world(floor, (max(x0, x1), max(y0, y1)))
        lines.append(
            f"stairs {p0[0] + CENTRE[0]:.3f} {p0[1] + CENTRE[1]:.3f} {direction} "
            f"{steps} {p1[0] - p0[0]:.3f} {p1[1] - p0[1]:.3f} {bottom} 3 0.000 0.680"
        )
    for x, y, size, text in data["labels"]:
        p = world(floor, (x, y))
        lines.append(
            f"label {p[0] + CENTRE[0]:.3f} {p[1] + CENTRE[1]:.3f} "
            f"{size:.3f} {text}"
        )
    return lines


def stair_registration() -> None:
    for floor in ("1f", "2f"):
        print(floor)
        for s in FLOORS[floor]["stairs"]:
            x0, y0, x1, y1 = s[:4]
            a = floor_to_3f(floor, (min(x0, x1), min(y0, y1)))
            b = floor_to_3f(floor, (max(x0, x1), max(y0, y1)))
            print(f"  ({a[0]:.0f},{a[1]:.0f})-({b[0]:.0f},{b[1]:.0f})")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-dir", default="levels")
    ap.add_argument("--source", action="store_true",
                    help="also write the 3F-pixel source plans")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    if args.check:
        stair_registration()
        return 0
    for floor in ("1f", "2f"):
        path = f"{args.out_dir}/re2-library-{floor}.lvl"
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(build_floor(floor)) + "\n")
        print(f"wrote {path}: {len(FLOORS[floor]['shapes'])} shapes, "
              f"{len(FLOORS[floor]['doors'])} doors, "
              f"{len(FLOORS[floor]['stairs'])} stairs")
        if args.source:
            spath = f"{args.out_dir}/re2-library-{floor}-source.lvl"
            with open(spath, "w", encoding="utf-8") as f:
                f.write("\n".join(build_source(floor)) + "\n")
            print(f"wrote {spath}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
