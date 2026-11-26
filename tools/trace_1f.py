#!/usr/bin/env python3
"""Trace the 1F plan from the reference onion skin.

The 1F reference image is a 1246x931 screenshot of the in-game map.  Its
linework was extracted at native resolution and every wall below is stored in
those same reference pixels, so the editor's image onion lands on it at 1:1.
World coordinates subtract the same origin the other floors use:

    world = 1F reference pixel - (476, 367.5)

All segments are horizontal or vertical.  The reference's rounded entrance
corners and its few diagonal transitions are stepped with H/V runs.  Stair
hatching is not wall linework: it is rendered by the editor's Stairs objects.

usage:
  python3 tools/trace_1f.py             # write levels/re2-library-1f.lvl
  python3 tools/trace_1f.py --source    # also write the 3F-pixel source plan
"""
from __future__ import annotations

import argparse
from collections import defaultdict

# 1F reference pixel -> world.  The onion image fit in the .lvl is the same
# rectangle, so the trace sits one reference pixel per world unit.
ORIGIN = (476.0, 367.5)
# 3F reference pixels use the measured registration 3F = 1F - (83, 121).
SOURCE_SHIFT = (83.0, 121.0)

# Horizontal walls: (x0, x1, y).
H = [
    # Boiler Room / north-east service block.
    (634, 772, 113), (633, 650, 145), (668, 711, 145),
    (773, 924, 170), (633, 773, 194),
    # West wing north wall and the Operations Room alcove.
    (132, 143, 189), (154, 276, 189), (276, 343, 189), (343, 407, 190),
    (156, 188, 212), (216, 252, 212), (287, 298, 193),
    # Safety Deposit, Dark Room and the corridor below them.
    (276, 327, 193), (275, 327, 334), (327, 426, 335),
    # Main Hall top and inner hall/shoulder walls.
    (434, 570, 236), (426, 434, 236), (567, 781, 227),
    (577, 642, 280), (295, 427, 382), (334, 427, 460),
    (426, 468, 500), (426, 495, 442), (461, 493, 566),
    (500, 515, 562), (528, 551, 566), (534, 546, 441), (530, 546, 446),
    (577, 589, 516), (577, 589, 540), (588, 622, 546),
    # West wing lower rooms.
    (156, 180, 351), (179, 214, 377), (162, 236, 311),
    (170, 245, 499), (282, 334, 499), (137, 427, 529),
    (240, 251, 334), (228, 240, 323), (275, 297, 322),
    (236, 250, 298), (352, 389, 240), (352, 407, 343),
    # Main Hall bottom and the entrance corridor / hall.
    (434, 465, 553), (547, 578, 553),
    (370, 462, 627), (550, 644, 627),
    (378, 493, 696), (528, 635, 696),
    (370, 378, 691), (635, 644, 691),
    # East wing service rooms.
    (672, 757, 255), (755, 787, 308), (588, 674, 334),
    (641, 703, 382), (730, 756, 382), (786, 849, 382),
    (786, 840, 338), (787, 822, 238), (787, 822, 244),
    (787, 822, 250), (787, 822, 256), (787, 806, 261),
    (787, 805, 266), (787, 805, 271), (787, 804, 278),
    (804, 821, 291), (787, 804, 301),
    # Break Room, Bathroom and the mid-east corridor.
    (848, 925, 238), (829, 925, 221), (712, 829, 221), (910, 925, 221),
    (848, 883, 351), (869, 902, 284), (804, 821, 291),
    (816, 855, 406), (823, 854, 412), (832, 854, 420),
    # The long corridor above East Office.
    (567, 901, 393),
    # East Office, Watchman's Room and Fire Escape.
    (609, 790, 443), (786, 855, 443), (855, 925, 429),
    (855, 925, 441), (855, 925, 435), (816, 855, 484),
    (620, 786, 529), (786, 855, 521), (855, 925, 529),
    (620, 665, 483),
]

