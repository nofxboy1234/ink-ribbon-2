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
