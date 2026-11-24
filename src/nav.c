#include "nav.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NAV_SQRT2 1.41421356f

static bool segment_crosses(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    Vec2 r = v2_sub(b, a);
    Vec2 s = v2_sub(d, c);
    float denom = r.x * s.y - r.y * s.x;
    if (fabsf(denom) < 1e-6f) return false;
    Vec2 q = v2_sub(c, a);
    float t = (q.x * s.y - q.y * s.x) / denom;
    float u = (q.x * r.y - q.y * r.x) / denom;
    return t > 0.0001f && t < 0.9999f && u > 0.0001f && u < 0.9999f;
}

/* Exact minimum distance between two non-intersecting segments is the
   smallest endpoint-to-opposite-segment distance. */
static float segment_distance(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    float best = point_segment_dist(a, c, d);
    float v = point_segment_dist(b, c, d);
    if (v < best) best = v;
    v = point_segment_dist(c, a, b);
    if (v < best) best = v;
    v = point_segment_dist(d, a, b);
    if (v < best) best = v;
    return best;
}

static int wall_solid_spans(const Level* level, const Wall* wall, float radius,
                            float* t0, float* t1, int max) {
    float len = wall_length(wall);
    if (len <= 0.0001f) return 0;
    float pad = radius / len;
    float holes[LEVEL_MAX_DOORS * 2];
    int hole_count = 0;
    bool vertical = wall_is_vertical(wall);
    for (int i = 0; i < level->door_count; i++) {
        const Door* d = &level->doors[i];
        const Wall* host = level_wall_by_id_const(level, d->wall);
        if (!host || wall_is_vertical(host) != vertical) continue;
        /* The same line can be covered by more than one wall record (shared
           room edges). A door opens every collinear wall it sits on. */
        Vec2 dc = level_door_center(level, d);
        float t = point_segment_t(dc, wall->a, wall->b);
        Vec2 on = segment_point(wall->a, wall->b, t);
        if (v2_dist(dc, on) > 0.5f) continue;
        float half = d->width / (2.0f * len);
        float a = clampf(t - half - pad, 0.0f, 1.0f);
        float b = clampf(t + half + pad, 0.0f, 1.0f);
        if (b <= a) continue;
        holes[hole_count * 2] = a;
        holes[hole_count * 2 + 1] = b;
        hole_count++;
    }
    for (int i = 1; i < hole_count; i++) {
        float a = holes[i * 2];
        float b = holes[i * 2 + 1];
        int j = i - 1;
        while (j >= 0 && holes[j * 2] > a) {
            holes[(j + 1) * 2] = holes[j * 2];
            holes[(j + 1) * 2 + 1] = holes[j * 2 + 1];
            j--;
        }
        holes[(j + 1) * 2] = a;
        holes[(j + 1) * 2 + 1] = b;
    }
    int count = 0;
    float start = 0.0f;
    for (int i = 0; i < hole_count && count < max; i++) {
        float a = holes[i * 2];
        float b = holes[i * 2 + 1];
        if (a > start + 0.0001f) {
            t0[count] = start;
            t1[count] = a;
            count++;
        }
        if (b > start) start = b;
    }
    if (start < 1.0f - 0.0001f && count < max) {
        t0[count] = start;
        t1[count] = 1.0f;
        count++;
    }
    return count;
}

static int stair_rails(const Level* level, Vec2 seg[][2], int max) {
    int n = 0;
    for (int i = 0; i < level->stairs_count && n + 2 <= max; i++) {
        const Stairs* st = &level->stairs[i];
        Rect r = level_stairs_rect(st);
        if (st->dir == STAIRS_V) {
            seg[n][0] = v2(r.min.x, r.min.y);
            seg[n][1] = v2(r.min.x, r.max.y);
            n++;
            seg[n][0] = v2(r.max.x, r.min.y);
            seg[n][1] = v2(r.max.x, r.max.y);
            n++;
        } else {
            seg[n][0] = v2(r.min.x, r.min.y);
            seg[n][1] = v2(r.max.x, r.min.y);
            n++;
            seg[n][0] = v2(r.min.x, r.max.y);
            seg[n][1] = v2(r.max.x, r.max.y);
            n++;
        }
    }
    return n;
}

#define NAV_SPANS_PER_WALL 16

typedef struct {
    int count;
    float t0[NAV_SPANS_PER_WALL];
    float t1[NAV_SPANS_PER_WALL];
} NavWallSpans;

