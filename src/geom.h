#ifndef GEOM_H
#define GEOM_H

#include <stdbool.h>

typedef struct {
    float x, y;
} Vec2;

typedef struct {
    Vec2 min, max;
} Rect;

Vec2 v2(float x, float y);
Vec2 v2_add(Vec2 a, Vec2 b);
Vec2 v2_sub(Vec2 a, Vec2 b);
Vec2 v2_scale(Vec2 a, float s);
float v2_dot(Vec2 a, Vec2 b);
float v2_len(Vec2 a);
float v2_dist(Vec2 a, Vec2 b);
Vec2 v2_lerp(Vec2 a, Vec2 b, float t);
bool v2_equal(Vec2 a, Vec2 b, float eps);

float clampf(float v, float lo, float hi);
float lerpf(float a, float b, float t);

/* Orthogonal cursor constraint. Chooses horizontal when |dx| > |dy|,
   otherwise vertical, then snaps the free coordinate to grid. */
Vec2 snap_ortho(Vec2 anchor, Vec2 cursor, float grid);
Vec2 snap_grid(Vec2 p, float grid);

bool rect_contains(Rect r, Vec2 p);
bool rect_overlaps(Rect a, Rect b);
Rect rect_from_points(Vec2 a, Vec2 b);

float point_segment_dist(Vec2 p, Vec2 a, Vec2 b);
float point_segment_t(Vec2 p, Vec2 a, Vec2 b);
Vec2 segment_point(Vec2 a, Vec2 b, float t);

bool point_in_polygon(const Vec2* pts, int count, Vec2 p);
float polygon_area(const Vec2* pts, int count);
/* Removes duplicate and collinear vertices in place, returns new count. */
int polygon_simplify(Vec2* pts, int count);
Rect polygon_bounds(const Vec2* pts, int count);

#endif
