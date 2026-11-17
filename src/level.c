#include "level.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ORTHO_EPS 0.001f
#define LOAD_LINE_MAX 16384

static void set_error(LevelError* err, const char* fmt, ...) {
    if (!err) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->message, sizeof err->message, fmt, ap);
    va_end(ap);
}

static bool fail_line(LevelError* err, int line, const char* fmt, ...) {
    if (err) {
        char message[200];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(message, sizeof message, fmt, ap);
        va_end(ap);
        snprintf(err->message, sizeof err->message, "line %d: %s", line, message);
    }
    return false;
}

static bool wall_ok(Vec2 a, Vec2 b) {
    float dx = fabsf(b.x - a.x);
    float dy = fabsf(b.y - a.y);
    if (dx <= ORTHO_EPS && dy <= ORTHO_EPS) return false;
    return dx <= ORTHO_EPS || dy <= ORTHO_EPS;
}

/* 0 slides a vertical wall on x, 1 slides a horizontal wall on y. */
static int wall_slide_axis(const Wall* wall) {
    float dx = fabsf(wall->b.x - wall->a.x);
    float dy = fabsf(wall->b.y - wall->a.y);
    if (dy <= ORTHO_EPS && dx > ORTHO_EPS) return 1;
    if (dx <= ORTHO_EPS && dy > ORTHO_EPS) return 0;
    return -1;
}

static int shape_index_by_id(const Level* level, ShapeId id) {
    if (id == 0) return -1;
    for (int i = 0; i < level->shape_count; i++) {
        if (level->shapes[i].id == id) return i;
    }
    return -1;
}

static void remove_shape_at(Level* level, int shape_index) {
    memmove(&level->shapes[shape_index], &level->shapes[shape_index + 1],
            (size_t)(level->shape_count - shape_index - 1) * sizeof(Shape));
    level->shape_count--;
}

static void remove_wall_at(Level* level, int wall_index) {
    WallId id = level->walls[wall_index].id;
    memmove(&level->walls[wall_index], &level->walls[wall_index + 1],
            (size_t)(level->wall_count - wall_index - 1) * sizeof(Wall));
    level->wall_count--;

    int keep = 0;
    for (int i = 0; i < level->door_count; i++) {
        if (level->doors[i].wall == id) continue;
        if (keep != i) level->doors[keep] = level->doors[i];
        keep++;
    }
    level->door_count = keep;
}

static int append_wall(Level* level, ShapeId shape, Vec2 a, Vec2 b) {
    if (level->wall_count >= LEVEL_MAX_WALLS) return -1;
    if (!wall_ok(a, b)) return -1;

    int index = level->wall_count++;
    level->walls[index] = (Wall){ level->next_wall_id++, shape, a, b };
    return index;
}

static bool shape_has_walls(const Level* level, ShapeId id) {
    for (int i = 0; i < level->wall_count; i++) {
        if (level->walls[i].shape == id) return true;
    }
    return false;
}

/* Walks the shape's walls in insertion order, choosing each wall's start so the
   chain stays connected. Writes one start point per wall when out is non-NULL. */
static int shape_loop(const Level* level, ShapeId id, Vec2* out, int cap) {
    int indices[LEVEL_MAX_WALLS];
    int count = 0;
    for (int i = 0; i < level->wall_count; i++) {
        if (level->walls[i].shape == id) indices[count++] = i;
    }
    if (count < 3) return -1;

    const Wall* first = &level->walls[indices[0]];
    Vec2 start = first->a;
    Vec2 far = first->b;
    if (out && cap > 0) out[0] = start;

    for (int i = 1; i < count; i++) {
        const Wall* wall = &level->walls[indices[i]];
        Vec2 near;
        if (v2_equal(wall->a, far, ORTHO_EPS)) {
            near = wall->a;
            far = wall->b;
        } else if (v2_equal(wall->b, far, ORTHO_EPS)) {
            near = wall->b;
            far = wall->a;
        } else {
            return -1;
        }
        if (out && i < cap) out[i] = near;
    }
    if (!v2_equal(far, start, ORTHO_EPS)) return -1;
    return count;
}

static int clamp_steps(int steps) {
    if (steps < STAIR_MIN_STEPS) return STAIR_MIN_STEPS;
    if (steps > STAIR_MAX_STEPS) return STAIR_MAX_STEPS;
    return steps;
}

/* A stair footprint must be finite and at least STAIR_MIN_SIZE on both axes.
   Everything that accepts a Rect from outside the model checks this. */
static bool stairs_footprint_ok(Rect rect) {
    if (!isfinite(rect.min.x) || !isfinite(rect.min.y) ||
        !isfinite(rect.max.x) || !isfinite(rect.max.y)) {
        return false;
    }
    return rect.max.x - rect.min.x >= STAIR_MIN_SIZE &&
           rect.max.y - rect.min.y >= STAIR_MIN_SIZE;
}

static void copy_label_text(char* dst, const char* src) {
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= LEVEL_LABEL_LEN) n = LEVEL_LABEL_LEN - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void sanitize_label(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }
    dst[i] = '\0';
}

static void rect_grow(Rect* r, bool* has, Vec2 p) {
    if (!*has) {
        r->min = p;
        r->max = p;
        *has = true;
        return;
    }
    if (p.x < r->min.x) r->min.x = p.x;
    if (p.y < r->min.y) r->min.y = p.y;
    if (p.x > r->max.x) r->max.x = p.x;
    if (p.y > r->max.y) r->max.y = p.y;
}

static char* next_token(char** save) {
    return strtok_r(NULL, " \t\r\n", save);
}