static bool edge_clear_of_walls(const Level* level, Vec2 a, Vec2 b,
                                float radius) {
    float t0[LEVEL_MAX_DOORS];
    float t1[LEVEL_MAX_DOORS];
    for (int i = 0; i < level->wall_count; i++) {
        const Wall* w = &level->walls[i];
        if (w->detail) continue;
        int n = wall_solid_spans(level, w, radius, t0, t1, LEVEL_MAX_DOORS);
        for (int k = 0; k < n; k++) {
            Vec2 c = segment_point(w->a, w->b, t0[k]);
            Vec2 d = segment_point(w->a, w->b, t1[k]);
            if (segment_crosses(a, b, c, d)) return false;
        }
    }
    Vec2 rails[LEVEL_MAX_STAIRS * 2][2];
    int rail_count = stair_rails(level, rails, LEVEL_MAX_STAIRS * 2);
    for (int i = 0; i < rail_count; i++) {
        if (segment_crosses(a, b, rails[i][0], rails[i][1])) return false;
    }
    return true;
}

/* A room counts when its wall chain forms one loop, even if the editor kept
   the shape's closed flag off because the Line tool never saw an explicit
   close. Rectangles and closed chains share the same path. */
static bool inside_any_shape_loop(const Level* level, Vec2 p) {
    Vec2 polygon[LEVEL_MAX_WALLS];
    for (int i = 0; i < level->shape_count; i++) {
        int count = level_shape_loop_polygon(level, level->shapes[i].id,
                                             polygon, LEVEL_MAX_WALLS);
        if (count >= 3 && point_in_polygon(polygon, count, p)) return true;
    }
    return false;
}

static bool point_clear_of_cached(const Level* level, const NavWallSpans* spans,
                                  Vec2 p, float radius) {
    for (int i = 0; i < level->wall_count; i++) {
        const Wall* w = &level->walls[i];
        for (int k = 0; k < spans[i].count; k++) {
            Vec2 a = segment_point(w->a, w->b, spans[i].t0[k]);
            Vec2 b = segment_point(w->a, w->b, spans[i].t1[k]);
            if (point_segment_dist(p, a, b) < radius) return false;
        }
    }
    Vec2 rails[LEVEL_MAX_STAIRS * 2][2];
    int rail_count = stair_rails(level, rails, LEVEL_MAX_STAIRS * 2);
    for (int i = 0; i < rail_count; i++) {
        if (point_segment_dist(p, rails[i][0], rails[i][1]) < radius) {
            return false;
        }
    }
    return true;
}

static bool edge_clear_of_cached(const Level* level,
                                 const NavWallSpans* spans,
                                 const NavWallSpans* solid_spans, Vec2 a,
                                 Vec2 b, float radius) {
    for (int i = 0; i < level->wall_count; i++) {
        const Wall* w = &level->walls[i];
        /* Crossing uses the real door opening: the raw link may not slip
           through the solid wall just above or below a doorway. Distance uses
           the radius-padded spans, so a doorway link is not rejected merely
           for being close to a jamb; the final smoothed route is centred. */
        for (int k = 0; k < solid_spans[i].count; k++) {
            Vec2 c = segment_point(w->a, w->b, solid_spans[i].t0[k]);
            Vec2 d = segment_point(w->a, w->b, solid_spans[i].t1[k]);
            if (segment_crosses(a, b, c, d)) return false;
        }
        for (int k = 0; k < spans[i].count; k++) {
            Vec2 c = segment_point(w->a, w->b, spans[i].t0[k]);
            Vec2 d = segment_point(w->a, w->b, spans[i].t1[k]);
            if (segment_distance(a, b, c, d) < radius - 0.001f) return false;
        }
    }
    Vec2 rails[LEVEL_MAX_STAIRS * 2][2];
    int rail_count = stair_rails(level, rails, LEVEL_MAX_STAIRS * 2);
    for (int i = 0; i < rail_count; i++) {
        if (segment_crosses(a, b, rails[i][0], rails[i][1])) return false;
        if (segment_distance(a, b, rails[i][0], rails[i][1]) <
            radius - 0.001f) {
            return false;
        }
    }
    return true;
}

