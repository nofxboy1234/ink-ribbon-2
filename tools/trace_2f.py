#!/usr/bin/env python3
"""Rebuild the 2F plan from the onion-skin trace in 2F reference pixels.

World = reference pixel - (533, 367.5), the registration that lands the
2F stairwells on the 3F plan. Wall vertices snap to the 8-unit nav grid.
Small fixtures snap to 4.
"""
from __future__ import annotations
import sys

OX, OY = 533.0, 367.5

# Room loops in world coordinates, mostly on the 8 grid.
ROOMS: dict[str, list[tuple[float, float]]] = {
    "topleft_room": [(-344,-184),(-280,-184),(-280,-128),(-344,-128)],
    "shower":       [(-280,-184),(-200,-184),(-200,-128),(-280,-128)],
    "stair_block":  [(-200,-184),(-72,-184),(-72,-128),(-200,-128)],
    "west_corridor":[(-344,-128),(-72,-128),(-72,-96),(-344,-96)],
    "armory":       [(-272,-104),(-216,-104),(-216,-32),(-272,-32)],
    "library":      [(-216,-96),(-72,-96),(-72,72),(-216,72)],
    "office":       [(-344,-104),(-272,-104),(-272,-8),(-216,-8),(-216,72),(-344,72)],
    "linen":        [(-344,72),(-216,72),(-216,160),(-344,160)],
    "lounge":       [(-160,72),(-72,72),(-72,160),(-160,160)],
    "lounge_corr":  [(-216,72),(-160,72),(-160,160),(-216,160)],
    "main_hall":    [(-72,-136),(128,-136),(128,184),(-72,184)],
    "chief":        [(152,-136),(272,-136),(272,-68),(152,-68)],
    "chief_corr":   [(128,-68),(272,-68),(272,-16),(128,-16)],
    "east_stair":   [(272,-136),(344,-136),(344,-48),(272,-48)],
    "roof":         [(344,-136),(448,-136),(448,-16),(344,-16)],
    "private":      [(128,-16),(288,-16),(288,72),(128,72)],
    "closet":       [(184,-16),(256,-16),(256,24),(184,24)],
    "waiting":      [(128,72),(216,72),(216,136),(128,136)],
    "art":          [(200,48),(352,48),(352,160),(200,160)],
    "art_inner":    [(200,48),(264,48),(264,80),(200,80)],
    "fire":         [(352,16),(448,16),(448,160),(352,160)],
    "east_mid":     [(288,-16),(352,-16),(352,48),(288,48)],
    "roof_low":     [(344,-16),(448,-16),(448,16),(344,16)],
    "roof_stair":   [(376,-176),(448,-176),(448,-136),(376,-136)],
    # Building perimeter, one closed loop so nav sees the whole interior.
    "outer":        [(-344,-184),(-72,-184),(-72,-136),(448,-136),(448,160),
                     (128,160),(128,184),(-72,184),(-72,160),(-344,160)],
}