static bool parse_float(const char* s, float* out) {
    char* end = NULL;
    errno = 0;
    float v = strtof(s, &end);
    if (end == s || *end != '\0' || !isfinite(v)) return false;
    *out = v;
    return true;
}

static bool parse_uint(const char* s, uint32_t* out) {
    if (s[0] == '-') return false;
    char* end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE || v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_int(const char* s, int* out) {
    char* end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE || v < INT_MIN || v > INT_MAX) {
        return false;
    }
    *out = (int)v;
    return true;
}

static bool read_float(char** save, float* out) {
    char* tok = next_token(save);
    return tok && parse_float(tok, out);
}

static bool read_uint(char** save, uint32_t* out) {
    char* tok = next_token(save);
    return tok && parse_uint(tok, out);
}

static bool read_int(char** save, int* out) {
    char* tok = next_token(save);
    return tok && parse_int(tok, out);
}

void level_init(Level* level) {
    memset(level, 0, sizeof *level);
    level->next_wall_id = 1;
    level->next_shape_id = 1;
}

bool level_is_empty(const Level* level) {
    return level->wall_count == 0 && level->room_count == 0 &&
           level->door_count == 0 && level->label_count == 0 &&
           level->stairs_count == 0;
}

Rect level_bounds(const Level* level) {
    Rect r = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };
    bool has = false;

    for (int i = 0; i < level->wall_count; i++) {
        rect_grow(&r, &has, level->walls[i].a);
        rect_grow(&r, &has, level->walls[i].b);
    }
    for (int i = 0; i < level->room_count; i++) {
        const Room* room = &level->rooms[i];
        for (int j = 0; j < room->count; j++) rect_grow(&r, &has, room->pts[j]);
    }
    for (int i = 0; i < level->label_count; i++) {
        const Label* label = &level->labels[i];
        Vec2 extent = {
            label->size * 0.6f * (float)strlen(label->text),
            label->size,
        };
        rect_grow(&r, &has, label->pos);
        rect_grow(&r, &has, v2_add(label->pos, extent));
    }
    for (int i = 0; i < level->stairs_count; i++) {
        Rect sr = level_stairs_rect(&level->stairs[i]);
        rect_grow(&r, &has, sr.min);
        rect_grow(&r, &has, sr.max);
    }
    return r;
}

int level_add_wall(Level* level, Vec2 a, Vec2 b) {
    if (level->wall_count >= LEVEL_MAX_WALLS) return -1;
    if (level->shape_count >= LEVEL_MAX_SHAPES) return -1;
    if (!wall_ok(a, b)) return -1;

    ShapeId shape = level_add_shape(level);
    if (shape == 0) return -1;
    return append_wall(level, shape, a, b);
}

ShapeId level_add_shape(Level* level) {
    if (level->shape_count >= LEVEL_MAX_SHAPES) return 0;

    ShapeId id = level->next_shape_id++;
    level->shapes[level->shape_count++] =
        (Shape){ id, false, SHAPE_FILL_NONE };
    return id;
}

Shape* level_shape_by_id(Level* level, ShapeId id) {
    int index = shape_index_by_id(level, id);
    return index < 0 ? NULL : &level->shapes[index];
}

const Shape* level_shape_by_id_const(const Level* level, ShapeId id) {
    int index = shape_index_by_id(level, id);
    return index < 0 ? NULL : &level->shapes[index];
}

bool level_set_shape_fill(Level* level, ShapeId id, ShapeFill fill) {
    if (fill != SHAPE_FILL_NONE && fill != SHAPE_FILL_BLUE &&
        fill != SHAPE_FILL_RED) {
        return false;
    }
    Shape* shape = level_shape_by_id(level, id);
    if (!shape) return false;
    shape->fill = fill;
    return true;
}

int level_add_wall_to_shape(Level* level, ShapeId shape, Vec2 a, Vec2 b) {
    if (!level_shape_by_id(level, shape)) return -1;
    return append_wall(level, shape, a, b);
}

bool level_close_shape(Level* level, ShapeId id) {
    Shape* shape = level_shape_by_id(level, id);
    if (!shape) return false;
    if (shape_loop(level, id, NULL, 0) < 3) return false;
    shape->closed = true;
    return true;
}

int level_shape_wall_indices(const Level* level, ShapeId id, int* out, int cap) {
    if (!level || !level_shape_by_id_const(level, id)) return -1;
    if (cap <= 0) return 0;

    int total = 0;
    for (int i = 0; i < level->wall_count; i++) {
        if (level->walls[i].shape != id) continue;
        if (out && total < cap) out[total] = i;
        total++;
    }
    return total < cap ? total : cap;
}

int level_shape_polygon(const Level* level, ShapeId id, Vec2* out, int cap) {
    const Shape* shape = level_shape_by_id_const(level, id);
    if (!shape || !shape->closed || !out || cap <= 0) return -1;

    int count = shape_loop(level, id, NULL, 0);
    if (count < 3 || cap < count) return -1;
    return shape_loop(level, id, out, cap);
}

int level_shape_at(const Level* level, Vec2 p) {
    if (!level) return -1;

    Vec2 polygon[LEVEL_MAX_WALLS];
    int best = -1;
    float best_area = INFINITY;
    for (int i = 0; i < level->shape_count; i++) {
        const Shape* shape = &level->shapes[i];
        if (!shape->closed) continue;
        int count = level_shape_polygon(level, shape->id, polygon,
                                        LEVEL_MAX_WALLS);
        if (count < 3 || !point_in_polygon(polygon, count, p)) continue;

        float area = polygon_area(polygon, count);
        if (area < best_area) {
            best_area = area;
            best = i;
        }
    }
    return best;
}

