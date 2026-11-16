#include "geom.h"

#include <math.h>
#include <string.h>

#define GEOM_EPS 1e-5f

Vec2 v2(float x, float y) {
    return (Vec2){ x, y };
}

Vec2 v2_add(Vec2 a, Vec2 b) {
    return (Vec2){ a.x + b.x, a.y + b.y };
}

Vec2 v2_sub(Vec2 a, Vec2 b) {
    return (Vec2){ a.x - b.x, a.y - b.y };
}

Vec2 v2_scale(Vec2 a, float s) {
    return (Vec2){ a.x * s, a.y * s };
}

float v2_dot(Vec2 a, Vec2 b) {
    return a.x * b.x + a.y * b.y;
}

float v2_len(Vec2 a) {
    return sqrtf(v2_dot(a, a));
}

float v2_dist(Vec2 a, Vec2 b) {
    return v2_len(v2_sub(a, b));
}

Vec2 v2_lerp(Vec2 a, Vec2 b, float t) {
    return v2_add(a, v2_scale(v2_sub(b, a), t));
}

bool v2_equal(Vec2 a, Vec2 b, float eps) {
    return fabsf(a.x - b.x) <= eps && fabsf(a.y - b.y) <= eps;
}

float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

Vec2 snap_grid(Vec2 p, float grid) {
    if (!(grid > 0.0f)) return p;
    return (Vec2){ roundf(p.x / grid) * grid, roundf(p.y / grid) * grid };
}

Vec2 snap_ortho(Vec2 anchor, Vec2 cursor, float grid) {
    Vec2 snapped = snap_grid(cursor, grid);
    float dx = fabsf(cursor.x - anchor.x);
    float dy = fabsf(cursor.y - anchor.y);
    if (dx > dy) return (Vec2){ snapped.x, anchor.y };
    return (Vec2){ anchor.x, snapped.y };
}

bool rect_contains(Rect r, Vec2 p) {
    return p.x >= r.min.x && p.x <= r.max.x && p.y >= r.min.y && p.y <= r.max.y;
}

bool rect_overlaps(Rect a, Rect b) {
    return a.min.x <= b.max.x && b.min.x <= a.max.x &&
           a.min.y <= b.max.y && b.min.y <= a.max.y;
}

Rect rect_from_points(Vec2 a, Vec2 b) {
    return (Rect){
        { fminf(a.x, b.x), fminf(a.y, b.y) },
        { fmaxf(a.x, b.x), fmaxf(a.y, b.y) },
    };
}

bool segment_intersects_rect(Vec2 a, Vec2 b, Rect r) {
    if (rect_contains(r, a) || rect_contains(r, b)) return true;
    float t0 = 0.0f;
    float t1 = 1.0f;
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    const float p[4] = { -dx, dx, -dy, dy };
    const float q[4] = { a.x - r.min.x, r.max.x - a.x, a.y - r.min.y, r.max.y - a.y };
    for (int i = 0; i < 4; i++) {
        if (fabsf(p[i]) <= GEOM_EPS) {
            if (q[i] < 0.0f) return false;
            continue;
        }
        float t = q[i] / p[i];
        if (p[i] < 0.0f) {
            if (t > t1) return false;
            if (t > t0) t0 = t;
        } else {
            if (t < t0) return false;
            if (t < t1) t1 = t;
        }
    }
    return t0 <= t1;
}

Vec2 segment_point(Vec2 a, Vec2 b, float t) {
    return v2_lerp(a, b, t);
}

float point_segment_t(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = v2_sub(b, a);
    float len2 = v2_dot(ab, ab);
    if (len2 <= 0.0f) return 0.0f;
    return clampf(v2_dot(v2_sub(p, a), ab) / len2, 0.0f, 1.0f);
}

float point_segment_dist(Vec2 p, Vec2 a, Vec2 b) {
    return v2_dist(p, segment_point(a, b, point_segment_t(p, a, b)));
}

bool point_in_polygon(const Vec2* pts, int count, Vec2 p) {
    if (!pts || count < 3) return false;

    bool inside = false;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        if (point_segment_dist(p, pts[j], pts[i]) <= GEOM_EPS) return true;
        if ((pts[i].y > p.y) != (pts[j].y > p.y)) {
            float x = (pts[j].x - pts[i].x) * (p.y - pts[i].y) /
                          (pts[j].y - pts[i].y) +
                      pts[i].x;
            if (p.x < x) inside = !inside;
        }
    }
    return inside;
}

bool polygon_intersects_rect(const Vec2* pts, int count, Rect r) {
    if (!pts || count < 3) return false;
    if (!rect_overlaps(polygon_bounds(pts, count), r)) return false;
    for (int i = 0; i < count; i++) {
        if (rect_contains(r, pts[i])) return true;
    }
    for (int i = 0, j = count - 1; i < count; j = i++) {
        if (segment_intersects_rect(pts[j], pts[i], r)) return true;
    }
    const Vec2 corners[4] = {
        r.min,
        { r.min.x, r.max.y },
        r.max,
        { r.max.x, r.min.y },
    };
    for (int i = 0; i < 4; i++) {
        if (point_in_polygon(pts, count, corners[i])) return true;
    }
    return false;
}