# Vertical walls: (y0, y1, x).
V = [
    # West outer wall.
    (188, 356, 133), (362, 387, 133), (386, 530, 136),
    # Operations Room and its corridor.
    (211, 251, 157), (251, 348, 157), (188, 251, 250), (251, 335, 250),
    (298, 311, 236),
    (188, 245, 276), (245, 335, 276),
    (240, 292, 406), (190, 240, 406),
    # Safety Deposit / Dark Room.
    (193, 335, 327), (382, 409, 276), (409, 474, 269),
    # West corridor and West Office.
    (350, 419, 180), (323, 344, 240), (358, 410, 240),
    (408, 440, 245), (453, 499, 245), (461, 510, 332),
    (236, 382, 426), (399, 468, 426), (499, 529, 426),
    (409, 469, 245),
    # Records Room partitions.
    (418, 436, 170), (472, 498, 170),
    # Main Hall.
    (235, 292, 434), (362, 376, 434), (408, 467, 434), (499, 554, 434),
    (235, 554, 434), (236, 554, 577), (405, 488, 577),
    (362, 397, 578), (503, 554, 578),
    (552, 627, 464), (552, 628, 548),
    (393, 427, 566), (333, 404, 589), (432, 478, 589),
    # Entrance hall.
    (626, 687, 370), (626, 686, 644), (687, 691, 370),
    (691, 696, 378), (686, 691, 644), (691, 696, 635),
    # Dark Room / west-east corridor.
    (239, 382, 353), (190, 236, 426), (269, 298, 368), (279, 300, 372),
    (279, 298, 380), (279, 300, 384),
    # East wing.
    (254, 383, 673), (254, 384, 756), (252, 326, 641), (333, 367, 640),
    (326, 367, 640), (301, 343, 787), (358, 383, 787),
    (339, 394, 848), (265, 322, 849), (381, 426, 816), (450, 484, 816),
    (283, 325, 902), (340, 392, 902),
    (221, 291, 820), (236, 281, 790), (236, 281, 796),
    (236, 281, 800), (235, 301, 805), (236, 257, 812),
    (236, 257, 816), (221, 291, 820),
    # Break Room / north-east perimeter.
    (238, 394, 848), (238, 351, 925), (351, 436, 925),
    (171, 182, 925), (199, 222, 925), (255, 286, 925),
    # East Office / Watchman's / Fire Escape.
    (424, 547, 620), (458, 522, 786), (443, 529, 786),
    (429, 529, 855),
    (436, 529, 925), (483, 521, 686),
    (391, 429, 888),
    # Boiler Room.
    (114, 195, 634), (113, 196, 712), (120, 170, 773),
    (170, 194, 773),
]

# Visible stair runs.  (x0, y0, x1, y1, direction, steps, flip)
STAIRS = [
    # One visible flight in the north-west stairwell.
    (349, 188, 380, 211, 0, 8, 0),
    # The three east-wing flights.
    (787, 238, 822, 301, 1, 11, 0),
    (837, 200, 883, 222, 0, 9, 0),
    (865, 233, 902, 250, 0, 6, 0),
    # Main Hall's two flights.
    (434, 297, 466, 355, 1, 10, 0),
    (547, 297, 578, 355, 1, 10, 0),
    # Fire-escape flights.
    (873, 465, 913, 482, 0, 7, 0),
    (895, 483, 913, 500, 1, 3, 0),
]
# Registration-only stairwells: these are the west stairwell rectangles that
# every floor shares.  The 1F reference does not draw them on this map, so the
# treads are transparent.  They keep the cross-floor contract without showing
# a stair where the onion skin has none.
REGISTRATION_STAIRS = [
    (327, 205, 348, 236, 1, 8, 0),
    (385, 205, 405, 236, 1, 8, 0),
]

# Non-colliding reference lines: counters, shelving and the stepped balcony.
DETAIL = [
    ('H', 156, 180, 351), ('H', 179, 214, 377),
    ('H', 228, 240, 323), ('H', 275, 297, 322),
    ('V', 323, 336, 302),
    ('H', 848, 883, 351), ('H', 869, 902, 284),
    ('H', 816, 855, 406), ('H', 823, 854, 412), ('H', 832, 854, 420),
    ('H', 620, 665, 483), ('V', 483, 521, 686),
    ('H', 855, 925, 435),
    ('H', 577, 589, 516), ('H', 577, 589, 540), ('H', 588, 622, 546),
    ('H', 500, 515, 562), ('H', 461, 493, 566), ('H', 528, 551, 566),
]
DETAIL_KEYS = set(DETAIL)