bool nav_build(NavGrid* nav, const Level* level, float cell, float radius) {
    if (!nav || !level || cell < 1.0f) return false;
    memset(nav, 0, sizeof *nav);
    nav->cell = cell;
    nav->radius = radius;
    nav->level = level;
    if (level->wall_count == 0 && level->stairs_count == 0) return false;
    Rect bounds = level_bounds(level);
    float pad = cell * 2.0f;
    nav->origin = v2(floorf((bounds.min.x - pad) / cell) * cell,
                     floorf((bounds.min.y - pad) / cell) * cell);
    float maxx = ceilf((bounds.max.x + pad) / cell) * cell;
    float maxy = ceilf((bounds.max.y + pad) / cell) * cell;
    nav->cols = (int)((maxx - nav->origin.x) / cell);
    nav->rows = (int)((maxy - nav->origin.y) / cell);
    if (nav->cols <= 0 || nav->rows <= 0) return false;
    if (nav->cols > NAV_MAX_CELLS / nav->rows) return false;
    int total = nav->cols * nav->rows;
    if (total > NAV_MAX_CELLS) return false;

    for (int i = 0; i < level->door_count; i++) {
        const Door* door = &level->doors[i];
        const Wall* host = level_wall_by_id_const(level, door->wall);
        if (!host) continue;
        NavDoor nd;
        nd.center = level_door_center(level, door);
        nd.vertical = wall_is_vertical(host);
        nd.half = door->width * 0.5f;
        bool duplicate = false;
        for (int k = 0; k < nav->door_count; k++) {
            if (nav->doors[k].vertical == nd.vertical &&
                v2_dist(nav->doors[k].center, nd.center) < 0.05f) {
                if (nd.half > nav->doors[k].half) nav->doors[k].half = nd.half;
                duplicate = true;
                break;
            }
        }
        if (!duplicate && nav->door_count < LEVEL_MAX_DOORS) {
            nav->doors[nav->door_count++] = nd;
        }
    }

    /* Precompute each wall's solid spans once.  Recomputing them per cell
       made a dense multi-floor plan with dozens of doors take seconds. */
    static NavWallSpans spans[LEVEL_MAX_WALLS];
    static NavWallSpans solid_spans[LEVEL_MAX_WALLS];
    for (int i = 0; i < level->wall_count; i++) {
        if (level->walls[i].detail) {
            spans[i].count = 0;
            solid_spans[i].count = 0;
            continue;
        }
        spans[i].count = wall_solid_spans(level, &level->walls[i], radius,
                                          spans[i].t0, spans[i].t1,
                                          NAV_SPANS_PER_WALL);
        solid_spans[i].count = wall_solid_spans(
            level, &level->walls[i], 0.0f, solid_spans[i].t0,
            solid_spans[i].t1, NAV_SPANS_PER_WALL);
    }

    Vec2 center[NAV_MAX_CELLS];
    static bool clear_cell[NAV_MAX_CELLS];
    static bool outside[NAV_MAX_CELLS];
    static int queue[NAV_MAX_CELLS];
    for (int y = 0; y < nav->rows; y++) {
        for (int x = 0; x < nav->cols; x++) {
            int i = y * nav->cols + x;
            center[i] = v2(nav->origin.x + (x + 0.5f) * cell,
                           nav->origin.y + (y + 0.5f) * cell);
            clear_cell[i] =
                point_clear_of_cached(level, spans, center[i], radius);
            nav->open[i] = false;
        }
    }

    /* A placed player is the strongest statement about where walking is
       possible. Flood the clear cells connected to it, so a room assembled
       from several open line and rectangle shapes is navigable even though
       no single shape is a closed loop. Without a player, keep the closed or
       geometric-loop room test used when choosing a placement. */
    int player_cell = -1;
    if (level->player_present) {
        int px = (int)floorf((level->player_pos.x - nav->origin.x) / cell);
        int py = (int)floorf((level->player_pos.y - nav->origin.y) / cell);
        if (px >= 0 && py >= 0 && px < nav->cols && py < nav->rows) {
            player_cell = py * nav->cols + px;
        }
    }
    if (player_cell >= 0 && clear_cell[player_cell]) {
        int head = 0;
        int tail = 0;
        nav->open[player_cell] = true;
        queue[tail++] = player_cell;
        static const int dx4[4] = { 1, -1, 0, 0 };
        static const int dy4[4] = { 0, 0, 1, -1 };
        while (head < tail) {
            int i = queue[head++];
            int x = i % nav->cols;
            int y = i / nav->cols;
            for (int d = 0; d < 4; d++) {
                int nx = x + dx4[d];
                int ny = y + dy4[d];
                if (nx < 0 || ny < 0 || nx >= nav->cols ||
                    ny >= nav->rows) {
                    continue;
                }
                int j = ny * nav->cols + nx;
                if (nav->open[j] || !clear_cell[j]) continue;
                if (!edge_clear_of_cached(level, spans, solid_spans, center[i], center[j],
                                          radius)) {
                    continue;
                }
                nav->open[j] = true;
                queue[tail++] = j;
            }
        }
    } else {
        /* With no player to seed from, open the closed/geometric-loop rooms
           plus anything the exterior cannot reach. That second part lets a
           visually closed hybrid room built from several open line and
           rectangle shapes accept the first player placement. */
        for (int i = 0; i < total; i++) {
            outside[i] = false;
            nav->open[i] = clear_cell[i] &&
                           inside_any_shape_loop(level, center[i]);
        }
        int head = 0;
        int tail = 0;
        for (int y = 0; y < nav->rows; y++) {
            for (int x = 0; x < nav->cols; x++) {
                if (x != 0 && y != 0 && x != nav->cols - 1 &&
                    y != nav->rows - 1) {
                    continue;
                }
                int i = y * nav->cols + x;
                if (clear_cell[i] && !outside[i]) {
                    outside[i] = true;
                    queue[tail++] = i;
                }
            }
        }
        static const int dx4[4] = { 1, -1, 0, 0 };
        static const int dy4[4] = { 0, 0, 1, -1 };
        while (head < tail) {
            int i = queue[head++];
            int x = i % nav->cols;
            int y = i / nav->cols;
            for (int d = 0; d < 4; d++) {
                int nx = x + dx4[d];
                int ny = y + dy4[d];
                if (nx < 0 || ny < 0 || nx >= nav->cols ||
                    ny >= nav->rows) {
                    continue;
                }
                int j = ny * nav->cols + nx;
                if (outside[j] || !clear_cell[j]) continue;
                if (!edge_clear_of_cached(level, spans, solid_spans, center[i], center[j],
                                          radius)) {
                    continue;
                }
                outside[j] = true;
                queue[tail++] = j;
            }
        }
        for (int i = 0; i < total; i++) {
            if (clear_cell[i] && !outside[i]) nav->open[i] = true;
        }
    }
    for (int y = 0; y < nav->rows; y++) {
        for (int x = 0; x < nav->cols; x++) {
            int i = y * nav->cols + x;
            if (!nav->open[i]) continue;
            unsigned char links = 0;
            if (x + 1 < nav->cols && nav->open[i + 1] &&
                edge_clear_of_cached(level, spans, solid_spans, center[i], center[i + 1], radius)) {
                links |= 1u << 0;
            }
            if (x > 0 && nav->open[i - 1] &&
                edge_clear_of_cached(level, spans, solid_spans, center[i], center[i - 1], radius)) {
                links |= 1u << 1;
            }
            if (y + 1 < nav->rows && nav->open[i + nav->cols] &&
                edge_clear_of_cached(level, spans, solid_spans, center[i], center[i + nav->cols], radius)) {
                links |= 1u << 2;
            }
            if (y > 0 && nav->open[i - nav->cols] &&
                edge_clear_of_cached(level, spans, solid_spans, center[i], center[i - nav->cols], radius)) {
                links |= 1u << 3;
            }
            bool east = (links & (1u << 0)) != 0;
            bool west = (links & (1u << 1)) != 0;
            bool south = (links & (1u << 2)) != 0;
            bool north = (links & (1u << 3)) != 0;
            /* 4=NE, 5=NW, 6=SE, 7=SW, matching the A* direction table. */
            if (x + 1 < nav->cols && y > 0 && east && north &&
                nav->open[i - nav->cols + 1] &&
                edge_clear_of_cached(level, spans, solid_spans, center[i],
                                    center[i - nav->cols + 1], radius)) {
                links |= 1u << 4;
            }
            if (x > 0 && y > 0 && west && north &&
                nav->open[i - nav->cols - 1] &&
                edge_clear_of_cached(level, spans, solid_spans, center[i],
                                    center[i - nav->cols - 1], radius)) {
                links |= 1u << 5;
            }
            if (x + 1 < nav->cols && y + 1 < nav->rows && east && south &&
                nav->open[i + nav->cols + 1] &&
                edge_clear_of_cached(level, spans, solid_spans, center[i],
                                    center[i + nav->cols + 1], radius)) {
                links |= 1u << 6;
            }
            if (x > 0 && y + 1 < nav->rows && west && south &&
                nav->open[i + nav->cols - 1] &&
                edge_clear_of_cached(level, spans, solid_spans, center[i],
                                    center[i + nav->cols - 1], radius)) {
                links |= 1u << 7;
            }
            nav->links[i] = links;
        }
    }
    nav->built = true;
    return true;
}