bool level_delete_wall(Level* level, int wall_index) {
    if (wall_index < 0 || wall_index >= level->wall_count) return false;
    ShapeId shape = level->walls[wall_index].shape;
    remove_wall_at(level, wall_index);

    if (shape == 0) return true;
    Shape* record = level_shape_by_id(level, shape);
    if (!record) return true;
    if (!shape_has_walls(level, shape)) {
        remove_shape_at(level, shape_index_by_id(level, shape));
        return true;
    }
    /* Removing an edge can break the loop. A closed shape that no longer
       closes would save a file the loader rejects, so reopen it. */
    if (record->closed && shape_loop(level, shape, NULL, 0) < 3) {
        record->closed = false;
    }
    return true;
}

bool level_delete_shape(Level* level, ShapeId id) {
    int index = shape_index_by_id(level, id);
    if (index < 0) return false;

    int keep = 0;
    for (int i = 0; i < level->door_count; i++) {
        int wall_index = level_find_wall_index(level, level->doors[i].wall);
        bool hosted = wall_index >= 0 &&
                      level->walls[wall_index].shape == id;
        if (hosted) continue;
        if (keep != i) level->doors[keep] = level->doors[i];
        keep++;
    }
    level->door_count = keep;

    for (int i = level->wall_count - 1; i >= 0; i--) {
        if (level->walls[i].shape == id) remove_wall_at(level, i);
    }
    remove_shape_at(level, index);
    return true;
}

Wall* level_wall_by_id(Level* level, WallId id) {
    if (id == 0) return NULL;
    for (int i = 0; i < level->wall_count; i++) {
        if (level->walls[i].id == id) return &level->walls[i];
    }
    return NULL;
}

const Wall* level_wall_by_id_const(const Level* level, WallId id) {
    if (id == 0) return NULL;
    for (int i = 0; i < level->wall_count; i++) {
        if (level->walls[i].id == id) return &level->walls[i];
    }
    return NULL;
}

int level_find_wall_index(const Level* level, WallId id) {
    if (id == 0) return -1;
    for (int i = 0; i < level->wall_count; i++) {
        if (level->walls[i].id == id) return i;
    }
    return -1;
}

bool wall_is_vertical(const Wall* wall) {
    return fabsf(wall->a.x - wall->b.x) <= ORTHO_EPS;
}

int level_nearest_wall(const Level* level, Vec2 p, float max_dist, float* out_t) {
    int best = -1;
    float best_dist = max_dist;
    float best_t = 0.0f;
    for (int i = 0; i < level->wall_count; i++) {
        float t = point_segment_t(p, level->walls[i].a, level->walls[i].b);
        float d = v2_dist(p, segment_point(level->walls[i].a, level->walls[i].b, t));
        if (d <= best_dist) {
            best = i;
            best_dist = d;
            best_t = t;
        }
    }
    if (best < 0) return -1;
    if (out_t) *out_t = best_t;
    return best;
}

int level_add_room(Level* level, const Vec2* pts, int count, RoomColor color) {
    if (!pts || count < 3 || count > LEVEL_MAX_ROOM_PTS) return -1;
    if (color != ROOM_BLUE && color != ROOM_RED) return -1;
    if (level->room_count >= LEVEL_MAX_ROOMS) return -1;

    Room* room = &level->rooms[level->room_count];
    memcpy(room->pts, pts, (size_t)count * sizeof(Vec2));
    room->count = polygon_simplify(room->pts, count);
    if (room->count < 3) return -1;
    room->color = color;
    return level->room_count++;
}

bool level_delete_room(Level* level, int room_index) {
    if (room_index < 0 || room_index >= level->room_count) return false;
    memmove(&level->rooms[room_index], &level->rooms[room_index + 1],
            (size_t)(level->room_count - room_index - 1) * sizeof(Room));
    level->room_count--;
    return true;
}

int level_room_at(const Level* level, Vec2 p) {
    for (int i = level->room_count - 1; i >= 0; i--) {
        if (point_in_polygon(level->rooms[i].pts, level->rooms[i].count, p)) return i;
    }
    return -1;
}

bool level_set_room_color(Level* level, int room_index, RoomColor color) {
    if (room_index < 0 || room_index >= level->room_count) return false;
    if (color != ROOM_BLUE && color != ROOM_RED) return false;
    level->rooms[room_index].color = color;
    return true;
}

bool level_move_room(Level* level, int room_index, Vec2 delta) {
    if (room_index < 0 || room_index >= level->room_count) return false;
    Room* room = &level->rooms[room_index];
    for (int i = 0; i < room->count; i++) room->pts[i] = v2_add(room->pts[i], delta);
    return true;
}

/* Coincident endpoints form one vertex group so they move as a unit. */
static int endpoint_group(Vec2* groups, int* count, Vec2 p) {
    for (int i = 0; i < *count; i++) {
        if (v2_equal(groups[i], p, ORTHO_EPS)) return i;
    }
    groups[*count] = p;
    return (*count)++;
}

