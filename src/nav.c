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

static bool point_clear_of_walls(const Level* level, Vec2 p, float radius) {
    float t0[LEVEL_MAX_DOORS];
    float t1[LEVEL_MAX_DOORS];
    for (int i = 0; i < level->wall_count; i++) {
        const Wall* w = &level->walls[i];
        int n = wall_solid_spans(level, w, radius, t0, t1, LEVEL_MAX_DOORS);
        for (int k = 0; k < n; k++) {
            Vec2 a = segment_point(w->a, w->b, t0[k]);
            Vec2 b = segment_point(w->a, w->b, t1[k]);
            if (point_segment_dist(p, a, b) < radius) return false;
        }
    }
    return true;
}

static bool edge_clear_of_walls(const Level* level, Vec2 a, Vec2 b,
                                float radius) {
    float t0[LEVEL_MAX_DOORS];
    float t1[LEVEL_MAX_DOORS];
    for (int i = 0; i < level->wall_count; i++) {
        const Wall* w = &level->walls[i];
        int n = wall_solid_spans(level, w, radius, t0, t1, LEVEL_MAX_DOORS);
        for (int k = 0; k < n; k++) {
            Vec2 c = segment_point(w->a, w->b, t0[k]);
            Vec2 d = segment_point(w->a, w->b, t1[k]);
            if (segment_crosses(a, b, c, d)) return false;
        }
    }
    return true;
}

bool nav_build(NavGrid* nav, const Level* level, float cell, float radius) {
    if (!nav || !level || cell < 1.0f) return false;
    memset(nav, 0, sizeof *nav);
    nav->cell = cell;
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

    Vec2 center[NAV_MAX_CELLS];
    for (int y = 0; y < nav->rows; y++) {
        for (int x = 0; x < nav->cols; x++) {
            int i = y * nav->cols + x;
            center[i] = v2(nav->origin.x + (x + 0.5f) * cell,
                           nav->origin.y + (y + 0.5f) * cell);
            nav->open[i] = level_shape_at(level, center[i]) >= 0 &&
                           point_clear_of_walls(level, center[i], radius);
        }
    }
    for (int y = 0; y < nav->rows; y++) {
        for (int x = 0; x < nav->cols; x++) {
            int i = y * nav->cols + x;
            if (!nav->open[i]) continue;
            unsigned char links = 0;
            if (x + 1 < nav->cols && nav->open[i + 1] &&
                edge_clear_of_walls(level, center[i], center[i + 1], radius)) {
                links |= 1u << 0;
            }
            if (x > 0 && nav->open[i - 1] &&
                edge_clear_of_walls(level, center[i], center[i - 1], radius)) {
                links |= 1u << 1;
            }
            if (y + 1 < nav->rows && nav->open[i + nav->cols] &&
                edge_clear_of_walls(level, center[i], center[i + nav->cols], radius)) {
                links |= 1u << 2;
            }
            if (y > 0 && nav->open[i - nav->cols] &&
                edge_clear_of_walls(level, center[i], center[i - nav->cols], radius)) {
                links |= 1u << 3;
            }
            bool east = (links & (1u << 0)) != 0;
            bool west = (links & (1u << 1)) != 0;
            bool south = (links & (1u << 2)) != 0;
            bool north = (links & (1u << 3)) != 0;
            /* 4=NE, 5=NW, 6=SE, 7=SW, matching the A* direction table. */
            if (x + 1 < nav->cols && y > 0 && east && north &&
                nav->open[i - nav->cols + 1] &&
                edge_clear_of_walls(level, center[i],
                                    center[i - nav->cols + 1], radius)) {
                links |= 1u << 4;
            }
            if (x > 0 && y > 0 && west && north &&
                nav->open[i - nav->cols - 1] &&
                edge_clear_of_walls(level, center[i],
                                    center[i - nav->cols - 1], radius)) {
                links |= 1u << 5;
            }
            if (x + 1 < nav->cols && y + 1 < nav->rows && east && south &&
                nav->open[i + nav->cols + 1] &&
                edge_clear_of_walls(level, center[i],
                                    center[i + nav->cols + 1], radius)) {
                links |= 1u << 6;
            }
            if (x > 0 && y + 1 < nav->rows && west && south &&
                nav->open[i + nav->cols - 1] &&
                edge_clear_of_walls(level, center[i],
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
            int nx = cx + ((dir == 0 || dir == 4 || dir == 6) ? 1 :
                           (dir == 1 || dir == 5 || dir == 7) ? -1 : 0);
            int ny = cy + ((dir == 2 || dir == 4 || dir == 5) ? 1 :
                           (dir == 3 || dir == 6 || dir == 7) ? -1 : 0);
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