int nav_cell_at(const NavGrid* nav, Vec2 p) {
    if (!nav || !nav->built) return -1;
    int x = (int)floorf((p.x - nav->origin.x) / nav->cell);
    int y = (int)floorf((p.y - nav->origin.y) / nav->cell);
    if (x < 0 || y < 0 || x >= nav->cols || y >= nav->rows) return -1;
    return y * nav->cols + x;
}

int nav_cell_at_world(const NavGrid* nav, Vec2 p) { return nav_cell_at(nav, p); }

Vec2 nav_cell_center(const NavGrid* nav, int index) {
    if (!nav || index < 0 || index >= nav->cols * nav->rows) {
        return v2(0.0f, 0.0f);
    }
    int x = index % nav->cols;
    int y = index / nav->cols;
    return v2(nav->origin.x + (x + 0.5f) * nav->cell,
              nav->origin.y + (y + 0.5f) * nav->cell);
}

bool nav_cell_valid(const NavGrid* nav, int index) {
    return nav && nav->built && index >= 0 && index < nav->cols * nav->rows;
}

bool nav_cell_is_open(const NavGrid* nav, int index) {
    return nav_cell_valid(nav, index) && nav->open[index];
}

static float heuristic(const NavGrid* nav, int a, int b) {
    int ax = a % nav->cols, ay = a / nav->cols;
    int bx = b % nav->cols, by = b / nav->cols;
    int dx = abs(ax - bx);
    int dy = abs(ay - by);
    return NAV_SQRT2 * (float)(dx < dy ? dx : dy) +
           (float)(dx > dy ? dx - (dx < dy ? dx : dy)
                           : dy - (dx < dy ? dx : dy));
}