bool level_slide_edges(Level* level, const bool* selected, int count, Vec2 delta,
                       float min_len) {
    if (!level || !selected) return false;
    if (count < 0 || count > level->wall_count) return false;
    if (!isfinite(delta.x) || !isfinite(delta.y)) return false;
    if (delta.x == 0.0f && delta.y == 0.0f) return false;

    Vec2 group_pos[LEVEL_MAX_WALLS * 2];
    bool group_x[LEVEL_MAX_WALLS * 2] = { false };
    bool group_y[LEVEL_MAX_WALLS * 2] = { false };
    int a_group[LEVEL_MAX_WALLS];
    int b_group[LEVEL_MAX_WALLS];
    int group_count = 0;
    for (int i = 0; i < level->wall_count; i++) {
        a_group[i] = endpoint_group(group_pos, &group_count, level->walls[i].a);
        b_group[i] = endpoint_group(group_pos, &group_count, level->walls[i].b);
    }

    bool move_x = false;
    bool move_y = false;
    for (int i = 0; i < count; i++) {
        if (!selected[i]) continue;
        int axis = wall_slide_axis(&level->walls[i]);
        if (axis == 0) {
            group_x[a_group[i]] = true;
            group_x[b_group[i]] = true;
            move_x = true;
        } else if (axis == 1) {
            group_y[a_group[i]] = true;
            group_y[b_group[i]] = true;
            move_y = true;
        }
    }
    if (!move_x && !move_y) return false;

    float keep = fmaxf(min_len, ORTHO_EPS);
    float lo_x = -INFINITY;
    float hi_x = INFINITY;
    float lo_y = -INFINITY;
    float hi_y = INFINITY;

    for (int i = 0; i < level->wall_count; i++) {
        const Wall* wall = &level->walls[i];
        if (group_x[a_group[i]] != group_x[b_group[i]]) {
            Vec2 fixed = group_x[a_group[i]] ? wall->b : wall->a;
            Vec2 moving = group_x[a_group[i]] ? wall->a : wall->b;
            float along = moving.x - fixed.x;
            float across = moving.y - fixed.y;
            if (fabsf(across) > ORTHO_EPS || fabsf(along) <= ORTHO_EPS) {
                lo_x = fmaxf(lo_x, 0.0f);
                hi_x = fminf(hi_x, 0.0f);
            } else if (along > 0.0f) {
                lo_x = fmaxf(lo_x, keep - along);
            } else {
                hi_x = fminf(hi_x, -keep - along);
            }
        }
        if (group_y[a_group[i]] != group_y[b_group[i]]) {
            Vec2 fixed = group_y[a_group[i]] ? wall->b : wall->a;
            Vec2 moving = group_y[a_group[i]] ? wall->a : wall->b;
            float along = moving.y - fixed.y;
            float across = moving.x - fixed.x;
            if (fabsf(across) > ORTHO_EPS || fabsf(along) <= ORTHO_EPS) {
                lo_y = fmaxf(lo_y, 0.0f);
                hi_y = fminf(hi_y, 0.0f);
            } else if (along > 0.0f) {
                lo_y = fmaxf(lo_y, keep - along);
            } else {
                hi_y = fminf(hi_y, -keep - along);
            }
        }
    }

    float applied_x = 0.0f;
    float applied_y = 0.0f;
    if (move_x && delta.x != 0.0f && lo_x <= hi_x) {
        applied_x = clampf(delta.x, lo_x, hi_x);
    }
    if (move_y && delta.y != 0.0f && lo_y <= hi_y) {
        applied_y = clampf(delta.y, lo_y, hi_y);
    }
    if (!isfinite(applied_x) || fabsf(applied_x) <= 1e-6f) applied_x = 0.0f;
    if (!isfinite(applied_y) || fabsf(applied_y) <= 1e-6f) applied_y = 0.0f;
    if (applied_x != 0.0f &&
        (applied_x > 0.0f) != (delta.x > 0.0f)) {
        applied_x = 0.0f;
    }
    if (applied_y != 0.0f &&
        (applied_y > 0.0f) != (delta.y > 0.0f)) {
        applied_y = 0.0f;
    }
    if (applied_x == 0.0f && applied_y == 0.0f) return false;

    for (int i = 0; i < level->wall_count; i++) {
        Wall* wall = &level->walls[i];
        if (group_x[a_group[i]]) wall->a.x += applied_x;
        if (group_y[a_group[i]]) wall->a.y += applied_y;
        if (group_x[b_group[i]]) wall->b.x += applied_x;
        if (group_y[b_group[i]]) wall->b.y += applied_y;
    }
    return true;
}

bool level_slide_wall_edge(Level* level, int wall_index, float delta,
                           float min_len) {
    if (!level || wall_index < 0 || wall_index >= level->wall_count) return false;
    if (!isfinite(delta) || delta == 0.0f) return false;

    int axis = wall_slide_axis(&level->walls[wall_index]);
    if (axis < 0) return false;

    bool selected[LEVEL_MAX_WALLS] = { false };
    selected[wall_index] = true;
    Vec2 projected = axis == 1 ? v2(0.0f, delta) : v2(delta, 0.0f);
    return level_slide_edges(level, selected, level->wall_count, projected,
                             min_len);
}

bool level_move_walls(Level* level, const bool* selected, int count, Vec2 delta) {
    if (!level || !selected) return false;
    if (count < 0 || count > level->wall_count) return false;
    if (!isfinite(delta.x) || !isfinite(delta.y)) return false;
    if (delta.x == 0.0f && delta.y == 0.0f) return false;

    bool moved = false;
    for (int i = 0; i < count; i++) {
        if (!selected[i]) continue;
        level->walls[i].a = v2_add(level->walls[i].a, delta);
        level->walls[i].b = v2_add(level->walls[i].b, delta);
        moved = true;
    }
    return moved;
}

int level_add_door(Level* level, WallId wall, float t, float width, DoorColor color) {
    if (level->door_count >= LEVEL_MAX_DOORS) return -1;
    if (!level_wall_by_id_const(level, wall)) return -1;

    int index = level->door_count++;
    level->doors[index] = (Door){ wall, t, width, color };
    level_set_door_span(level, index, t, width);
    return index;
}