# Door bars from the reference image, in reference pixels: (x, y, w, h).
# Blue, red, and white bars all mark a door bay; the editor draws every door
# white, so the trace restores the missing wall span and hosts a visible door
# there.  Two white marks inside the Typewriter callout are text, not doors.
DOOR_BARS = [
    (192, 218, 22, 4), (392, 237, 11, 5), (306, 333, 11, 4),
    (785, 345, 4, 12), (283, 381, 11, 4), (706, 380, 22, 4),
    (425, 387, 4, 10), (839, 480, 11, 4), (331, 513, 4, 11),
    (162, 311, 11, 4), (239, 346, 4, 11), (620, 450, 3, 21),
    (784, 459, 3, 11), (853, 507, 4, 11), (496, 564, 21, 4),
    (496, 695, 21, 4), (856, 282, 11, 4), (218, 376, 10, 3),
    (887, 520, 10, 3),
]

# Navigation-only openings (not drawn) where the reference has a door bar but
# the traced wall line is continuous.  (axis, wall coordinate, position, width)
NAV_DOORS = [
    ('V', 855, 500, 16),   # Watchman's Room <-> Fire Escape
    ('V', 855, 445, 16),   # corridor <-> Fire Escape
    ('V', 786, 480, 16),   # East Office <-> Watchman's Room
    ('V', 620, 480, 16),   # corridor <-> East Office
    ('H', 443, 800, 16),   # corridor <-> Watchman's Room
    ('H', 194, 700, 16),   # Boiler Room <-> north service corridor
    ('V', 712, 150, 16),   # Boiler Room's two halves
    ('H', 221, 820, 16),   # north service block <-> east corridor
    ('H', 221, 900, 16),   # north service block <-> Break Room
    ('V', 577, 300, 16),   # Main Hall <-> east corridor
    ('V', 577, 420, 16),
    ('V', 577, 500, 16),
    ('V', 434, 300, 16),   # Main Hall <-> west corridor
    ('V', 434, 420, 16),
    ('V', 434, 500, 16),
    ('V', 426, 300, 16),
    ('V', 426, 420, 16),
    ('V', 426, 500, 16),
    ('V', 578, 300, 16),
    ('V', 578, 420, 16),
    ('V', 578, 500, 16),
    ('V', 787, 480, 16),
    ('V', 784, 480, 16),
    ('V', 589, 300, 16),
    ('V', 589, 420, 16),
    ('V', 589, 500, 16),
]

# Labels: (x, y, size, text) in reference pixels, measured against the onion.
LABELS = [
    (155, 247, 14, "Operations Room"),
    (275, 247, 13, "Safety Deposit"),
    (300, 266, 13, "Room"),
    (365, 250, 13, "Dark-"),
    (365, 266, 13, "Room"),
    (139, 439, 13, "Records Room"),
    (306, 412, 13, "West Office"),
    (359, 490, 13, "Reception"),
    (480, 384, 18, "Main Hall"),
    (573, 234, 12, "Interrogation"),
    (596, 250, 12, "Room"),
    (574, 281, 12, "Observation"),
    (596, 297, 12, "Room"),
    (685, 312, 13, "Press Room"),
    (786, 350, 12, "Bathroom"),
    (843, 314, 13, "Break Room"),
    (685, 445, 13, "East Office"),
    (798, 440, 11, "Watchman's"),
    (810, 456, 11, "Room"),
    (861, 448, 11, "Fire Escape"),
    (641, 160, 13, "Boiler Room"),
    (468, 655, 16, "Entrance"),
]

PLAYER = (504.0, 579.5)

ONION_IMAGE = ("1 0.3500 1.0000 0.0000 0.0000 "
               "-476.0000 -367.5000 770.0000 563.5000 "
               "/home/dylan/Downloads/re2-map/2D-map/23112026/1f.png")
ONION_PIVOT = "147.000 98.000"

def world(x: float, y: float) -> tuple[float, float]:
    return (x - ORIGIN[0], y - ORIGIN[1])


def source(x: float, y: float) -> tuple[float, float]:
    wx, wy = world(x, y)
    return (wx + ORIGIN[0] - SOURCE_SHIFT[0],
            wy + ORIGIN[1] - SOURCE_SHIFT[1])