static int push_heap(int* heap, float* cost, int count, int cell, float f) {
    int i = count++;
    heap[i] = cell;
    cost[i] = f;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (cost[p] <= cost[i]) break;
        int hi = heap[p];
        float hc = cost[p];
        heap[p] = heap[i];
        cost[p] = cost[i];
        heap[i] = hi;
        cost[i] = hc;
        i = p;
    }
    return count;
}

static int pop_heap(int* heap, float* cost, int* count) {
    int top = heap[0];
    (*count)--;
    if (*count > 0) {
        heap[0] = heap[*count];
        cost[0] = cost[*count];
        int i = 0;
        for (;;) {
            int l = i * 2 + 1;
            int r = l + 1;
            int m = i;
            if (l < *count && cost[l] < cost[m]) m = l;
            if (r < *count && cost[r] < cost[m]) m = r;
            if (m == i) break;
            int hi = heap[m];
            float hc = cost[m];
            heap[m] = heap[i];
            cost[m] = cost[i];
            heap[i] = hi;
            cost[i] = hc;
            i = m;
        }
    }
    return top;
}

/* Keep a path segment the full player radius away from every solid wall and
   stair rail. Door openings use their real width (no radius padding), so a
   path can still pass through a doorway, but it may not graze a jamb or a
   wall endpoint. */
static bool line_clear_of_walls(const Level* level, Vec2 a, Vec2 b,
                                float radius) {
    float minx = fminf(a.x, b.x) - radius;
    float maxx = fmaxf(a.x, b.x) + radius;
    float miny = fminf(a.y, b.y) - radius;
    float maxy = fmaxf(a.y, b.y) + radius;
    float t0[LEVEL_MAX_DOORS];
    float t1[LEVEL_MAX_DOORS];
    for (int i = 0; i < level->wall_count; i++) {
        const Wall* w = &level->walls[i];
        if (w->detail) continue;
        float wx0 = fminf(w->a.x, w->b.x);
        float wx1 = fmaxf(w->a.x, w->b.x);
        float wy0 = fminf(w->a.y, w->b.y);
        float wy1 = fmaxf(w->a.y, w->b.y);
        if (wx1 < minx || wx0 > maxx || wy1 < miny || wy0 > maxy) continue;
        int n = wall_solid_spans(level, w, 0.0f, t0, t1, LEVEL_MAX_DOORS);
        for (int k = 0; k < n; k++) {
            Vec2 c = segment_point(w->a, w->b, t0[k]);
            Vec2 d = segment_point(w->a, w->b, t1[k]);
            if (segment_crosses(a, b, c, d)) return false;
            if (segment_distance(a, b, c, d) < radius - 0.001f) return false;
        }
    }
    Vec2 rails[LEVEL_MAX_STAIRS * 2][2];
    int rail_count = stair_rails(level, rails, LEVEL_MAX_STAIRS * 2);
    for (int i = 0; i < rail_count; i++) {
        if (segment_crosses(a, b, rails[i][0], rails[i][1])) return false;
        if (segment_distance(a, b, rails[i][0], rails[i][1]) <
            radius - 0.001f) {
            return false;
        }
    }
    return true;
}

bool nav_line_clear(const NavGrid* nav, Vec2 a, Vec2 b) {
    if (!nav || !nav->level) return false;
    if (!line_clear_of_walls(nav->level, a, b, nav->radius)) return false;
    float len = v2_dist(a, b);
    int steps = (int)ceilf(len / 0.5f);
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++) {
        Vec2 p = v2_lerp(a, b, (float)i / (float)steps);
        if (!nav_cell_is_open(nav, nav_cell_at(nav, p))) return false;
    }
    return true;
}

bool nav_segment_clear_soft(const NavGrid* nav, Vec2 a, Vec2 b) {
    if (!nav || !nav->level) return false;
    return edge_clear_of_walls(nav->level, a, b, 0.75f);
}