bool level_delete_door(Level* level, int door_index) {
    if (door_index < 0 || door_index >= level->door_count) return false;
    memmove(&level->doors[door_index], &level->doors[door_index + 1],
            (size_t)(level->door_count - door_index - 1) * sizeof(Door));
    level->door_count--;
    return true;
}

bool level_set_door_span(Level* level, int door_index, float t, float width) {
    if (door_index < 0 || door_index >= level->door_count) return false;
    Door* door = &level->doors[door_index];
    const Wall* wall = level_wall_by_id_const(level, door->wall);
    if (!wall) return false;

    float len = wall_length(wall);
    if (!(len > 0.0f)) return false;

    if (!isfinite(width)) width = len;
    if (!isfinite(t)) t = 0.5f;
    width = fmaxf(width, DOOR_MIN_WIDTH);
    width = fminf(width, len);
    door->width = width;

    float half = width / (2.0f * len);
    door->t = half >= 0.5f ? 0.5f : clampf(t, half, 1.0f - half);
    return true;
}

bool level_set_door_color(Level* level, int door_index, DoorColor color) {
    if (door_index < 0 || door_index >= level->door_count) return false;
    level->doors[door_index].color = color;
    return true;
}

Vec2 level_door_center(const Level* level, const Door* door) {
    if (!door) return v2(0.0f, 0.0f);
    const Wall* wall = level_wall_by_id_const(level, door->wall);
    if (!wall) return v2(0.0f, 0.0f);
    return segment_point(wall->a, wall->b, door->t);
}

void level_door_span(const Level* level, const Door* door, Vec2* out_a, Vec2* out_b) {
    if (!door || !out_a || !out_b) return;
    const Wall* wall = level_wall_by_id_const(level, door->wall);
    if (!wall) {
        *out_a = v2(0.0f, 0.0f);
        *out_b = v2(0.0f, 0.0f);
        return;
    }
    Vec2 dir = v2_sub(wall->b, wall->a);
    float len = v2_len(dir);
    dir = len > 0.0f ? v2_scale(dir, 1.0f / len) : v2(0.0f, 0.0f);
    Vec2 half = v2_scale(dir, door->width * 0.5f);
    Vec2 center = segment_point(wall->a, wall->b, door->t);
    *out_a = v2_sub(center, half);
    *out_b = v2_add(center, half);
}

int level_add_label(Level* level, Vec2 pos, const char* text, float size) {
    if (level->label_count >= LEVEL_MAX_LABELS) return -1;
    if (!isfinite(size) || size < 6.0f) size = 6.0f;

    int index = level->label_count++;
    Label* label = &level->labels[index];
    label->pos = pos;
    label->size = size;
    copy_label_text(label->text, text);
    return index;
}

bool level_delete_label(Level* level, int label_index) {
    if (label_index < 0 || label_index >= level->label_count) return false;
    memmove(&level->labels[label_index], &level->labels[label_index + 1],
            (size_t)(level->label_count - label_index - 1) * sizeof(Label));
    level->label_count--;
    return true;
}

bool level_set_label_text(Level* level, int label_index, const char* text) {
    if (label_index < 0 || label_index >= level->label_count) return false;
    copy_label_text(level->labels[label_index].text, text);
    return true;
}

bool level_set_label_size(Level* level, int label_index, float size) {
    if (label_index < 0 || label_index >= level->label_count) return false;
    if (!isfinite(size) || size < 6.0f) size = 6.0f;
    level->labels[label_index].size = size;
    return true;
}

bool level_move_label(Level* level, int label_index, Vec2 pos) {
    if (label_index < 0 || label_index >= level->label_count) return false;
    level->labels[label_index].pos = pos;
    return true;
}

int level_add_stairs(Level* level, Rect rect, StairsDir dir, int steps) {
    if (level->stairs_count >= LEVEL_MAX_STAIRS) return -1;
    if (dir != STAIRS_H && dir != STAIRS_V) return -1;
    if (!stairs_footprint_ok(rect)) return -1;

    int index = level->stairs_count++;
    level->stairs[index] = (Stairs){
        rect.min,
        rect.max.x - rect.min.x,
        rect.max.y - rect.min.y,
        clamp_steps(steps),
        dir,
    };
    return index;
}

bool level_delete_stairs(Level* level, int stairs_index) {
    if (stairs_index < 0 || stairs_index >= level->stairs_count) return false;
    memmove(&level->stairs[stairs_index], &level->stairs[stairs_index + 1],
            (size_t)(level->stairs_count - stairs_index - 1) * sizeof(Stairs));
    level->stairs_count--;
    return true;
}

bool level_set_stairs(Level* level, int stairs_index, StairsDir dir, int steps) {
    if (stairs_index < 0 || stairs_index >= level->stairs_count) return false;
    if (dir != STAIRS_H && dir != STAIRS_V) return false;
    level->stairs[stairs_index].dir = dir;
    level->stairs[stairs_index].steps = clamp_steps(steps);
    return true;
}

bool level_set_stairs_rect(Level* level, int stairs_index, Rect rect) {
    if (stairs_index < 0 || stairs_index >= level->stairs_count) return false;
    if (!stairs_footprint_ok(rect)) return false;
    Stairs* stairs = &level->stairs[stairs_index];
    stairs->pos = rect.min;
    stairs->width = rect.max.x - rect.min.x;
    stairs->length = rect.max.y - rect.min.y;
    return true;
}

bool level_move_stairs(Level* level, int stairs_index, Vec2 pos) {
    if (stairs_index < 0 || stairs_index >= level->stairs_count) return false;
    level->stairs[stairs_index].pos = pos;
    return true;
}

Rect level_stairs_rect(const Stairs* stairs) {
    return (Rect){
        stairs->pos,
        { stairs->pos.x + stairs->width, stairs->pos.y + stairs->length },
    };
}