# Interior of each stair footprint: hatch lines and handrails there are drawn
# by the Stairs object, not stored as walls.
STAIR_CLEAR = [
    (349.5, 188.5, 379.5, 210.5), (787.5, 238.5, 819.5, 300.5),
    (837.5, 200.5, 882.5, 221.5), (865.5, 233.5, 901.5, 249.5),
    (434.5, 297.5, 465.5, 354.5), (547.5, 297.5, 577.5, 354.5),
    (873.5, 465.5, 912.5, 481.5), (895.5, 483.5, 912.5, 499.5),
]


def _inside_stair(segment):
    axis, a, b, c = segment
    for x0, y0, x1, y1 in STAIR_CLEAR:
        if axis == 'H':
            if x0 <= (a + b) * 0.5 <= x1 and y0 < c < y1:
                return True
        else:
            if y0 <= (a + b) * 0.5 <= y1 and x0 < c < x1:
                return True
    return False


def _segments():
    """All wall segments as (axis, a, b), merging duplicated collinear runs."""
    hs = {}
    for x0, x1, y in H:
        if x1 < x0:
            x0, x1 = x1, x0
        hs.setdefault(y, []).append((x0, x1))
    vs = {}
    for y0, y1, x in V:
        if y1 < y0:
            y0, y1 = y1, y0
        vs.setdefault(x, []).append((y0, y1))
    out = []
    for y, runs in hs.items():
        runs.sort()
        cur = list(runs[0])
        for a, b in runs[1:]:
            if a <= cur[1] + 2.5:
                cur[1] = max(cur[1], b)
            else:
                out.append(('H', cur[0], cur[1], y))
                cur = [a, b]
        out.append(('H', cur[0], cur[1], y))
    for x, runs in vs.items():
        runs.sort()
        cur = list(runs[0])
        for a, b in runs[1:]:
            if a <= cur[1] + 2.5:
                cur[1] = max(cur[1], b)
            else:
                out.append(('V', cur[0], cur[1], x))
                cur = [a, b]
        out.append(('V', cur[0], cur[1], x))
    out = [s for s in out if not _inside_stair(s)]
    return out


def _stitch(segments):
    """Close one- or two-pixel cracks where orthogonal walls nearly meet."""
    segs = [[axis, a, b, c] for axis, a, b, c in segments]
    for _ in range(3):
        changed = False
        for h in segs:
            if h[0] != 'H':
                continue
            for end in (1, 2):
                x = h[end]
                for v in segs:
                    if v[0] != 'V':
                        continue
                    if abs(x - v[3]) <= 2.0 and (
                            abs(h[3] - v[1]) <= 2.0 or
                            abs(h[3] - v[2]) <= 2.0):
                        h[end] = v[3]
                        if abs(h[3] - v[1]) <= 2.0:
                            v[1] = h[3]
                        if abs(h[3] - v[2]) <= 2.0:
                            v[2] = h[3]
                        changed = True
                    elif abs(x - v[3]) <= 2.0 and v[1] - 2.0 <= h[3] <= v[2] + 2.0:
                        h[end] = v[3]
                        changed = True
        for v in segs:
            if v[0] != 'V':
                continue
            for end in (1, 2):
                y = v[end]
                for h in segs:
                    if h[0] != 'H':
                        continue
                    if abs(y - h[3]) <= 2.0 and (
                            abs(v[3] - h[1]) <= 2.0 or
                            abs(v[3] - h[2]) <= 2.0):
                        v[end] = h[3]
                        if abs(v[3] - h[1]) <= 2.0:
                            h[1] = v[3]
                        if abs(v[3] - h[2]) <= 2.0:
                            h[2] = v[3]
                        changed = True
                    elif abs(y - h[3]) <= 2.0 and h[1] - 2.0 <= v[3] <= h[2] + 2.0:
                        v[end] = h[3]
                        changed = True
        if not changed:
            break
    out = []
    for axis, a, b, c in segs:
        if abs(b - a) > 0.25:
            out.append((axis, a, b, c))
    return out