# Detail fixtures in 2F reference pixels. They draw but do not collide.
DETAILS_PX: list[tuple[bool, list[tuple[float,float]]]] = [
    (True, [(330,300),(390,300),(390,330),(330,330)]),
    (True, [(410,320),(455,320),(455,345),(410,345)]),
    (True, [(330,390),(390,390),(390,420),(330,420)]),
    (True, [(415,400),(455,400),(455,425),(415,425)]),
    (True, [(275,295),(318,295),(318,330),(275,330)]),
    (True, [(250,235),(315,235),(315,260),(250,260)]),
    (True, [(200,460),(260,460),(260,500),(200,500)]),
    (True, [(680,350),(750,350),(750,382),(680,382)]),
    (True, [(370,351),(420,351),(420,370),(370,370)]),
    (True, [(382,370),(420,370),(420,384),(382,384)]),
    (True, [(475,205),(500,205),(500,230),(475,230)]),
    # Front-hall balcony curve, stepped with the Line tool.
    (False, [(522,292),(535,292),(535,302),(548,302),(548,312),(561,312),
             (561,320),(574,320),(574,312),(587,312),(587,302),(600,302),
             (600,292),(602,292),(602,360),(634,360),(640,360),(640,470),
             (634,470),(634,520),(640,520),(640,552),(489,552),(489,520),
             (495,520),(495,470),(489,470),(489,360),(522,360)]),
    # Curved hall-side recesses, proxied with steps.
    (False, [(462,300),(490,300),(490,335),(462,335)]),
    (False, [(462,405),(490,405),(490,440),(462,440)]),
    (False, [(630,300),(659,300),(659,335),(630,335)]),
    (False, [(630,405),(659,405),(659,440),(630,440)]),
    (False, [(630,500),(659,500),(659,530),(630,530)]),
    # Fire-escape flight is a diagonal run in the reference. Proxy it with a
    # stepped rectangle plus straight tread lines.
    (True, [(920,455),(980,455),(980,500),(920,500)]),
    (False, [(920,460),(980,460)]),
    (False, [(920,470),(980,470)]),
    (False, [(920,480),(980,480)]),
    (False, [(920,490),(980,490)]),
]


# Extra axis-aligned reference lines traced from the onion skin.
DETAIL_SEGMENTS = [
    ('H', 207, 403, 444),
    ('H', 220, 843, 961),
    ('H', 253, 199, 232),
    ('H', 254, 363, 384),
    ('H', 298, 548, 575),
    ('H', 300, 221, 259),
    ('H', 305, 258, 304),
    ('H', 308, 837, 878),
    ('H', 315, 670, 794),
    ('H', 334, 320, 342),
    ('H', 352, 320, 342),
    ('H', 352, 369, 421),
    ('H', 352, 439, 463),
    ('H', 361, 699, 730),
    ('H', 370, 320, 342),
    ('H', 370, 369, 421),
    ('H', 370, 441, 463),
    ('H', 376, 375, 401),
    ('H', 380, 293, 321),
    ('H', 395, 658, 694),
    ('H', 402, 700, 726),
    ('H', 415, 698, 720),
    ('H', 420, 695, 717),
    ('H', 448, 224, 258),
    ('H', 458, 258, 345),
    ('H', 459, 358, 463),
    ('H', 494, 767, 859),
    ('H', 497, 221, 278),
    ('H', 506, 658, 742),
    ('V', 220, 300, 498),
    ('V', 258, 408, 458),
    ('V', 341, 271, 335),
    ('V', 341, 439, 460),
    ('V', 408, 439, 490),
    ('V', 408, 496, 528),
    ('V', 490, 358, 426),
    ('V', 490, 448, 516),
    ('V', 495, 427, 447),
    ('V', 495, 517, 537),
    ('V', 565, 432, 452),
    ('V', 628, 517, 537),
    ('V', 629, 427, 447),
    ('V', 634, 358, 426),
    ('V', 634, 448, 516),
    ('V', 696, 336, 362),
    ('V', 742, 351, 393),
    ('V', 742, 404, 448),
    ('V', 766, 390, 495),
    ('V', 836, 309, 378),
    ('V', 836, 385, 491),
    ('V', 860, 416, 495),
    ('V', 907, 480, 520),
    ('V', 948, 351, 378),
    ('V', 960, 378, 416),
    ('V', 968, 480, 500),
]

# Visible doors: (x, y, horizontal, width) in world coordinates.
DOORS = [
    (-216, 16, False, 10),
    (-216, 112, False, 10),
    (-72, 16, False, 10),
    (-72, 112, False, 10),
    (128, -56, False, 10),
    (128, 24, False, 10),
    (152, -64, False, 10),
    (272, -64, False, 10),
    (288, 24, False, 10),
    (352, 56, False, 10),
    (352, 136, False, 10),
    (216, 112, False, 10),
    (-248, -32, True, 10),
    (-200, -128, True, 10),
    (160, -136, True, 10),
    (240, -68, True, 10),
    (400, -136, True, 10),
    (304, -16, True, 10),
]
# Stairs: (x, y, w, h, dir, steps, bottom)
STAIRS = [
    # Treads are 3 reference pixels apart in the onion skin. The fade end
    # follows the bright end of the reference hatching.
    (-152,-176, 24, 48, 1, 15, 1),
    ( -88,-176, 24, 48, 1, 15, 1),
    ( -40, -84, 32, 76, 1, 24, 0),
    (  72, -84, 32, 76, 1, 24, 0),
    (-152,  72, 56, 16, 0, 17, 1),
    ( 312,-128, 32, 40, 1, 13, 0),
    ( 376,-168, 56, 24, 0, 18, 0),
]

