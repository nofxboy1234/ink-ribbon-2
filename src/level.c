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

static int clamp_steps(int steps) {
    if (steps < STAIR_MIN_STEPS) return STAIR_MIN_STEPS;
    if (steps > STAIR_MAX_STEPS) return STAIR_MAX_STEPS;
    return steps;
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
    if (!wall_ok(a, b)) return -1;

    int index = level->wall_count++;
    level->walls[index] = (Wall){ level->next_wall_id++, a, b };
    return index;
}

bool level_delete_wall(Level* level, int wall_index) {
    if (wall_index < 0 || wall_index >= level->wall_count) return false;
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

bool level_move_wall(Level* level, int wall_index, Vec2 delta) {
    if (wall_index < 0 || wall_index >= level->wall_count) return false;
    if (!isfinite(delta.x) || !isfinite(delta.y)) return false;
    if (delta.x == 0.0f && delta.y == 0.0f) return false;

    Wall* wall = &level->walls[wall_index];
    wall->a = v2_add(wall->a, delta);
    wall->b = v2_add(wall->b, delta);
    return true;
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

int level_add_stairs(Level* level, Vec2 pos, StairsDir dir, int steps) {
    if (level->stairs_count >= LEVEL_MAX_STAIRS) return -1;
    if (dir != STAIRS_H && dir != STAIRS_V) return -1;

    int index = level->stairs_count++;
    level->stairs[index] = (Stairs){ pos, clamp_steps(steps), dir };
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

bool level_move_stairs(Level* level, int stairs_index, Vec2 pos) {
    if (stairs_index < 0 || stairs_index >= level->stairs_count) return false;
    level->stairs[stairs_index].pos = pos;
    return true;
}

Rect level_stairs_rect(const Stairs* stairs) {
    float along = (float)stairs->steps * STAIR_STEP_DEPTH;
    float across = STAIR_WIDTH;
    float w = stairs->dir == STAIRS_V ? across : along;
    float h = stairs->dir == STAIRS_V ? along : across;
    return (Rect){
        { stairs->pos.x - w * 0.5f, stairs->pos.y - h * 0.5f },
        { stairs->pos.x + w * 0.5f, stairs->pos.y + h * 0.5f },
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

    bool ok = fprintf(f, "ink-level 1\n") >= 0;

    for (int i = 0; ok && i < level->wall_count; i++) {
        const Wall* w = &level->walls[i];
        ok = fprintf(f, "wall %" PRIu32 " %.3f %.3f %.3f %.3f\n",
                     w->id, w->a.x, w->a.y, w->b.x, w->b.y) >= 0;
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
        ok = fprintf(f, "stairs %.3f %.3f %d %d\n",
                     s->pos.x, s->pos.y, (int)s->dir, s->steps) >= 0;
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
            char* version = next_token(&save);
            char* extra = next_token(&save);
            if (strcmp(tag, "ink-level") != 0 || !version ||
                strcmp(version, "1") != 0 || extra) {
                ok = fail_line(err, line_no, "expected ink-level 1");
                break;
            }
            saw_header = true;
            continue;
        }

        if (strcmp(tag, "wall") == 0) {
            uint32_t id;
            float x1, y1, x2, y2;
            if (!read_uint(&save, &id) || !read_float(&save, &x1) ||
                !read_float(&save, &y1) || !read_float(&save, &x2) ||
                !read_float(&save, &y2) || next_token(&save)) {
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
            tmp.walls[tmp.wall_count++] = (Wall){ id, a, b };
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
            float x, y;
            int dir, steps;
            if (!read_float(&save, &x) || !read_float(&save, &y) ||
                !read_int(&save, &dir) || !read_int(&save, &steps) ||
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
            tmp.stairs[tmp.stairs_count++] =
                (Stairs){ v2(x, y), clamp_steps(steps), (StairsDir)dir };
        } else {
            ok = fail_line(err, line_no, "unknown record '%s'", tag);
        }
    }

    if (ok && ferror(f)) ok = fail_line(err, line_no + 1, "read error");
    fclose(f);
    if (!ok) return false;

    if (!saw_header) {
        return fail_line(err, line_no + 1, "expected ink-level 1");
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