def _components(segments):
    parent = list(range(len(segments)))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[rb] = ra

    def ends(s):
        if s[0] == 'H':
            return (s[1], s[3]), (s[2], s[3])
        return (s[3], s[1]), (s[3], s[2])

    for i, a in enumerate(segments):
        ae = ends(a)
        for j in range(i + 1, len(segments)):
            b = segments[j]
            be = ends(b)
            hit = any(abs(p[0] - q[0]) <= 1 and abs(p[1] - q[1]) <= 1
                      for p in ae for q in be)
            if not hit:
                for p in ae:
                    if b[0] == 'H' and abs(p[1] - b[3]) <= 1 and b[1] - 1 <= p[0] <= b[2] + 1:
                        hit = True
                    if b[0] == 'V' and abs(p[0] - b[3]) <= 1 and b[1] - 1 <= p[1] <= b[2] + 1:
                        hit = True
                for q in be:
                    if a[0] == 'H' and abs(q[1] - a[3]) <= 1 and a[1] - 1 <= q[0] <= a[2] + 1:
                        hit = True
                    if a[0] == 'V' and abs(q[0] - a[3]) <= 1 and a[1] - 1 <= q[1] <= a[2] + 1:
                        hit = True
            if hit:
                union(i, j)
    groups = defaultdict(list)
    for i, s in enumerate(segments):
        groups[find(i)].append(s)
    return list(groups.values())