float wall_length(const Wall* wall) {
    return v2_dist(wall->a, wall->b);
}

bool level_save(const Level* level, const char* path, LevelError* err) {
    if (err) err->message[0] = '\0';
    if (!level || !path) {
        set_error(err, "invalid argument");
        return false;
    }

    char tmp[4096];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof tmp) {
        set_error(err, "path too long");
        return false;
    }

    FILE* f = fopen(tmp, "wb");
    if (!f) {
        set_error(err, "cannot open %s: %s", tmp, strerror(errno));
        return false;
    }

    bool ok = fprintf(f, "ink-level 3\n") >= 0;

    for (int i = 0; ok && i < level->shape_count; i++) {
        const Shape* shape = &level->shapes[i];
        ok = fprintf(f, "shape %" PRIu32 " %d %d\n", shape->id,
                     shape->closed ? 1 : 0, (int)shape->fill) >= 0;
    }
    for (int i = 0; ok && i < level->wall_count; i++) {
        const Wall* w = &level->walls[i];
        ok = fprintf(f, "wall %" PRIu32 " %" PRIu32 " %.3f %.3f %.3f %.3f\n",
                     w->id, w->shape, w->a.x, w->a.y, w->b.x, w->b.y) >= 0;
    }
    for (int i = 0; ok && i < level->room_count; i++) {
        const Room* room = &level->rooms[i];
        ok = fprintf(f, "room %d %d", (int)room->color, room->count) >= 0;
        for (int j = 0; ok && j < room->count; j++) {
            ok = fprintf(f, " %.3f %.3f", room->pts[j].x, room->pts[j].y) >= 0;
        }
        if (ok) ok = fputc('\n', f) != EOF;
    }
    for (int i = 0; ok && i < level->door_count; i++) {
        const Door* d = &level->doors[i];
        ok = fprintf(f, "door %d %" PRIu32 " %.3f %.3f\n",
                     (int)d->color, d->wall, d->t, d->width) >= 0;
    }
    for (int i = 0; ok && i < level->label_count; i++) {
        const Label* label = &level->labels[i];
        char text[LEVEL_LABEL_LEN];
        sanitize_label(text, sizeof text, label->text);
        ok = fprintf(f, "label %.3f %.3f %.3f %s\n",
                     label->pos.x, label->pos.y, label->size, text) >= 0;
    }
    for (int i = 0; ok && i < level->stairs_count; i++) {
        const Stairs* s = &level->stairs[i];
        ok = fprintf(f, "stairs %.3f %.3f %d %d %.3f %.3f\n",
                     s->pos.x, s->pos.y, (int)s->dir, s->steps,
                     s->width, s->length) >= 0;
    }

    int write_errno = 0;
    if (ok && fflush(f) != 0) {
        ok = false;
        write_errno = errno;
    }
    if (ok && ferror(f)) {
        ok = false;
        write_errno = errno;
    }
    if (fclose(f) != 0) {
        ok = false;
        if (write_errno == 0) write_errno = errno;
    }

    if (!ok) {
        remove(tmp);
        if (write_errno == 0) write_errno = EIO;
        set_error(err, "cannot write %s: %s", path, strerror(write_errno));
        return false;
    }
    if (rename(tmp, path) != 0) {
        int rename_errno = errno;
        remove(tmp);
        set_error(err, "cannot replace %s: %s", path, strerror(rename_errno));
        return false;
    }
    return true;
}