float polygon_area(const Vec2* pts, int count) {
    if (!pts || count < 3) return 0.0f;
    float sum = 0.0f;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        sum += pts[j].x * pts[i].y - pts[i].x * pts[j].y;
    }
    return fabsf(sum) * 0.5f;
}

int polygon_simplify(Vec2* pts, int count) {
    if (!pts || count <= 0) return 0;

    int n = 0;
    for (int i = 0; i < count; i++) {
        if (n == 0 || !v2_equal(pts[n - 1], pts[i], GEOM_EPS)) {
            pts[n++] = pts[i];
        }
    }
    while (n > 1 && v2_equal(pts[0], pts[n - 1], GEOM_EPS)) n--;

    bool removed = true;
    while (removed && n >= 3) {
        removed = false;
        for (int i = 0; i < n; i++) {
            int prev = (i + n - 1) % n;
            int next = (i + 1) % n;
            Vec2 ab = v2_sub(pts[i], pts[prev]);
            Vec2 bc = v2_sub(pts[next], pts[i]);
            float cross = ab.x * bc.y - ab.y * bc.x;
            float scale = v2_len(ab) * v2_len(bc);
            if (fabsf(cross) <= GEOM_EPS * scale && v2_dot(ab, bc) >= 0.0f) {
                memmove(&pts[i], &pts[i + 1], (size_t)(n - i - 1) * sizeof(Vec2));
                n--;
                removed = true;
                break;
            }
        }
    }
    return n;
}

Rect polygon_bounds(const Vec2* pts, int count) {
    if (!pts || count <= 0) {
        return (Rect){ { 0.0f, 0.0f }, { 0.0f, 0.0f } };
    }
    Rect r = { pts[0], pts[0] };
    for (int i = 1; i < count; i++) {
        r.min.x = fminf(r.min.x, pts[i].x);
        r.min.y = fminf(r.min.y, pts[i].y);
        r.max.x = fmaxf(r.max.x, pts[i].x);
        r.max.y = fmaxf(r.max.y, pts[i].y);
    }
    return r;
}

static float triangle_area2(Vec2 a, Vec2 b, Vec2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static bool point_in_triangle(Vec2 p, Vec2 a, Vec2 b, Vec2 c) {
    float d1 = triangle_area2(a, b, p);
    float d2 = triangle_area2(b, c, p);
    float d3 = triangle_area2(c, a, p);
    bool negative = d1 < -GEOM_EPS || d2 < -GEOM_EPS || d3 < -GEOM_EPS;
    bool positive = d1 > GEOM_EPS || d2 > GEOM_EPS || d3 > GEOM_EPS;
    return !(negative && positive);
}

int polygon_triangulate(const Vec2* pts, int count, int* out, int max_triangles) {
    if (!pts || !out || count < 3) return -1;
    int triangle_count = count - 2;
    if (max_triangles < triangle_count) return -1;

    Rect bounds = polygon_bounds(pts, count);
    float extent = fmaxf(bounds.max.x - bounds.min.x,
                         bounds.max.y - bounds.min.y);
    if (!(extent > GEOM_EPS)) return -1;

    float area2 = 0.0f;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        area2 += pts[j].x * pts[i].y - pts[i].x * pts[j].y;
    }
    float epsilon = GEOM_EPS * extent * extent;
    if (fabsf(area2) <= epsilon) return -1;
    bool ccw = area2 > 0.0f;

    int active[count];
    for (int i = 0; i < count; i++) active[i] = i;

    int n = count;
    int written = 0;
    while (n > 3) {
        bool clipped = false;
        for (int i = 0; i < n; i++) {
            int prev = (i + n - 1) % n;
            int next = (i + 1) % n;
            int ia = active[prev];
            int ib = active[i];
            int ic = active[next];
            Vec2 a = pts[ia];
            Vec2 b = pts[ib];
            Vec2 c = pts[ic];

            float cross = triangle_area2(a, b, c);
            if (ccw ? cross <= epsilon : cross >= -epsilon) continue;

            bool ear = true;
            for (int j = 0; j < n; j++) {
                if (j == prev || j == i || j == next) continue;
                if (point_in_triangle(pts[active[j]], a, b, c)) {
                    ear = false;
                    break;
                }
            }
            if (!ear) continue;

            out[written * 3 + 0] = ia;
            out[written * 3 + 1] = ib;
            out[written * 3 + 2] = ic;
            written++;

            for (int j = i; j < n - 1; j++) active[j] = active[j + 1];
            n--;
            clipped = true;
            break;
        }
        if (!clipped) return -1;
    }

    float cross = triangle_area2(pts[active[0]], pts[active[1]], pts[active[2]]);
    if (fabsf(cross) <= epsilon) return -1;
    out[written * 3 + 0] = active[0];
    out[written * 3 + 1] = active[1];
    out[written * 3 + 2] = active[2];
    written++;
    return written == triangle_count ? written : -1;
}