LABELS = [
    (226-533, 205-367.5, 13, "Shower Room"),
    (247-533, 274-367.5, 13, "Armory"),
    (192-533, 369-367.5, 11, "S.T.A.R.S. Office"),
    (341-533, 319-367.5, 13, "Library"),
    (190-533, 460-367.5, 12, "Linen Room"),
    (305-533, 479-367.5, 13, "Lounge"),
    (511-533, 395-367.5, 14, "Main Hall"),
    (678-533, 260-367.5, 12, "Chief's Office"),
    (885-533, 260-367.5, 13, "Roof"),
    (650-533, 377-367.5, 9, "Private Collection Room"),
    (744-533, 434-367.5, 12, "Art Room"),
    (660-533, 459-367.5, 11, "Waiting Room"),
    (875-533, 439-367.5, 12, "Fire Escape"),
]

def snap(v, step=8):
    return float(round(v / step) * step)

def wall_segments(loop):
    n = len(loop)
    for i in range(n):
        yield loop[i], loop[(i + 1) % n]

def build():
    lines = ["ink-level 3"]
    shapes = []
    walls = []  # (sid, a, b, detail)
    sid = 0
    for name, loop in ROOMS.items():
        sid += 1
        shapes.append((sid, True))
        for a, b in wall_segments(loop):
            walls.append((sid, a, b, False))
    for closed, loop_px in DETAILS_PX:
        sid += 1
        shapes.append((sid, closed))
        pts = [(snap(x - OX, 4), snap(y - OY, 4)) for x, y in loop_px]
        dedup = []
        for pt in pts:
            if not dedup or abs(pt[0]-dedup[-1][0]) > 1e-6 or abs(pt[1]-dedup[-1][1]) > 1e-6:
                dedup.append(pt)
        if closed and len(dedup) > 1 and dedup[0] == dedup[-1]:
            dedup.pop()
        pts = dedup
        count = len(pts) if closed else len(pts) - 1
        for i in range(count):
            a, b = pts[i], pts[(i + 1) % len(pts)]
            if abs(a[0]-b[0]) < 1e-6 and abs(a[1]-b[1]) < 1e-6:
                continue
            walls.append((sid, a, b, True))
    for o, c, a, b in DETAIL_SEGMENTS:
        sid += 1
        shapes.append((sid, False))
        if o == 'H':
            p0 = (snap(a - OX, 4), snap(c - OY, 4))
            p1 = (snap(b - OX, 4), snap(c - OY, 4))
        else:
            p0 = (snap(c - OX, 4), snap(a - OY, 4))
            p1 = (snap(c - OX, 4), snap(b - OY, 4))
        if p0 == p1:
            continue
        walls.append((sid, p0, p1, True))
    for i, (sid_, closed) in enumerate(shapes, start=1):
        lines.append(f"shape {sid_} {1 if closed else 0} 0")
    wid = 0
    for sid_, a, b, detail in walls:
        wid += 1
        lines.append(f"wall {wid} {sid_} {a[0]:.3f} {a[1]:.3f} {b[0]:.3f} {b[1]:.3f}"
                     + (" 1" if detail else ""))
    # host doors on walls
    def host(x, y, horizontal, tol=6.0):
        matches = []
        for wi, (sid_, a, b, detail) in enumerate(walls, start=1):
            if detail:
                continue
            if horizontal:
                if abs(a[1] - b[1]) > 1e-6: continue
                if abs(a[1] - y) > tol: continue
                lo, hi = sorted((a[0], b[0]))
                if not (lo - tol <= x <= hi + tol): continue
                t = (x - a[0]) / (b[0] - a[0])
            else:
                if abs(a[0] - b[0]) > 1e-6: continue
                if abs(a[0] - x) > tol: continue
                lo, hi = sorted((a[1], b[1]))
                if not (lo - tol <= y <= hi + tol): continue
                t = (y - a[1]) / (b[1] - a[1])
            if -0.08 <= t <= 1.08:
                best = (wi, min(max(t, 0.0), 1.0))
                matches.append(best)
        return matches
    used = []
    for x, y, horizontal, width in DOORS:
        hits = host(x, y, horizontal)
        if not hits:
            print(f"WARN no wall hosts door at {x},{y}", file=sys.stderr)
            continue
        for wi, t in hits:
            used.append((wi, t, float(width), 0))
    # hidden openings between overlapping collinear walls of different shapes
    groups = {}
    for wi, (sid_, a, b, detail) in enumerate(walls, start=1):
        if detail: continue
        if abs(a[0]-b[0]) < 1e-6:
            groups.setdefault(('V', round(a[0],3)), []).append((min(a[1],b[1]),max(a[1],b[1]),sid_,wi))
        elif abs(a[1]-b[1]) < 1e-6:
            groups.setdefault(('H', round(a[1],3)), []).append((min(a[0],b[0]),max(a[0],b[0]),sid_,wi))
    def covered(x, y, horizontal):
        for wi,t,w,h in used:
            sid_,a,b,d = walls[wi-1]
            qx=a[0]+(b[0]-a[0])*t; qy=a[1]+(b[1]-a[1])*t
            if horizontal:
                if abs(qy-y)<0.5 and abs(qx-x)<16: return True
            else:
                if abs(qx-x)<0.5 and abs(qy-y)<16: return True
        return False
    hidden = 0
    for (axis, coord), entries in groups.items():
        for i in range(len(entries)):
            for j in range(i+1, len(entries)):
                a0,a1,sa,wa = entries[i]; b0,b1,sb,wb = entries[j]
                if sa == sb: continue
                lo, hi = max(a0,b0), min(a1,b1)
                if hi-lo < 24: continue
                mid = (lo+hi)*0.5
                if covered(coord, mid, axis=='H'): continue
                wi = wa
                sid_,a,b,d = walls[wi-1]
                if axis=='H':
                    t=(mid-a[0])/(b[0]-a[0])
                else:
                    t=(mid-a[1])/(b[1]-a[1])
                used.append((wi,t,16.0,1)); hidden += 1
    for wi,t,width,h in used:
        lines.append(f"door 0 {wi} {t:.4f} {width:.3f} {h}")
    for x,y,w,h,dir_,steps,bottom in STAIRS:
        lines.append(f"stairs {x:.3f} {y:.3f} {dir_} {steps} {w:.3f} {h:.3f} {bottom} 3 0.000 0.680")
    for x,y,size,text in LABELS:
        lines.append(f"label {x:.3f} {y:.3f} {size:.3f} {text}")
    lines.append("onion_image 1 0.3500 1.0000 -3.4902 125.5656 -529.5098 -493.0656 764.4902 415.9344 /home/dylan/Downloads/re2-map/2D-map/23112026/2f.png")
    lines.append("onion_floor 2 0.3500")
    print(f"{len(ROOMS)} rooms, {len(DETAILS_PX)}+{len(DETAIL_SEGMENTS)} details, {len(used)} doors ({hidden} hidden), "
          f"{len(STAIRS)} stairs, {len(walls)} walls", file=sys.stderr)
    return "\n".join(lines) + "\n"

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "levels/re2-library-2f.lvl"
    text = build()
    with open(out, "w", encoding="utf-8") as handle:
        handle.write(text)
    print(f"wrote {out}: {len(ROOMS)} rooms, {len(STAIRS)} stairs, "
          f"{text.count(chr(10) + 'wall ')} walls", file=sys.stderr)