bool level_load(Level* level, const char* path, LevelError* err) {
    if (err) err->message[0] = '\0';
    if (!level || !path) {
        set_error(err, "invalid argument");
        return false;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        set_error(err, "line 1: cannot open %s: %s", path, strerror(errno));
        return false;
    }

    Level tmp;
    level_init(&tmp);
    int door_lines[LEVEL_MAX_DOORS];
    int wall_lines[LEVEL_MAX_WALLS];
    int shape_lines[LEVEL_MAX_SHAPES];
    int version = 0;
    char buf[LOAD_LINE_MAX];
    int line_no = 0;
    bool saw_header = false;
    bool ok = true;

    while (ok && fgets(buf, sizeof buf, f)) {
        line_no++;
        size_t len = strlen(buf);
        if (len == sizeof buf - 1 && buf[len - 1] != '\n' && !feof(f)) {
            ok = fail_line(err, line_no, "line too long");
            break;
        }

        char* p = buf;
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;

        char* save = NULL;
        char* tag = strtok_r(p, " \t\r\n", &save);
        if (!tag) continue;

        if (!saw_header) {
            char* version_text = next_token(&save);
            char* extra = next_token(&save);
            if (strcmp(tag, "ink-level") != 0 || !version_text || extra ||
                (strcmp(version_text, "1") != 0 &&
                 strcmp(version_text, "2") != 0 &&
                 strcmp(version_text, "3") != 0)) {
                ok = fail_line(err, line_no, "expected ink-level 1, 2, or 3");
                break;
            }
            version = version_text[0] - '0';
            saw_header = true;
            continue;
        }

        if (strcmp(tag, "shape") == 0) {
            uint32_t id;
            int closed, fill;
            if (version < 2) {
                ok = fail_line(err, line_no,
                               "shape record requires ink-level 2 or later");
                break;
            }
            if (!read_uint(&save, &id) || !read_int(&save, &closed) ||
                !read_int(&save, &fill) || next_token(&save)) {
                ok = fail_line(err, line_no, "malformed shape record");
                break;
            }
            if (id == 0) {
                ok = fail_line(err, line_no, "shape id must be non-zero");
                break;
            }
            if (closed != 0 && closed != 1) {
                ok = fail_line(err, line_no, "bad shape closed flag");
                break;
            }
            if (fill < 0 || fill > 2) {
                ok = fail_line(err, line_no, "bad shape fill");
                break;
            }
            if (tmp.shape_count >= LEVEL_MAX_SHAPES) {
                ok = fail_line(err, line_no, "too many shapes");
                break;
            }
            if (shape_index_by_id(&tmp, id) >= 0) {
                ok = fail_line(err, line_no, "duplicate shape id");
                break;
            }
            shape_lines[tmp.shape_count] = line_no;
            tmp.shapes[tmp.shape_count++] =
                (Shape){ id, closed != 0, (ShapeFill)fill };
        } else if (strcmp(tag, "wall") == 0) {
            uint32_t id, shape = 0;
            float x1, y1, x2, y2;
            if (!read_uint(&save, &id)) {
                ok = fail_line(err, line_no, "malformed wall record");
                break;
            }
            if (version >= 2 && !read_uint(&save, &shape)) {
                ok = fail_line(err, line_no, "malformed wall record");
                break;
            }
            if (!read_float(&save, &x1) || !read_float(&save, &y1) ||
                !read_float(&save, &x2) || !read_float(&save, &y2) ||
                next_token(&save)) {
                ok = fail_line(err, line_no, "malformed wall record");
                break;
            }
            Vec2 a = v2(x1, y1);
            Vec2 b = v2(x2, y2);
            if (id == 0) {
                ok = fail_line(err, line_no, "wall id must be non-zero");
                break;
            }
            if (tmp.wall_count >= LEVEL_MAX_WALLS) {
                ok = fail_line(err, line_no, "too many walls");
                break;
            }
            if (level_find_wall_index(&tmp, id) >= 0) {
                ok = fail_line(err, line_no, "duplicate wall id");
                break;
            }
            if (!wall_ok(a, b)) {
                bool zero = fabsf(b.x - a.x) <= ORTHO_EPS &&
                            fabsf(b.y - a.y) <= ORTHO_EPS;
                ok = fail_line(err, line_no,
                               zero ? "zero-length wall" : "non-orthogonal wall");
                break;
            }
            wall_lines[tmp.wall_count] = line_no;
            tmp.walls[tmp.wall_count++] = (Wall){ id, shape, a, b };
        } else if (strcmp(tag, "room") == 0) {
            int color, count;
            if (!read_int(&save, &color) || !read_int(&save, &count)) {
                ok = fail_line(err, line_no, "malformed room record");
                break;
            }
            if (color < 0 || color > 1) {
                ok = fail_line(err, line_no, "bad room color");
                break;
            }
            if (count < 3 || count > LEVEL_MAX_ROOM_PTS) {
                ok = fail_line(err, line_no, "bad room point count");
                break;
            }
            if (tmp.room_count >= LEVEL_MAX_ROOMS) {
                ok = fail_line(err, line_no, "too many rooms");
                break;
            }
            Room* room = &tmp.rooms[tmp.room_count];
            room->color = (RoomColor)color;
            room->count = count;
            bool coords_ok = true;
            for (int i = 0; i < count; i++) {
                float x, y;
                if (!read_float(&save, &x) || !read_float(&save, &y)) {
                    coords_ok = false;
                    break;
                }
                room->pts[i] = v2(x, y);
            }
            if (!coords_ok || next_token(&save)) {
                ok = fail_line(err, line_no, "malformed room record");
                break;
            }
            tmp.room_count++;
        } else if (strcmp(tag, "door") == 0) {
            int color;
            uint32_t wall;
            float t, width;
            if (!read_int(&save, &color) || !read_uint(&save, &wall) ||
                !read_float(&save, &t) || !read_float(&save, &width) ||
                next_token(&save)) {
                ok = fail_line(err, line_no, "malformed door record");
                break;
            }
            if (color < 0 || color > 2) {
                ok = fail_line(err, line_no, "bad door color");
                break;
            }
            if (tmp.door_count >= LEVEL_MAX_DOORS) {
                ok = fail_line(err, line_no, "too many doors");
                break;
            }
            door_lines[tmp.door_count] = line_no;
            tmp.doors[tmp.door_count++] =
                (Door){ wall, t, width, (DoorColor)color };
        } else if (strcmp(tag, "label") == 0) {
            float x, y, size;
            if (!read_float(&save, &x) || !read_float(&save, &y) ||
                !read_float(&save, &size)) {
                ok = fail_line(err, line_no, "malformed label record");
                break;
            }
            char* text = save;
            while (*text == ' ' || *text == '\t') text++;
            size_t text_len = strlen(text);
            while (text_len > 0 &&
                   (text[text_len - 1] == '\n' || text[text_len - 1] == '\r')) {
                text[--text_len] = '\0';
            }
            if (tmp.label_count >= LEVEL_MAX_LABELS) {
                ok = fail_line(err, line_no, "too many labels");
                break;
            }
            Label* label = &tmp.labels[tmp.label_count++];
            label->pos = v2(x, y);
            label->size = isfinite(size) && size >= 6.0f ? size : 6.0f;
            copy_label_text(label->text, text);
        } else if (strcmp(tag, "stairs") == 0) {
            float x = 0, y = 0, width = 0, length = 0;
            int dir, steps;
            if (!read_float(&save, &x) || !read_float(&save, &y) ||
                !read_int(&save, &dir) || !read_int(&save, &steps) ||
                (version == 3 && (!read_float(&save, &width) ||
                                  !read_float(&save, &length))) ||
                next_token(&save)) {
                ok = fail_line(err, line_no, "malformed stairs record");
                break;
            }
            if (dir != STAIRS_H && dir != STAIRS_V) {
                ok = fail_line(err, line_no, "bad stairs direction");
                break;
            }
            if (tmp.stairs_count >= LEVEL_MAX_STAIRS) {
                ok = fail_line(err, line_no, "too many stairs");
                break;
            }
            int kept_steps = clamp_steps(steps);
            if (version < 3) {
                /* Version 1 and 2 stored the centre and derived the size from
                   the step count. Convert to the explicit min-corner box. */
                float run = (float)kept_steps * STAIR_STEP_DEPTH;
                width = dir == STAIRS_V ? STAIR_WIDTH : run;
                length = dir == STAIRS_V ? run : STAIR_WIDTH;
                x -= width * 0.5f;
                y -= length * 0.5f;
            }
            Rect box = { { x, y }, { x + width, y + length } };
            if (!stairs_footprint_ok(box)) {
                ok = fail_line(err, line_no, "bad stairs footprint");
                break;
            }
            tmp.stairs[tmp.stairs_count++] =
                (Stairs){ box.min, width, length, kept_steps, (StairsDir)dir };
        } else {
            ok = fail_line(err, line_no, "unknown record '%s'", tag);
        }
    }

    if (ok && ferror(f)) ok = fail_line(err, line_no + 1, "read error");
    fclose(f);
    if (!ok) return false;

    if (!saw_header) {
        return fail_line(err, line_no + 1, "expected ink-level 1, 2, or 3");
    }

    if (version == 1) {
        for (int i = 0; i < tmp.wall_count; i++) {
            ShapeId shape = level_add_shape(&tmp);
            if (shape == 0) {
                return fail_line(err, 1, "too many shapes");
            }
            tmp.walls[i].shape = shape;
        }
    } else {
        for (int i = 0; i < tmp.wall_count; i++) {
            if (!level_shape_by_id_const(&tmp, tmp.walls[i].shape)) {
                return fail_line(err, wall_lines[i],
                                 "wall references unknown shape %" PRIu32,
                                 tmp.walls[i].shape);
            }
        }
        for (int i = 0; i < tmp.shape_count; i++) {
            if (!tmp.shapes[i].closed) continue;
            int walls = 0;
            for (int j = 0; j < tmp.wall_count; j++) {
                if (tmp.walls[j].shape == tmp.shapes[i].id) walls++;
            }
            if (walls < 3) {
                return fail_line(err, shape_lines[i],
                                 "closed shape needs at least three walls");
            }
        }
    }

    for (int i = 0; i < tmp.door_count; i++) {
        if (!level_wall_by_id_const(&tmp, tmp.doors[i].wall)) {
            return fail_line(err, door_lines[i],
                             "door references unknown wall %" PRIu32,
                             tmp.doors[i].wall);
        }
        level_set_door_span(&tmp, i, tmp.doors[i].t, tmp.doors[i].width);
    }

    tmp.next_wall_id = 1;
    for (int i = 0; i < tmp.wall_count; i++) {
        if (tmp.walls[i].id >= tmp.next_wall_id) {
            tmp.next_wall_id = tmp.walls[i].id + 1;
        }
    }
    tmp.next_shape_id = 1;
    for (int i = 0; i < tmp.shape_count; i++) {
        if (tmp.shapes[i].id >= tmp.next_shape_id) {
            tmp.next_shape_id = tmp.shapes[i].id + 1;
        }
    }

    *level = tmp;
    return true;
}