def build(source_pixels: bool = False) -> list[str]:
    segments = _stitch(_segments())
    groups = _components(segments)
    lines = ["ink-level 3"]
    for sid, group in enumerate(groups, start=1):
        lines.append(f"shape {sid} 0 0")
    wid = 1
    wall_records = []
    for sid, group in enumerate(groups, start=1):
        for axis, a, b, c in group:
            if axis == 'H':
                p0 = (a, c); p1 = (b, c)
            else:
                p0 = (c, a); p1 = (c, b)
            if source_pixels:
                p0 = source(*p0); p1 = source(*p1)
            else:
                p0 = world(*p0); p1 = world(*p1)
            detail = (axis, a, b, c) in DETAIL_KEYS
            lines.append(
                f"wall {wid} {sid} {p0[0]:.3f} {p0[1]:.3f} "
                f"{p1[0]:.3f} {p1[1]:.3f}" + (" 1" if detail else "")
            )
            wall_records.append((wid, axis, a, b, c))
            wid += 1

    # Restore the wall span under every reference door bar and host a visible
    # door there.  Bars that already sit on a wall only add the door.
    bay_sid = len(groups) + 1
    bay_walls = []
    bay_doors = []
    seen = set()
    for bx, by, bw, bh in DOOR_BARS:
        horizontal = bw >= bh
        length = float(bw if horizontal else bh)
        cx = bx + bw / 2.0
        cy = by + bh / 2.0
        axis = 'H' if horizontal else 'V'
        if horizontal:
            near = [w for w in wall_records
                    if w[1] == 'H' and abs(w[4] - cy) <= 3]
            line = min((w[4] for w in near),
                       key=lambda c: abs(c - cy)) if near else cy
            same = [w for w in wall_records + bay_walls
                    if w[1] == 'H' and abs(w[4] - line) <= 0.51]
            host = next((w for w in same if w[2] <= cx <= w[3]), None)
            if host is None:
                left = [w[3] for w in same
                        if w[3] <= cx + 0.5 and cx - w[3] <= 45]
                right = [w[2] for w in same
                         if w[2] >= cx - 0.5 and w[2] - cx <= 45]
                a = max(left) if left else cx - length / 2.0 - 3.0
                b = min(right) if right else cx + length / 2.0 + 3.0
                if b - a > 90.0:
                    a = cx - length / 2.0 - 4.0
                    b = cx + length / 2.0 + 4.0
                wid += 1
                host = (wid, 'H', a, b, line)
                bay_walls.append(host)
            pos = cx
        else:
            near = [w for w in wall_records
                    if w[1] == 'V' and abs(w[4] - cx) <= 3]
            line = min((w[4] for w in near),
                       key=lambda c: abs(c - cx)) if near else cx
            same = [w for w in wall_records + bay_walls
                    if w[1] == 'V' and abs(w[4] - line) <= 0.51]
            host = next((w for w in same if w[2] <= cy <= w[3]), None)
            if host is None:
                low = [w[3] for w in same
                       if w[3] <= cy + 0.5 and cy - w[3] <= 45]
                high = [w[2] for w in same
                        if w[2] >= cy - 0.5 and w[2] - cy <= 45]
                a = max(low) if low else cy - length / 2.0 - 3.0
                b = min(high) if high else cy + length / 2.0 + 3.0
                if b - a > 90.0:
                    a = cy - length / 2.0 - 4.0
                    b = cy + length / 2.0 + 4.0
                wid += 1
                host = (wid, 'V', a, b, line)
                bay_walls.append(host)
            pos = cy
        host_id, _axis, a, b, _line = host
        key = (host_id, round(pos, 1))
        if key in seen:
            continue
        seen.add(key)
        t = (pos - a) / (b - a) if b != a else 0.5
        width = max(8.0, min(24.0, length))
        bay_doors.append((host_id, t, width))
    if bay_walls:
        lines.append(f"shape {bay_sid} 0 0")
        for wid_, axis, a, b, c in bay_walls:
            if axis == 'H':
                p0 = (a, c); p1 = (b, c)
            else:
                p0 = (c, a); p1 = (c, b)
            if source_pixels:
                p0 = source(*p0); p1 = source(*p1)
            else:
                p0 = world(*p0); p1 = world(*p1)
            lines.append(
                f"wall {wid_} {bay_sid} {p0[0]:.3f} {p0[1]:.3f} "
                f"{p1[0]:.3f} {p1[1]:.3f}"
            )
        for host_id, t, width in bay_doors:
            lines.append(f"door 0 {host_id} {t:.4f} {width:.3f} 0")
    for axis, coord, pos, width in NAV_DOORS:
        for host_id, wax, a, b, c in wall_records:
            if wax != axis or abs(c - coord) > 1e-6:
                continue
            if not (a <= pos <= b):
                continue
            t = (pos - a) / (b - a) if b != a else 0.5
            lines.append(f"door 0 {host_id} {t:.4f} {width:.3f} 1")
            break
    for x0, y0, x1, y1, direction, steps, flip in REGISTRATION_STAIRS:
        p0 = world(min(x0, x1), min(y0, y1))
        p1 = world(max(x0, x1), max(y0, y1))
        lines.append(
            f"stairs {p0[0]:.3f} {p0[1]:.3f} {direction} {steps} "
            f"{p1[0] - p0[0]:.3f} {p1[1] - p0[1]:.3f} {flip} 3 0.000 0.000"
        )
    for x0, y0, x1, y1, direction, steps, flip in STAIRS:
        p0 = world(min(x0, x1), min(y0, y1))
        p1 = world(max(x0, x1), max(y0, y1))
        lines.append(
            f"stairs {p0[0]:.3f} {p0[1]:.3f} {direction} {steps} "
            f"{p1[0] - p0[0]:.3f} {p1[1] - p0[1]:.3f} {flip} 3 0.000 0.680"
        )
    for x, y, size, text in LABELS:
        p = source(x, y) if source_pixels else world(x, y)
        lines.append(f"label {p[0]:.3f} {p[1]:.3f} {size:.3f} {text}")
    if source_pixels:
        p = source(*PLAYER)
        lines.append(f"player {p[0]:.3f} {p[1]:.3f} 0.0000")
    else:
        p = world(*PLAYER)
        lines.append(f"player {p[0]:.3f} {p[1]:.3f} 0.0000")
        lines.append(f"onion_image {ONION_IMAGE}")
        lines.append(f"onion_pivot {ONION_PIVOT}")
    return lines


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-dir", default="levels")
    ap.add_argument("--source", action="store_true")
    args = ap.parse_args()
    import os
    os.makedirs(args.out_dir, exist_ok=True)
    path = os.path.join(args.out_dir, "re2-library-1f.lvl")
    with open(path, "w") as handle:
        handle.write("\n".join(build(False)) + "\n")
    print(f"wrote {path}")
    if args.source:
        spath = os.path.join(args.out_dir, "re2-library-1f-source.lvl")
        with open(spath, "w") as handle:
            handle.write("\n".join(build(True)) + "\n")
        print(f"wrote {spath}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