int nav_smooth_path(const NavGrid* nav, const int* cells, int count, Vec2* out,
                    int max) {
    if (!nav || !cells || !out || count <= 0 || max <= 0) return 0;
    Vec2 pts[NAV_MAX_PATH];
    int n = 0;
    for (int i = 0; i < count && n < NAV_MAX_PATH; i++) {
        Vec2 p = nav_cell_center(nav, cells[i]);
        if (n == 0 || !v2_equal(p, pts[n - 1], 0.01f)) pts[n++] = p;
    }
    if (n == 0) return 0;
    if (n == 1) {
        out[0] = pts[0];
        return 1;
    }

    Vec2 filtered[NAV_MAX_PATH];
    int fn = 0;
    filtered[fn++] = pts[0];
    int last = 0;
    while (last < n - 1 && fn < NAV_MAX_PATH) {
        int next = last + 1;
        for (int j = n - 1; j > last + 1; j--) {
            if (nav_line_clear(nav, pts[last], pts[j])) {
                next = j;
                break;
            }
        }
        filtered[fn++] = pts[next];
        last = next;
    }
    if (fn < 2) {
        out[0] = filtered[0];
        return 1;
    }
    /* A door centre is usually a few grid units off the natural crossing.
       Shifting only the crossing point makes a small S-bend. Warp a window of
       filtered points toward the door axis first, so the lane change is spread
       over many segments and the crossing itself stays centred. */
    for (int d = 0; d < nav->door_count; d++) {
        const NavDoor* nd = &nav->doors[d];
        for (int i = 0; i < fn - 1; i++) {
            Vec2 a = filtered[i];
            Vec2 b = filtered[i + 1];
            float t;
            float across;
            if (nd->vertical) {
                if (fabsf(b.x - a.x) < 0.0001f) continue;
                t = (nd->center.x - a.x) / (b.x - a.x);
                across = a.y + (b.y - a.y) * t;
            } else {
                if (fabsf(b.y - a.y) < 0.0001f) continue;
                t = (nd->center.y - a.y) / (b.y - a.y);
                across = a.x + (b.x - a.x) * t;
            }
            if (t <= 0.001f || t >= 0.999f) continue;
            if (fabsf(across - (nd->vertical ? nd->center.y : nd->center.x)) >
                nd->half) {
                continue;
            }
            float delta = (nd->vertical ? nd->center.y : nd->center.x) -
                          across;
            if (fabsf(delta) < 0.01f) break;
            const float radius = 40.0f;
            for (int j = 0; j < fn; j++) {
                if (j == 0 || j == fn - 1) continue;
                float dist = v2_dist(filtered[j], nd->center);
                if (dist >= radius) continue;
                float w = 1.0f - dist / radius;
                w = w * w * (3.0f - 2.0f * w);
                if (nd->vertical) filtered[j].y += delta * w;
                else filtered[j].x += delta * w;
            }
            break;
        }
    }

    /* Route through the centre of each doorway the filtered polyline crosses.
       Add a short approach and exit point on the door normal so the path
       crosses the opening straight and the curve can stay smooth there. */
    Vec2 routed[NAV_MAX_PATH];
    bool routed_door[NAV_MAX_PATH] = { false };
    bool door_done[LEVEL_MAX_DOORS] = { false };
    int rn = 0;
    routed[rn++] = filtered[0];
    for (int i = 0; i < fn - 1; i++) {
        Vec2 a = filtered[i];
        Vec2 b = filtered[i + 1];
        float hits[LEVEL_MAX_DOORS];
        int hit_doors[LEVEL_MAX_DOORS];
        int hit_count = 0;
        for (int d = 0; d < nav->door_count; d++) {
            const NavDoor* nd = &nav->doors[d];
            float t;
            float across;
            if (nd->vertical) {
                if (fabsf(b.x - a.x) < 0.0001f) continue;
                t = (nd->center.x - a.x) / (b.x - a.x);
                across = a.y + (b.y - a.y) * t;
            } else {
                if (fabsf(b.y - a.y) < 0.0001f) continue;
                t = (nd->center.y - a.y) / (b.y - a.y);
                across = a.x + (b.x - a.x) * t;
            }
            if (t <= 0.001f || t >= 0.999f) continue;
            if (fabsf(across - (nd->vertical ? nd->center.y : nd->center.x)) >
                nd->half + 4.0f) {
                continue;
            }
            int at = hit_count;
            while (at > 0 && hits[at - 1] > t) {
                hits[at] = hits[at - 1];
                hit_doors[at] = hit_doors[at - 1];
                at--;
            }
            hits[at] = t;
            hit_doors[at] = d;
            hit_count++;
        }
        for (int h = 0; h < hit_count && rn + 3 < NAV_MAX_PATH; h++) {
            int door_id = hit_doors[h];
            if (door_done[door_id]) continue;
            door_done[door_id] = true;
            const NavDoor* nd = &nav->doors[door_id];
            Vec2 normal = nd->vertical ? v2(1.0f, 0.0f) : v2(0.0f, 1.0f);
            float t_hit = hits[h];
            float across = nd->vertical
                               ? a.y + (b.y - a.y) * t_hit
                               : a.x + (b.x - a.x) * t_hit;
            float residual = fabsf(across - (nd->vertical ? nd->center.y
                                                          : nd->center.x));
            /* The warp already brought the natural crossing close to the
               centre. Insert only the centre point there; adding the full
               approach/exit plateau would create the little S-bend. */
            if (residual < 1.0f) {
                if (v2_dist(nd->center, routed[rn - 1]) > 0.02f) {
                    routed_door[rn] = true;
                    routed[rn++] = nd->center;
                }
                continue;
            }
            Vec2 p0 = a;
            Vec2 p3 = b;
            float span = v2_dist(p0, p3);
            float tangent = fmaxf(2.0f, span * 0.5f);
            Vec2 sum = v2_scale(
                v2_sub(v2_scale(nd->center, 8.0f), v2_add(p0, p3)),
                1.0f / 3.0f);
            Vec2 diff = v2_add(
                v2_scale(normal, (4.0f / 3.0f) * tangent),
                v2_sub(p0, p3));
            Vec2 p1 = v2_scale(v2_sub(sum, diff), 0.5f);
            Vec2 p2 = v2_scale(v2_add(sum, diff), 0.5f);
            int steps = (int)ceilf(span / 0.5f);
            if (steps < 8) steps = 8;
            if (steps > 64) steps = 64;
            if (steps & 1) steps++;

            /* The cubic passes through the door centre with a tangent along
               the door normal. Use it when every piece clears the walls; that
               removes the S-bend a straight centre insertion can leave. */
            Vec2 curve[96];
            int cn = 0;
            curve[cn++] = p0;
            for (int k = 1; k <= steps; k++) {
                float t = (float)k / (float)steps;
                float u = 1.0f - t;
                Vec2 q = v2_add(
                    v2_add(v2_scale(p0, u * u * u),
                           v2_scale(p1, 3.0f * u * u * t)),
                    v2_add(v2_scale(p2, 3.0f * u * t * t),
                           v2_scale(p3, t * t * t)));
                if (v2_dist(q, curve[cn - 1]) > 0.02f) curve[cn++] = q;
            }
            bool curve_clear = cn >= 2;
            for (int k = 1; curve_clear && k < cn; k++) {
                if (!line_clear_of_walls(nav->level, curve[k - 1], curve[k],
                                         nav->radius)) {
                    curve_clear = false;
                }
            }
            if (curve_clear) {
                for (int k = 1; k < cn && rn < NAV_MAX_PATH; k++) {
                    routed_door[rn] = k == steps / 2;
                    routed[rn++] = curve[k];
                }
            } else {
                float approach = fmaxf(10.0f, nav->radius + 4.0f);
                float d_before = fminf(
                    approach, fmaxf(1.0f, span * t_hit * 0.45f));
                float d_after = fminf(
                    approach, fmaxf(1.0f, span * (1.0f - t_hit) * 0.45f));
                Vec2 before = v2_sub(nd->center,
                                     v2_scale(normal, d_before));
                Vec2 after = v2_add(nd->center,
                                    v2_scale(normal, d_after));
                if (v2_dot(v2_sub(b, a), normal) < 0.0f) {
                    Vec2 swap = before;
                    before = after;
                    after = swap;
                }
                Vec2 insert[3] = { before, nd->center, after };
                for (int k = 0; k < 3; k++) {
                    if (v2_dist(insert[k], routed[rn - 1]) > 0.02f) {
                        routed_door[rn] = k == 1;
                        routed[rn++] = insert[k];
                    }
                }
            }
        }
        if (v2_dist(b, routed[rn - 1]) > 0.02f) routed[rn++] = b;
    }
    bool filtered_door[NAV_MAX_PATH] = { false };
    if (rn >= 2) {
        fn = rn;
        for (int i = 0; i < rn; i++) {
            filtered[i] = routed[i];
            filtered_door[i] = routed_door[i];
        }
    }

    /* Round each corner independently. A quadratic arc never leaves the
       corner's own triangle, so it cannot loop or overshoot like a spline.
       Start with the widest pleasant radius and shrink just that corner until
       its arc stays clear of walls and open cells. */
    Vec2 smooth[NAV_MAX_PATH];
    int sn = 0;
    smooth[sn++] = filtered[0];
    for (int i = 1; i < fn - 1 && sn < NAV_MAX_PATH; i++) {
        Vec2 prev = filtered[i - 1];
        Vec2 cur = filtered[i];
        Vec2 next = filtered[i + 1];
        float l0 = v2_dist(prev, cur);
        float l1 = v2_dist(cur, next);
        if (l0 < 0.001f || l1 < 0.001f) continue;
        Vec2 d0 = v2_scale(v2_sub(cur, prev), 1.0f / l0);
        Vec2 d1 = v2_scale(v2_sub(next, cur), 1.0f / l1);
        if (filtered_door[i]) {
            if (v2_dist(cur, smooth[sn - 1]) > 0.02f) smooth[sn++] = cur;
            continue;
        }
        float dot = v2_dot(d0, d1);
        if (dot < -1.0f) dot = -1.0f;
        if (dot > 1.0f) dot = 1.0f;
        float angle = acosf(dot);
        float wanted = 10.0f * (0.25f + 0.75f *
                                (1.0f - angle / 3.14159265f));
        float limit = fminf(l0, l1) * 0.45f;
        if (wanted > limit) wanted = limit;
        if (dot > 0.999f || wanted < 0.2f) {
            if (v2_dist(cur, smooth[sn - 1]) > 0.02f) smooth[sn++] = cur;
            continue;
        }
        const float scale_down[5] = { 1.0f, 0.6f, 0.35f, 0.2f, 0.08f };
        bool placed = false;
        for (int si = 0; si < 5 && !placed; si++) {
            float radius = wanted * scale_down[si];
            if (radius < 0.2f) continue;
            Vec2 a = v2_sub(cur, v2_scale(d0, radius));
            Vec2 b = v2_add(cur, v2_scale(d1, radius));
            Vec2 arc[40];
            int an = 0;
            arc[an++] = a;
            int steps = (int)ceilf(radius / 0.5f);
            if (steps < 8) steps = 8;
            if (steps > 48) steps = 48;
            for (int k = 1; k <= steps; k++) {
                float t = (float)k / (float)steps;
                float u = 1.0f - t;
                Vec2 q = v2_add(
                    v2_add(v2_scale(a, u * u), v2_scale(cur, 2.0f * u * t)),
                    v2_scale(b, t * t));
                if (v2_dist(q, arc[an - 1]) > 0.02f) arc[an++] = q;
            }
            bool clear = an >= 2;
            for (int k = 1; clear && k < an; k++) {
                if (!nav_line_clear(nav, arc[k - 1], arc[k])) clear = false;
            }
            if (!clear) continue;
            for (int k = 0; k < an; k++) {
                if (v2_dist(arc[k], smooth[sn - 1]) > 0.02f) {
                    smooth[sn++] = arc[k];
                    if (sn >= NAV_MAX_PATH) break;
                }
            }
            placed = true;
        }
        if (!placed && v2_dist(cur, smooth[sn - 1]) > 0.02f) {
            smooth[sn++] = cur;
        }
    }
    if (sn < NAV_MAX_PATH &&
        v2_dist(filtered[fn - 1], smooth[sn - 1]) > 0.02f) {
        smooth[sn++] = filtered[fn - 1];
    }
    if (sn < 2) {
        int take = fn < max ? fn : max;
        for (int i = 0; i < take; i++) out[i] = filtered[i];
        return take;
    }
    int take = sn < max ? sn : max;
    for (int i = 0; i < take; i++) out[i] = smooth[i];
    return take;
}