bool history_init(History* history) {
    history->snapshots = malloc((size_t)HISTORY_MAX * sizeof(Level));
    if (!history->snapshots) {
        history->count = 0;
        history->cursor = 0;
        history->capacity = 0;
        return false;
    }
    history->count = 0;
    history->cursor = 0;
    history->capacity = HISTORY_MAX;
    return true;
}

void history_shutdown(History* history) {
    free(history->snapshots);
    history->snapshots = NULL;
    history->count = 0;
    history->cursor = 0;
    history->capacity = 0;
}

void history_reset(History* history, const Level* level) {
    if (!history->snapshots || history->capacity <= 0) return;
    history->snapshots[0] = *level;
    history->count = 1;
    history->cursor = 0;
}

void history_commit(History* history, const Level* level) {
    if (!history->snapshots || history->capacity <= 0) return;
    if (history->count <= 0) {
        history->snapshots[0] = *level;
        history->count = 1;
        history->cursor = 0;
        return;
    }

    int live = history->cursor + 1;
    if (live >= history->capacity) {
        memmove(&history->snapshots[0], &history->snapshots[1],
                (size_t)(history->capacity - 1) * sizeof(Level));
        live = history->capacity - 1;
    }
    history->snapshots[live] = *level;
    history->count = live + 1;
    history->cursor = live;
}

bool history_undo(History* history, Level* level) {
    if (!history->snapshots || history->cursor <= 0) return false;
    history->cursor--;
    *level = history->snapshots[history->cursor];
    return true;
}

bool history_redo(History* history, Level* level) {
    if (!history->snapshots || history->cursor + 1 >= history->count) return false;
    history->cursor++;
    *level = history->snapshots[history->cursor];
    return true;
}

bool history_restore_current(History* history, Level* level) {
    if (!history || !history->snapshots || history->cursor < 0 ||
        history->cursor >= history->count) {
        return false;
    }
    *level = history->snapshots[history->cursor];
    return true;
}

bool history_can_undo(const History* history) {
    return history->snapshots && history->count > 0 && history->cursor > 0;
}

bool history_can_redo(const History* history) {
    return history->snapshots && history->count > 0 &&
           history->cursor + 1 < history->count;
}