int nav_find_path(const NavGrid* nav, int start, int goal, int* out, int max) {
    if (!nav_cell_is_open(nav, start) || !nav_cell_is_open(nav, goal)) return 0;
    int total = nav->cols * nav->rows;
    static int came_from[NAV_MAX_CELLS];
    static float g_score[NAV_MAX_CELLS];
    static bool closed[NAV_MAX_CELLS];
    static int heap[NAV_MAX_CELLS];
    static float heap_cost[NAV_MAX_CELLS];
    for (int i = 0; i < total; i++) {
        came_from[i] = -1;
        g_score[i] = INFINITY;
        closed[i] = false;
    }
    g_score[start] = 0.0f;
    int heap_count = 0;
    heap_count = push_heap(heap, heap_cost, heap_count, start,
                           heuristic(nav, start, goal));
    bool found = false;
    while (heap_count > 0) {
        int current = pop_heap(heap, heap_cost, &heap_count);
        if (closed[current]) continue;
        if (current == goal) {
            found = true;
            break;
        }
        closed[current] = true;
        int cx = current % nav->cols;
        int cy = current / nav->cols;
        for (int dir = 0; dir < 8; dir++) {
            /* 0=E, 1=W, 2=S, 3=N, 4=NE, 5=NW, 6=SE, 7=SW. This must match
               the bit assignment used by the link builder exactly. */
            int nx = cx + ((dir == 0 || dir == 4 || dir == 6) ? 1 :
                           (dir == 1 || dir == 5 || dir == 7) ? -1 : 0);
            int ny = cy + ((dir == 2 || dir == 6 || dir == 7) ? 1 :
                           (dir == 3 || dir == 4 || dir == 5) ? -1 : 0);
            if (nx < 0 || ny < 0 || nx >= nav->cols || ny >= nav->rows) continue;
            int next = ny * nav->cols + nx;
            if (!(nav->links[current] & (1u << dir))) continue;
            if (closed[next]) continue;
            float step = dir < 4 ? 1.0f : NAV_SQRT2;
            float tentative = g_score[current] + step;
            if (tentative >= g_score[next]) continue;
            g_score[next] = tentative;
            came_from[next] = current;
            heap_count = push_heap(heap, heap_cost, heap_count, next,
                                   tentative + heuristic(nav, next, goal));
        }
    }
    if (!found) return 0;
    int reversed[NAV_MAX_PATH];
    int count = 0;
    int cell = goal;
    while (cell >= 0 && count < NAV_MAX_PATH) {
        reversed[count++] = cell;
        if (cell == start) break;
        cell = came_from[cell];
    }
    if (count == 0 || reversed[count - 1] != start) return 0;
    int n = count < max ? count : max;
    for (int i = 0; i < n; i++) out[i] = reversed[count - 1 - i];
    return n;
}
