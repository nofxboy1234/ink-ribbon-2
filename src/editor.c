#include "editor.h"

#include "cimgui.h"
#include "geom.h"
#include "level.h"

#include <dirent.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PANEL_LEFT_W 190.0f
#define PANEL_RIGHT_W 300.0f
#define STATUS_H 24.0f
#define FIT_MARGIN 40.0f
#define HIT_TOL_PX 7.0f
#define DOOR_TOL_PX 7.0f
#define CLOSE_TOL_PX 12.0f
#define DRAG_END_PX 8.0f
#define CANVAS_BG 0x0b0c0e
#define GRID_COL 0x15171a
#define GRID_BRIGHT 0x1d2126
#define WALL_COL 0x8b9097
#define ROOM_BLUE_COL 0x2b3f50
#define ROOM_RED_COL 0x4a2028
#define DOOR_WHITE_COL 0xf2f2f2
#define DOOR_RED_COL 0xb3323b
#define DOOR_BLUE_COL 0x3d7fb5
#define LABEL_COL 0xe6e6e6
#define STAIR_COL 0x767d85
#define STAIR_FILL 0x1c2026
#define ACCENT_COL 0x5aa0c8
#define SELECT_COL 0x5aa0c8
#define TEXT_COL 0xd9dadd
#define MUTED_COL 0x7f868e
#define ERROR_COL 0xd07373
#define PANEL_BG 0x16181c
#define WINDOW_BG 0x101114
#define FRAME_BG 0x1d2025
#define BORDER_COL 0x2a2e34
#define ACCENT_UI 0x4e7f9e

typedef enum {
    TOOL_SELECT = 0,
    TOOL_WALL,
    TOOL_ROOM,
    TOOL_DOOR,
    TOOL_LABEL,
    TOOL_STAIRS,
    TOOL_COUNT,
} Tool;

typedef enum {
    SEL_NONE = 0,
    SEL_WALL,
    SEL_ROOM,
    SEL_DOOR,
    SEL_LABEL,
    SEL_STAIRS,
    SEL_KIND_COUNT,
} SelKind;

typedef enum {
    MODAL_NONE = 0,
    MODAL_OPEN,
    MODAL_SAVE_AS,
    MODAL_HELP,
} Modal;

typedef enum {
    GESTURE_NONE = 0,
    GESTURE_CHAIN,
    GESTURE_POLYGON,
    GESTURE_MARQUEE,
    GESTURE_MOVE,
    GESTURE_DOOR_SLIDE,
    GESTURE_DOOR_RESIZE,
} GestureKind;

typedef struct {
    GestureKind kind;
    Vec2 pts[LEVEL_MAX_ROOM_PTS];
    int point_count;
    int wall_count;
    bool truncated;
    Vec2 press_screen;
    Vec2 press_world;
    Vec2 cur_world;
    Vec2 anchor;
    Vec2 applied;
    int object_index;
    bool additive;
    bool moved;
    float fixed_t;
} Gesture;

typedef struct {
    Vec2 center;
    float zoom;
} Camera;

typedef struct {
    SelKind kind;
    int index;
} SelRef;

typedef struct {
    bool walls[LEVEL_MAX_WALLS];
    bool rooms[LEVEL_MAX_ROOMS];
    bool doors[LEVEL_MAX_DOORS];
    bool labels[LEVEL_MAX_LABELS];
    bool stairs[LEVEL_MAX_STAIRS];
    SelRef primary;
} Selection;

typedef struct {
    SelKind kind;
    int index;
} Hit;

struct Editor {
    Level level;
    History history;
    Camera camera;
    Rect canvas;
    float menu_height;
    float display_w;
    float display_h;

    Tool tool;
    Selection selection;
    Gesture gesture;
    bool fit_pending;
    bool pan_active;

    float grid_size;
    bool snap;
    bool show_grid;
    RoomColor fill_color;
    DoorColor door_color;
    StairsDir stairs_dir;
    int stairs_steps;

    char current_path[512];
    char path_buf[512];
    char status[512];
    bool status_error;
    Modal modal;
    Modal request_modal;
    bool path_focus;
    char modal_error[512];

    char label_buf[LEVEL_LABEL_LEN];
    bool label_buf_ready;
    bool focus_label_input;

    Vec2 cursor_world;
    bool cursor_valid;
};

typedef struct {
    float a, b;
} Span;

static const char* TOOL_NAMES[TOOL_COUNT] = {
    "Select", "Wall", "Room", "Door", "Label", "Stairs",
};

static const char* TOOL_HINTS[TOOL_COUNT] = {
    "click to select, shift-click adds, drag empty canvas to marquee",
    "click to add walls, click first point or Enter closes",
    "click to draw a fill, Enter or double-click closes",
    "click a wall to place a door",
    "click to place a room label",
    "click to place stairs",
};

static ImU32 col32(unsigned rgb) {
    return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, 255);
}

static ImU32 col32a(unsigned rgb, unsigned alpha) {
    return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, alpha);
}

static ImVec4 v4c(unsigned rgb, float alpha) {
    return (ImVec4){
        ((rgb >> 16) & 0xff) / 255.0f,
        ((rgb >> 8) & 0xff) / 255.0f,
        (rgb & 0xff) / 255.0f,
        alpha,
    };
}

static ImU32 room_color(RoomColor c) {
    return col32(c == ROOM_RED ? ROOM_RED_COL : ROOM_BLUE_COL);
}

static ImU32 door_color(DoorColor c) {
    switch (c) {
        case DOOR_RED: return col32(DOOR_RED_COL);
        case DOOR_BLUE: return col32(DOOR_BLUE_COL);
        case DOOR_WHITE:
        default: return col32(DOOR_WHITE_COL);
    }
}

static void set_status(Editor* e, bool error, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->status, sizeof e->status, fmt, ap);
    va_end(ap);
    e->status_error = error;
}

static void commit(Editor* e, const char* message) {
    history_commit(&e->history, &e->level);
    if (message && message[0]) set_status(e, false, "%s", message);
}

static bool file_exists(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static Vec2 screen_to_world(const Editor* e, ImVec2 p) {
    float cx = (e->canvas.min.x + e->canvas.max.x) * 0.5f;
    float cy = (e->canvas.min.y + e->canvas.max.y) * 0.5f;
    float z = e->camera.zoom > 0.0001f ? e->camera.zoom : 1.0f;
    return v2(e->camera.center.x + (p.x - cx) / z,
              e->camera.center.y + (p.y - cy) / z);
}

static ImVec2 world_to_screen(const Editor* e, Vec2 p) {
    float cx = (e->canvas.min.x + e->canvas.max.x) * 0.5f;
    float cy = (e->canvas.min.y + e->canvas.max.y) * 0.5f;
    return (ImVec2){
        cx + (p.x - e->camera.center.x) * e->camera.zoom,
        cy + (p.y - e->camera.center.y) * e->camera.zoom,
    };
}

static Vec2 snapped(const Editor* e, Vec2 p) {
    return e->snap ? snap_grid(p, e->grid_size) : p;
}

static Vec2 constrained(const Editor* e, Vec2 anchor, Vec2 cursor) {
    if (e->snap) return snap_ortho(anchor, cursor, e->grid_size);
    Vec2 d = v2_sub(cursor, anchor);
    if (fabsf(d.x) > fabsf(d.y)) return v2(cursor.x, anchor.y);
    return v2(anchor.x, cursor.y);
}

static float snap_scalar(float value, float grid) {
    return snap_grid(v2(value, 0.0f), grid).x;
}

static float snap_wall_t(const Wall* wall, float t, float grid) {
    Vec2 p = segment_point(wall->a, wall->b, t);
    if (wall_is_vertical(wall)) p.y = snap_scalar(p.y, grid);
    else p.x = snap_scalar(p.x, grid);
    return point_segment_t(p, wall->a, wall->b);
}

static bool primary_anchor(const Editor* e, Vec2* out) {
    SelRef ref = e->selection.primary;
    switch (ref.kind) {
        case SEL_LABEL:
            if (ref.index < 0 || ref.index >= e->level.label_count) return false;
            *out = e->level.labels[ref.index].pos;
            return true;
        case SEL_STAIRS:
            if (ref.index < 0 || ref.index >= e->level.stairs_count) return false;
            *out = e->level.stairs[ref.index].pos;
            return true;
        case SEL_ROOM:
            if (ref.index < 0 || ref.index >= e->level.room_count) return false;
            if (e->level.rooms[ref.index].count <= 0) return false;
            *out = e->level.rooms[ref.index].pts[0];
            return true;
        case SEL_WALL:
            if (ref.index < 0 || ref.index >= e->level.wall_count) return false;
            *out = e->level.walls[ref.index].a;
            return true;
        case SEL_DOOR:
            if (ref.index < 0 || ref.index >= e->level.door_count) return false;
            if (!level_wall_by_id_const(&e->level, e->level.doors[ref.index].wall)) {
                return false;
            }
            *out = level_door_center(&e->level, &e->level.doors[ref.index]);
            return true;
        default:
            return false;
    }
}

static float screen_dist(ImVec2 a, ImVec2 b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return sqrtf(dx * dx + dy * dy);
}

static ImVec2 v2_to_im(Vec2 p) {
    return (ImVec2){ p.x, p.y };
}

static bool gesture_active(const Editor* e) {
    return e->gesture.kind != GESTURE_NONE;
}

static bool gesture_is_drag(const Editor* e) {
    return e->gesture.kind == GESTURE_MOVE || e->gesture.kind == GESTURE_DOOR_SLIDE ||
           e->gesture.kind == GESTURE_DOOR_RESIZE || e->gesture.kind == GESTURE_MARQUEE;
}

static void cancel_gesture(Editor* e) {
    memset(&e->gesture, 0, sizeof e->gesture);
}

static bool* sel_pool(Selection* s, SelKind kind) {
    switch (kind) {
        case SEL_WALL: return s->walls;
        case SEL_ROOM: return s->rooms;
        case SEL_DOOR: return s->doors;
        case SEL_LABEL: return s->labels;
        case SEL_STAIRS: return s->stairs;
        case SEL_NONE:
        default: return NULL;
    }
}

static const bool* sel_pool_const(const Selection* s, SelKind kind) {
    switch (kind) {
        case SEL_WALL: return s->walls;
        case SEL_ROOM: return s->rooms;
        case SEL_DOOR: return s->doors;
        case SEL_LABEL: return s->labels;
        case SEL_STAIRS: return s->stairs;
        case SEL_NONE:
        default: return NULL;
    }
}

static int sel_cap(SelKind kind) {
    switch (kind) {
        case SEL_WALL: return LEVEL_MAX_WALLS;
        case SEL_ROOM: return LEVEL_MAX_ROOMS;
        case SEL_DOOR: return LEVEL_MAX_DOORS;
        case SEL_LABEL: return LEVEL_MAX_LABELS;
        case SEL_STAIRS: return LEVEL_MAX_STAIRS;
        case SEL_NONE:
        default: return 0;
    }
}

static bool sel_has(const Selection* s, SelKind kind, int index) {
    const bool* bits = sel_pool_const(s, kind);
    if (!bits || index < 0 || index >= sel_cap(kind)) return false;
    return bits[index];
}

static int sel_count_kind(const Selection* s, SelKind kind) {
    const bool* bits = sel_pool_const(s, kind);
    int cap = sel_cap(kind);
    int n = 0;
    for (int i = 0; i < cap; i++) {
        if (bits[i]) n++;
    }
    return n;
}

static int sel_count(const Selection* s) {
    int n = 0;
    for (int k = SEL_WALL; k < SEL_KIND_COUNT; k++) {
        n += sel_count_kind(s, (SelKind)k);
    }
    return n;
}

static void sel_clear(Editor* e) {
    memset(&e->selection, 0, sizeof e->selection);
    e->selection.primary.kind = SEL_NONE;
    e->selection.primary.index = -1;
    e->label_buf_ready = false;
}

static void sel_promote(Editor* e) {
    Selection* s = &e->selection;
    for (int k = SEL_WALL; k < SEL_KIND_COUNT; k++) {
        SelKind kind = (SelKind)k;
        const bool* bits = sel_pool_const(s, kind);
        int cap = sel_cap(kind);
        for (int i = 0; i < cap; i++) {
            if (!bits[i]) continue;
            if (s->primary.kind != kind || s->primary.index != i) {
                s->primary = (SelRef){ kind, i };
                e->label_buf_ready = false;
            }
            return;
        }
    }
    if (s->primary.kind != SEL_NONE || s->primary.index != -1) {
        s->primary = (SelRef){ SEL_NONE, -1 };
        e->label_buf_ready = false;
    }
}

static bool sel_valid(const Editor* e, SelRef ref) {
    switch (ref.kind) {
        case SEL_WALL:
            return ref.index >= 0 && ref.index < e->level.wall_count;
        case SEL_ROOM:
            return ref.index >= 0 && ref.index < e->level.room_count;
        case SEL_DOOR:
            return ref.index >= 0 && ref.index < e->level.door_count &&
                   level_wall_by_id_const(&e->level, e->level.doors[ref.index].wall) != NULL;
        case SEL_LABEL:
            return ref.index >= 0 && ref.index < e->level.label_count;
        case SEL_STAIRS:
            return ref.index >= 0 && ref.index < e->level.stairs_count;
        case SEL_NONE:
        default:
            return false;
    }
}

static void sel_prune(Editor* e) {
    Selection* s = &e->selection;
    bool changed = false;
    for (int k = SEL_WALL; k < SEL_KIND_COUNT; k++) {
        SelKind kind = (SelKind)k;
        bool* bits = sel_pool(s, kind);
        int cap = sel_cap(kind);
        for (int i = 0; i < cap; i++) {
            if (!bits[i]) continue;
            if (!sel_valid(e, (SelRef){ kind, i })) {
                bits[i] = false;
                changed = true;
            }
        }
    }
    if (sel_count(s) == 0) {
        if (s->primary.kind != SEL_NONE || s->primary.index != -1) {
            s->primary = (SelRef){ SEL_NONE, -1 };
            changed = true;
        }
    } else if (!sel_valid(e, s->primary) || !sel_has(s, s->primary.kind, s->primary.index)) {
        sel_promote(e);
        return;
    }
    if (changed) e->label_buf_ready = false;
}

static bool sel_single(const Selection* s, SelRef* out) {
    if (sel_count(s) != 1) return false;
    for (int k = SEL_WALL; k < SEL_KIND_COUNT; k++) {
        SelKind kind = (SelKind)k;
        const bool* bits = sel_pool_const(s, kind);
        int cap = sel_cap(kind);
        for (int i = 0; i < cap; i++) {
            if (!bits[i]) continue;
            if (out) *out = (SelRef){ kind, i };
            return true;
        }
    }
    return false;
}

static void sel_set_primary(Editor* e, SelKind kind, int index) {
    if (!sel_has(&e->selection, kind, index)) return;
    if (e->selection.primary.kind == kind && e->selection.primary.index == index) return;
    e->selection.primary = (SelRef){ kind, index };
    e->label_buf_ready = false;
}

static bool sel_add(Editor* e, SelKind kind, int index) {
    bool* bits = sel_pool(&e->selection, kind);
    if (!bits || index < 0 || index >= sel_cap(kind) || bits[index]) return false;
    bits[index] = true;
    if (!sel_has(&e->selection, e->selection.primary.kind, e->selection.primary.index)) {
        e->selection.primary = (SelRef){ kind, index };
    }
    e->label_buf_ready = false;
    return true;
}

static bool sel_remove(Editor* e, SelKind kind, int index) {
    bool* bits = sel_pool(&e->selection, kind);
    if (!bits || index < 0 || index >= sel_cap(kind) || !bits[index]) return false;
    bits[index] = false;
    e->label_buf_ready = false;
    if (e->selection.primary.kind == kind && e->selection.primary.index == index) {
        sel_promote(e);
    }
    return true;
}

static void sel_only(Editor* e, SelKind kind, int index) {
    sel_clear(e);
    bool* bits = sel_pool(&e->selection, kind);
    if (!bits || index < 0 || index >= sel_cap(kind)) return;
    bits[index] = true;
    e->selection.primary = (SelRef){ kind, index };
}

static void sel_toggle(Editor* e, SelKind kind, int index) {
    if (sel_has(&e->selection, kind, index)) {
        sel_remove(e, kind, index);
        return;
    }
    if (sel_add(e, kind, index)) {
        sel_set_primary(e, kind, index);
    }
}

static void switch_tool(Editor* e, Tool tool) {
    if (e->tool == tool) return;
    if (gesture_is_drag(e) && e->gesture.kind != GESTURE_MARQUEE) {
        history_restore_current(&e->history, &e->level);
        sel_prune(e);
    }
    cancel_gesture(e);
    e->tool = tool;
    set_status(e, false, "%s tool", TOOL_NAMES[tool]);
}

static void fit_camera(Editor* e) {
    float cw = e->canvas.max.x - e->canvas.min.x;
    float ch = e->canvas.max.y - e->canvas.min.y;
    if (cw < 40.0f) cw = 40.0f;
    if (ch < 40.0f) ch = 40.0f;

    if (level_is_empty(&e->level)) {
        e->camera.center = v2(0.0f, 0.0f);
        e->camera.zoom = 1.0f;
        return;
    }

    Rect b = level_bounds(&e->level);
    float w = b.max.x - b.min.x;
    float h = b.max.y - b.min.y;
    if (w < 1.0f) w = 1.0f;
    if (h < 1.0f) h = 1.0f;
    float zx = (cw - FIT_MARGIN * 2.0f) / w;
    float zy = (ch - FIT_MARGIN * 2.0f) / h;
    float z = zx < zy ? zx : zy;
    e->camera.zoom = clampf(z, 0.15f, 6.0f);
    e->camera.center = v2((b.min.x + b.max.x) * 0.5f, (b.min.y + b.max.y) * 0.5f);
}

static Rect label_world_rect(const Editor* e, const Label* label) {
    ImVec2 sz = ImFont_CalcTextSizeA(igGetFont(), label->size * e->camera.zoom,
                                     FLT_MAX, 0.0f, label->text);
    float z = e->camera.zoom > 0.0001f ? e->camera.zoom : 1.0f;
    return (Rect){
        label->pos,
        v2(label->pos.x + sz.x / z, label->pos.y + sz.y / z),
    };
}

static Hit hit_test(const Editor* e, Vec2 world) {
    Hit hit = { SEL_NONE, -1 };
    float tol = HIT_TOL_PX / e->camera.zoom;

    for (int i = e->level.door_count - 1; i >= 0; i--) {
        Vec2 a, b;
        level_door_span(&e->level, &e->level.doors[i], &a, &b);
        float d = point_segment_dist(world, a, b);
        if (d <= tol + DOOR_THICKNESS * 0.5f) {
            hit.kind = SEL_DOOR;
            hit.index = i;
            return hit;
        }
    }
    for (int i = e->level.label_count - 1; i >= 0; i--) {
        if (rect_contains(label_world_rect(e, &e->level.labels[i]), world)) {
            hit.kind = SEL_LABEL;
            hit.index = i;
            return hit;
        }
    }
    for (int i = e->level.stairs_count - 1; i >= 0; i--) {
        if (rect_contains(level_stairs_rect(&e->level.stairs[i]), world)) {
            hit.kind = SEL_STAIRS;
            hit.index = i;
            return hit;
        }
    }
    for (int i = e->level.wall_count - 1; i >= 0; i--) {
        const Wall* w = &e->level.walls[i];
        if (point_segment_dist(world, w->a, w->b) <= tol) {
            hit.kind = SEL_WALL;
            hit.index = i;
            return hit;
        }
    }
    int room = level_room_at(&e->level, world);
    if (room >= 0) {
        hit.kind = SEL_ROOM;
        hit.index = room;
    }
    return hit;
}

static void add_door_to_wall(Editor* e, WallId wall_id, float t, float width) {
    const Wall* w = level_wall_by_id_const(&e->level, wall_id);
    if (!w) {
        set_status(e, true, "Door needs a wall");
        return;
    }
    if (e->snap) t = snap_wall_t(w, t, e->grid_size);
    int idx = level_add_door(&e->level, wall_id, t, width, e->door_color);
    if (idx < 0) {
        set_status(e, true, "Could not add door: level is full or wall is invalid");
        return;
    }
    sel_only(e, SEL_DOOR, idx);
    commit(e, "Door added");
}

static void delete_selection(Editor* e) {
    sel_prune(e);
    if (sel_count(&e->selection) == 0) return;
    Selection* s = &e->selection;
    int deleted = 0;
    for (int i = e->level.door_count - 1; i >= 0; i--) {
        if (s->doors[i] && level_delete_door(&e->level, i)) deleted++;
    }
    for (int i = e->level.label_count - 1; i >= 0; i--) {
        if (s->labels[i] && level_delete_label(&e->level, i)) deleted++;
    }
    for (int i = e->level.stairs_count - 1; i >= 0; i--) {
        if (s->stairs[i] && level_delete_stairs(&e->level, i)) deleted++;
    }
    for (int i = e->level.room_count - 1; i >= 0; i--) {
        if (s->rooms[i] && level_delete_room(&e->level, i)) deleted++;
    }
    for (int i = e->level.wall_count - 1; i >= 0; i--) {
        if (s->walls[i] && level_delete_wall(&e->level, i)) deleted++;
    }
    sel_clear(e);
    cancel_gesture(e);
    if (deleted > 0) {
        char message[64];
        snprintf(message, sizeof message, "Deleted %d object%s", deleted,
                 deleted == 1 ? "" : "s");
        commit(e, message);
    } else {
        set_status(e, true, "Could not delete the selection");
    }
}

typedef struct {
    int walls, rooms, doors, labels, stairs;
} LevelCounts;

static LevelCounts level_counts(const Level* level) {
    return (LevelCounts){
        level->wall_count,
        level->room_count,
        level->door_count,
        level->label_count,
        level->stairs_count,
    };
}

static bool counts_equal(LevelCounts a, LevelCounts b) {
    return a.walls == b.walls && a.rooms == b.rooms && a.doors == b.doors &&
           a.labels == b.labels && a.stairs == b.stairs;
}

static void sel_after_history(Editor* e, LevelCounts before) {
    if (!counts_equal(before, level_counts(&e->level))) sel_clear(e);
    else sel_prune(e);
}

static void do_undo(Editor* e) {
    if (gesture_is_drag(e)) { set_status(e, false, "Finish the drag before undoing"); return; }
    if (!history_can_undo(&e->history)) { set_status(e, false, "Nothing to undo"); return; }
    LevelCounts before = level_counts(&e->level);
    cancel_gesture(e);
    history_undo(&e->history, &e->level);
    sel_after_history(e, before);
    e->label_buf_ready = false;
    set_status(e, false, "Undo");
}

static void do_redo(Editor* e) {
    if (gesture_is_drag(e)) { set_status(e, false, "Finish the drag before redoing"); return; }
    if (!history_can_redo(&e->history)) { set_status(e, false, "Nothing to redo"); return; }
    LevelCounts before = level_counts(&e->level);
    cancel_gesture(e);
    history_redo(&e->history, &e->level);
    sel_after_history(e, before);
    e->label_buf_ready = false;
    set_status(e, false, "Redo");
}

static void new_level(Editor* e) {
    level_init(&e->level);
    history_reset(&e->history, &e->level);
    sel_clear(e);
    cancel_gesture(e);
    snprintf(e->current_path, sizeof e->current_path, "%s", "levels/untitled.lvl");
    e->fit_pending = true;
    set_status(e, false, "New level");
}

static bool save_level_as(Editor* e, const char* path) {
    LevelError err;
    if (!level_save(&e->level, path, &err)) {
        snprintf(e->modal_error, sizeof e->modal_error, "%s", err.message);
        set_status(e, true, "Save failed: %s", err.message);
        return false;
    }
    snprintf(e->current_path, sizeof e->current_path, "%s", path);
    set_status(e, false, "Saved %s", path);
    e->modal_error[0] = '\0';
    return true;
}

static bool open_level(Editor* e, const char* path) {
    LevelError err;
    if (!level_load(&e->level, path, &err)) {
        snprintf(e->modal_error, sizeof e->modal_error, "%s", err.message);
        set_status(e, true, "Open failed: %s", err.message);
        return false;
    }
    history_reset(&e->history, &e->level);
    sel_clear(e);
    cancel_gesture(e);
    snprintf(e->current_path, sizeof e->current_path, "%s", path);
    e->fit_pending = true;
    e->modal_error[0] = '\0';
    e->label_buf_ready = false;
    set_status(e, false, "Opened %s", path);
    return true;
}

static void apply_theme(void) {
    ImGuiStyle* s = igGetStyle();
    s->WindowRounding = 3.0f;
    s->ChildRounding = 3.0f;
    s->FrameRounding = 3.0f;
    s->PopupRounding = 3.0f;
    s->ScrollbarRounding = 3.0f;
    s->GrabRounding = 3.0f;
    s->TabRounding = 3.0f;
    s->WindowPadding = (ImVec2){ 12.0f, 12.0f };
    s->FramePadding = (ImVec2){ 8.0f, 4.0f };
    s->ItemSpacing = (ImVec2){ 8.0f, 4.0f };
    s->ItemInnerSpacing = (ImVec2){ 6.0f, 4.0f };
    s->WindowBorderSize = 1.0f;
    s->FrameBorderSize = 0.0f;
    s->PopupBorderSize = 1.0f;
    s->ScrollbarSize = 10.0f;
    s->GrabMinSize = 12.0f;

    s->Colors[ImGuiCol_Text] = v4c(TEXT_COL, 1.0f);
    s->Colors[ImGuiCol_TextDisabled] = v4c(0x6f757c, 1.0f);
    s->Colors[ImGuiCol_WindowBg] = v4c(WINDOW_BG, 1.0f);
    s->Colors[ImGuiCol_ChildBg] = v4c(WINDOW_BG, 1.0f);
    s->Colors[ImGuiCol_PopupBg] = v4c(PANEL_BG, 1.0f);
    s->Colors[ImGuiCol_Border] = v4c(BORDER_COL, 1.0f);
    s->Colors[ImGuiCol_BorderShadow] = v4c(0x000000, 0.0f);
    s->Colors[ImGuiCol_FrameBg] = v4c(0x20242a, 1.0f);
    s->Colors[ImGuiCol_FrameBgHovered] = v4c(0x24282e, 1.0f);
    s->Colors[ImGuiCol_FrameBgActive] = v4c(0x2b3037, 1.0f);
    s->Colors[ImGuiCol_TitleBg] = v4c(WINDOW_BG, 1.0f);
    s->Colors[ImGuiCol_TitleBgActive] = v4c(PANEL_BG, 1.0f);
    s->Colors[ImGuiCol_TitleBgCollapsed] = v4c(WINDOW_BG, 1.0f);
    s->Colors[ImGuiCol_MenuBarBg] = v4c(PANEL_BG, 1.0f);
    s->Colors[ImGuiCol_ScrollbarBg] = v4c(WINDOW_BG, 1.0f);
    s->Colors[ImGuiCol_ScrollbarGrab] = v4c(BORDER_COL, 1.0f);
    s->Colors[ImGuiCol_ScrollbarGrabHovered] = v4c(0x3a4048, 1.0f);
    s->Colors[ImGuiCol_ScrollbarGrabActive] = v4c(ACCENT_UI, 1.0f);
    s->Colors[ImGuiCol_CheckMark] = v4c(ACCENT_UI, 1.0f);
    s->Colors[ImGuiCol_SliderGrab] = v4c(ACCENT_UI, 1.0f);
    s->Colors[ImGuiCol_SliderGrabActive] = v4c(ACCENT_COL, 1.0f);
    s->Colors[ImGuiCol_Button] = v4c(FRAME_BG, 1.0f);
    s->Colors[ImGuiCol_ButtonHovered] = v4c(0x24282e, 1.0f);
    s->Colors[ImGuiCol_ButtonActive] = v4c(0x2b3037, 1.0f);
    s->Colors[ImGuiCol_Header] = v4c(0x23272d, 1.0f);
    s->Colors[ImGuiCol_HeaderHovered] = v4c(0x2a3038, 1.0f);
    s->Colors[ImGuiCol_HeaderActive] = v4c(0x34424d, 1.0f);
    s->Colors[ImGuiCol_Separator] = v4c(BORDER_COL, 1.0f);
    s->Colors[ImGuiCol_SeparatorHovered] = v4c(ACCENT_UI, 1.0f);
    s->Colors[ImGuiCol_SeparatorActive] = v4c(ACCENT_COL, 1.0f);
    s->Colors[ImGuiCol_ResizeGrip] = v4c(BORDER_COL, 1.0f);
    s->Colors[ImGuiCol_ResizeGripHovered] = v4c(ACCENT_UI, 1.0f);
    s->Colors[ImGuiCol_ResizeGripActive] = v4c(ACCENT_COL, 1.0f);
    s->Colors[ImGuiCol_TextSelectedBg] = v4c(ACCENT_UI, 0.55f);
    s->Colors[ImGuiCol_NavCursor] = v4c(ACCENT_COL, 0.90f);
    s->Colors[ImGuiCol_ModalWindowDimBg] = v4c(0x000000, 0.62f);
}

static void heading(const char* text) {
    igDummy((ImVec2){ 0.0f, 2.0f });
    igTextColored(v4c(ACCENT_COL, 1.0f), "%s", text);
    igSeparator();
}

static void small_note(const char* text) {
    igPushFontFloat(igGetFont(), 10.0f);
    igPushStyleColorImVec4(ImGuiCol_Text, v4c(MUTED_COL, 1.0f));
    igPushTextWrapPos(0.0f);
    igTextUnformatted(text);
    igPopTextWrapPos();
    igPopStyleColorEx(1);
    igPopFont();
}

static bool swatch_button(const char* id, const char* label, ImU32 fill, bool selected,
                          float w) {
    float h = 22.0f;
    ImVec2 pos = igGetCursorScreenPos();
    bool pressed = igInvisibleButton(id, (ImVec2){ w, h }, 0);
    bool hovered = igIsItemHovered(0);
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 p1 = { pos.x + w, pos.y + h };
    ImDrawList_AddRectFilledEx(dl, pos, p1, fill, 3.0f, 0);
    ImU32 border = selected ? col32(ACCENT_COL) : (hovered ? col32(0x3a4048) : col32(BORDER_COL));
    ImDrawList_AddRectEx(dl, pos, p1, border, 3.0f, selected ? 2.0f : 1.0f, 0);
    ImU32 text = (fill == col32(DOOR_WHITE_COL)) ? col32(0x121417) : col32(0xf0f1f3);
    ImVec2 ts = ImFont_CalcTextSizeA(igGetFont(), 12.0f, FLT_MAX, 0.0f, label);
    ImDrawList_AddTextImFontPtrEx(dl, igGetFont(), 12.0f,
                                  (ImVec2){ pos.x + (w - ts.x) * 0.5f,
                                            pos.y + (h - ts.y) * 0.5f },
                                  text, label, NULL, 0.0f, NULL);
    return pressed;
}

static bool tool_button(Editor* e, Tool tool) {
    bool active = e->tool == tool;
    char label[48];
    snprintf(label, sizeof label, "%d  %s", (int)tool + 1, TOOL_NAMES[tool]);
    igPushStyleColorImVec4(ImGuiCol_Button, active ? v4c(0x2b3f50, 1.0f) : v4c(FRAME_BG, 1.0f));
    igPushStyleColorImVec4(ImGuiCol_ButtonHovered, active ? v4c(0x34506a, 1.0f) : v4c(0x24282e, 1.0f));
    igPushStyleColorImVec4(ImGuiCol_ButtonActive, active ? v4c(0x3b5c78, 1.0f) : v4c(0x2b3037, 1.0f));
    bool pressed = igButtonEx(label, (ImVec2){ igGetContentRegionAvail().x, 22.0f });
    igPopStyleColorEx(3);
    if (active) {
        ImVec2 p = igGetItemRectMin();
        ImVec2 q = igGetItemRectMax();
        ImDrawList_AddRectEx(igGetWindowDrawList(), p, q, col32(ACCENT_COL), 3.0f, 1.0f, 0);
    }
    return pressed;
}

static void draw_dashed_line(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col,
                             float thickness, float dash, float gap) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.001f) return;
    float ux = dx / len;
    float uy = dy / len;
    float at = 0.0f;
    while (at < len) {
        float end = at + dash;
        if (end > len) end = len;
        ImVec2 p0 = { a.x + ux * at, a.y + uy * at };
        ImVec2 p1 = { a.x + ux * end, a.y + uy * end };
        ImDrawList_AddLineEx(dl, p0, p1, col, thickness, 0);
        at += dash + gap;
    }
}

static void draw_grid(Editor* e) {
    if (!e->show_grid) return;
    ImDrawList* dl = igGetWindowDrawList();
    Vec2 w0 = screen_to_world(e, (ImVec2){ e->canvas.min.x, e->canvas.min.y });
    Vec2 w1 = screen_to_world(e, (ImVec2){ e->canvas.max.x, e->canvas.max.y });
    float minx = w0.x < w1.x ? w0.x : w1.x;
    float maxx = w0.x > w1.x ? w0.x : w1.x;
    float miny = w0.y < w1.y ? w0.y : w1.y;
    float maxy = w0.y > w1.y ? w0.y : w1.y;
    const float step = 32.0f;
    int i0 = (int)floorf(minx / step);
    int i1 = (int)ceilf(maxx / step);
    int j0 = (int)floorf(miny / step);
    int j1 = (int)ceilf(maxy / step);
    if (i1 - i0 > 2000 || j1 - j0 > 2000) return;
    for (int i = i0; i <= i1; i++) {
        float x = i * step;
        ImVec2 a = world_to_screen(e, v2(x, miny));
        ImU32 col = (i % 4 == 0) ? col32(GRID_BRIGHT) : col32(GRID_COL);
        ImDrawList_AddLineVEx(dl, a.x, e->canvas.min.y, e->canvas.max.y, col, 1.0f, 0);
    }
    for (int j = j0; j <= j1; j++) {
        float y = j * step;
        ImVec2 a = world_to_screen(e, v2(minx, y));
        ImU32 col = (j % 4 == 0) ? col32(GRID_BRIGHT) : col32(GRID_COL);
        ImDrawList_AddLineHEx(dl, e->canvas.min.x, e->canvas.max.x, a.y, col, 1.0f, 0);
    }
}

static void draw_rooms(const Editor* e) {
    ImDrawList* dl = igGetWindowDrawList();
    for (int i = 0; i < e->level.room_count; i++) {
        const Room* room = &e->level.rooms[i];
        if (room->count < 3) continue;
        ImVec2 pts[LEVEL_MAX_ROOM_PTS];
        for (int j = 0; j < room->count; j++) pts[j] = world_to_screen(e, room->pts[j]);
        ImDrawList_AddConcavePolyFilled(dl, pts, room->count, room_color(room->color));
    }
}

static void draw_stairs(const Editor* e) {
    ImDrawList* dl = igGetWindowDrawList();
    for (int i = 0; i < e->level.stairs_count; i++) {
        const Stairs* s = &e->level.stairs[i];
        Rect r = level_stairs_rect(s);
        ImVec2 a = world_to_screen(e, r.min);
        ImVec2 b = world_to_screen(e, r.max);
        ImDrawList_AddRectFilledEx(dl, a, b, col32(STAIR_FILL), 0.0f, 0);
        ImDrawList_AddRectEx(dl, a, b, col32a(0x3a4048, 190), 0.0f, 1.0f, 0);
        float along = (float)s->steps * STAIR_STEP_DEPTH;
        if (s->dir == STAIRS_H) {
            float x0 = s->pos.x - along * 0.5f;
            for (int k = 0; k <= s->steps; k++) {
                float x = x0 + (float)k * STAIR_STEP_DEPTH;
                ImVec2 p = world_to_screen(e, v2(x, r.min.y));
                ImDrawList_AddLineVEx(dl, p.x, a.y, b.y, col32(STAIR_COL), 1.0f, 0);
            }
        } else {
            float y0 = s->pos.y - along * 0.5f;
            for (int k = 0; k <= s->steps; k++) {
                float y = y0 + (float)k * STAIR_STEP_DEPTH;
                ImVec2 p = world_to_screen(e, v2(r.min.x, y));
                ImDrawList_AddLineHEx(dl, a.x, b.x, p.y, col32(STAIR_COL), 1.0f, 0);
            }
        }
    }
}

static void draw_door_bar(ImDrawList* dl, const Editor* e, const Wall* wall,
                          Vec2 a, Vec2 b, ImU32 col, float extra) {
    ImVec2 pa = world_to_screen(e, a);
    ImVec2 pb = world_to_screen(e, b);
    float th = DOOR_THICKNESS * e->camera.zoom;
    if (th < 2.0f) th = 2.0f;
    th += extra;
    if (wall_is_vertical(wall)) {
        float x = (pa.x + pb.x) * 0.5f;
        float y0 = pa.y < pb.y ? pa.y : pb.y;
        float y1 = pa.y > pb.y ? pa.y : pb.y;
        ImDrawList_AddRectFilledEx(dl, (ImVec2){ x - th * 0.5f, y0 },
                                   (ImVec2){ x + th * 0.5f, y1 }, col, 0.0f, 0);
    } else {
        float y = (pa.y + pb.y) * 0.5f;
        float x0 = pa.x < pb.x ? pa.x : pb.x;
        float x1 = pa.x > pb.x ? pa.x : pb.x;
        ImDrawList_AddRectFilledEx(dl, (ImVec2){ x0, y - th * 0.5f },
                                   (ImVec2){ x1, y + th * 0.5f }, col, 0.0f, 0);
    }
}

static int wall_door_spans(const Editor* e, const Wall* wall, Span* spans) {
    float len = wall_length(wall);
    if (len <= 0.0001f) return 0;
    int n = 0;
    for (int i = 0; i < e->level.door_count; i++) {
        const Door* d = &e->level.doors[i];
        if (d->wall != wall->id) continue;
        float half = d->width / (2.0f * len);
        float a = d->t - half;
        float b = d->t + half;
        if (a < 0.0f) a = 0.0f;
        if (b > 1.0f) b = 1.0f;
        if (b > a) {
            spans[n].a = a;
            spans[n].b = b;
            n++;
        }
        if (n >= LEVEL_MAX_DOORS) break;
    }
    for (int i = 1; i < n; i++) {
        Span key = spans[i];
        int j = i - 1;
        while (j >= 0 && spans[j].a > key.a) {
            spans[j + 1] = spans[j];
            j--;
        }
        spans[j + 1] = key;
    }
    return n;
}

static void draw_wall_segment(const Editor* e, ImDrawList* dl, const Wall* wall,
                              float t0, float t1, ImU32 col, float thickness) {
    if (t1 - t0 <= 0.00001f) return;
    Vec2 a = segment_point(wall->a, wall->b, t0);
    Vec2 b = segment_point(wall->a, wall->b, t1);
    ImDrawList_AddLineEx(dl, world_to_screen(e, a), world_to_screen(e, b), col,
                         thickness, 0);
}

static void draw_walls(const Editor* e) {
    ImDrawList* dl = igGetWindowDrawList();
    float wall_px = 2.0f;
    for (int i = 0; i < e->level.wall_count; i++) {
        const Wall* w = &e->level.walls[i];
        Span spans[LEVEL_MAX_DOORS];
        int n = wall_door_spans(e, w, spans);
        float cursor = 0.0f;
        for (int k = 0; k < n; k++) {
            if (spans[k].a > cursor) {
                draw_wall_segment(e, dl, w, cursor, spans[k].a, col32(WALL_COL), wall_px);
            }
            if (spans[k].b > cursor) cursor = spans[k].b;
        }
        if (cursor < 1.0f) {
            draw_wall_segment(e, dl, w, cursor, 1.0f, col32(WALL_COL), wall_px);
        }
    }
}

static void draw_doors(const Editor* e) {
    ImDrawList* dl = igGetWindowDrawList();
    for (int i = 0; i < e->level.door_count; i++) {
        const Door* d = &e->level.doors[i];
        const Wall* w = level_wall_by_id_const(&e->level, d->wall);
        if (!w) continue;
        Vec2 a, b;
        level_door_span(&e->level, d, &a, &b);
        draw_door_bar(dl, e, w, a, b, door_color(d->color), 0.0f);
    }
}

static void draw_labels(const Editor* e) {
    ImDrawList* dl = igGetWindowDrawList();
    for (int i = 0; i < e->level.label_count; i++) {
        const Label* l = &e->level.labels[i];
        if (!l->text[0]) continue;
        ImVec2 p = world_to_screen(e, l->pos);
        ImDrawList_AddTextImFontPtrEx(dl, igGetFont(), l->size * e->camera.zoom, p,
                                      col32(LABEL_COL), l->text, NULL, 0.0f, NULL);
    }
}

static void draw_selection_handles(ImDrawList* dl, ImVec2 pmin, ImVec2 pmax) {
    ImU32 col = col32(SELECT_COL);
    ImDrawList_AddRectEx(dl, pmin, pmax, col, 0.0f, 2.0f, 0);
    const float h = 4.0f;
    ImVec2 corners[4] = {
        pmin,
        (ImVec2){ pmax.x, pmin.y },
        pmax,
        (ImVec2){ pmin.x, pmax.y },
    };
    for (int i = 0; i < 4; i++) {
        ImDrawList_AddRectFilledEx(dl, (ImVec2){ corners[i].x - h, corners[i].y - h },
                                   (ImVec2){ corners[i].x + h, corners[i].y + h },
                                   col, 0.0f, 0);
    }
}

static void draw_selection_object(const Editor* e, SelKind kind, int index, bool primary) {
    ImDrawList* dl = igGetWindowDrawList();
    ImU32 col = col32(SELECT_COL);
    switch (kind) {
        case SEL_WALL: {
            const Wall* w = &e->level.walls[index];
            ImVec2 a = world_to_screen(e, w->a);
            ImVec2 b = world_to_screen(e, w->b);
            ImDrawList_AddLineEx(dl, a, b, col, 3.0f, 0);
            if (primary) {
                ImDrawList_AddCircleFilled(dl, a, 3.5f, col, 12);
                ImDrawList_AddCircleFilled(dl, b, 3.5f, col, 12);
            }
            break;
        }
        case SEL_ROOM: {
            const Room* room = &e->level.rooms[index];
            ImVec2 pts[LEVEL_MAX_ROOM_PTS + 1];
            for (int i = 0; i < room->count; i++) pts[i] = world_to_screen(e, room->pts[i]);
            pts[room->count] = pts[0];
            ImDrawList_AddPolyline(dl, pts, room->count + 1, col, 2.0f, 0);
            if (primary) {
                ImVec2 pmin = pts[0], pmax = pts[0];
                for (int i = 1; i < room->count; i++) {
                    if (pts[i].x < pmin.x) pmin.x = pts[i].x;
                    if (pts[i].y < pmin.y) pmin.y = pts[i].y;
                    if (pts[i].x > pmax.x) pmax.x = pts[i].x;
                    if (pts[i].y > pmax.y) pmax.y = pts[i].y;
                }
                draw_selection_handles(dl, pmin, pmax);
            }
            break;
        }
        case SEL_DOOR: {
            const Door* d = &e->level.doors[index];
            const Wall* w = level_wall_by_id_const(&e->level, d->wall);
            if (!w) break;
            Vec2 a, b;
            level_door_span(&e->level, d, &a, &b);
            ImVec2 pa = world_to_screen(e, a);
            ImVec2 pb = world_to_screen(e, b);
            float pad = 3.0f;
            ImVec2 pmin = { (pa.x < pb.x ? pa.x : pb.x) - pad,
                            (pa.y < pb.y ? pa.y : pb.y) - pad };
            ImVec2 pmax = { (pa.x > pb.x ? pa.x : pb.x) + pad,
                            (pa.y > pb.y ? pa.y : pb.y) + pad };
            if (wall_is_vertical(w)) {
                float x = (pa.x + pb.x) * 0.5f;
                pmin = (ImVec2){ x - pad - 2.0f, pmin.y };
                pmax = (ImVec2){ x + pad + 2.0f, pmax.y };
            } else {
                float y = (pa.y + pb.y) * 0.5f;
                pmin = (ImVec2){ pmin.x, y - pad - 2.0f };
                pmax = (ImVec2){ pmax.x, y + pad + 2.0f };
            }
            if (primary) draw_selection_handles(dl, pmin, pmax);
            else ImDrawList_AddRectEx(dl, pmin, pmax, col, 0.0f, 2.0f, 0);
            break;
        }
        case SEL_LABEL: {
            Rect r = label_world_rect(e, &e->level.labels[index]);
            ImVec2 pmin = world_to_screen(e, r.min);
            ImVec2 pmax = world_to_screen(e, r.max);
            pmin.x -= 3.0f;
            pmin.y -= 3.0f;
            pmax.x += 3.0f;
            pmax.y += 3.0f;
            if (primary) draw_selection_handles(dl, pmin, pmax);
            else ImDrawList_AddRectEx(dl, pmin, pmax, col, 0.0f, 2.0f, 0);
            break;
        }
        case SEL_STAIRS: {
            Rect r = level_stairs_rect(&e->level.stairs[index]);
            ImVec2 pmin = world_to_screen(e, r.min);
            ImVec2 pmax = world_to_screen(e, r.max);
            pmin.x -= 3.0f;
            pmin.y -= 3.0f;
            pmax.x += 3.0f;
            pmax.y += 3.0f;
            if (primary) draw_selection_handles(dl, pmin, pmax);
            else ImDrawList_AddRectEx(dl, pmin, pmax, col, 0.0f, 2.0f, 0);
            break;
        }
        case SEL_NONE:
        default: break;
    }
}

static void draw_marquee(const Editor* e) {
    if (e->gesture.kind != GESTURE_MARQUEE || !e->gesture.moved) return;
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 a = world_to_screen(e, e->gesture.press_world);
    ImVec2 b = world_to_screen(e, e->gesture.cur_world);
    ImVec2 pmin = { fminf(a.x, b.x), fminf(a.y, b.y) };
    ImVec2 pmax = { fmaxf(a.x, b.x), fmaxf(a.y, b.y) };
    ImDrawList_AddRectFilledEx(dl, pmin, pmax, col32a(ACCENT_COL, 36), 0.0f, 0);
    ImDrawList_AddRectEx(dl, pmin, pmax, col32a(ACCENT_COL, 220), 0.0f, 1.5f, 0);
}

static void draw_selection(Editor* e) {
    sel_prune(e);
    for (int k = SEL_WALL; k < SEL_KIND_COUNT; k++) {
        SelKind kind = (SelKind)k;
        int cap = sel_cap(kind);
        for (int i = 0; i < cap; i++) {
            if (!sel_has(&e->selection, kind, i)) continue;
            if (e->selection.primary.kind == kind && e->selection.primary.index == i) continue;
            draw_selection_object(e, kind, i, false);
        }
    }
    SelRef primary = e->selection.primary;
    if (sel_has(&e->selection, primary.kind, primary.index)) {
        draw_selection_object(e, primary.kind, primary.index, true);
    }
}

static void draw_chain_preview(Editor* e, ImVec2 mouse) {
    ImDrawList* dl = igGetWindowDrawList();
    Gesture* g = &e->gesture;
    if (g->point_count <= 0) return;
    Vec2 cursor = screen_to_world(e, mouse);
    Vec2 last_world = g->pts[g->point_count - 1];
    Vec2 first_world = g->pts[0];
    Vec2 next = constrained(e, last_world, cursor);
    ImVec2 last = world_to_screen(e, last_world);
    ImVec2 first = world_to_screen(e, first_world);
    bool near = screen_dist(mouse, first) <= CLOSE_TOL_PX && g->point_count >= 3;
    bool aligned = fabsf(last_world.x - first_world.x) <= 0.001f ||
                   fabsf(last_world.y - first_world.y) <= 0.001f;
    if (near && !aligned && g->point_count >= 2) {
        Vec2 prev = g->pts[g->point_count - 2];
        bool last_horizontal = fabsf(last_world.y - prev.y) <= 0.001f;
        Vec2 corner = last_horizontal ? v2(last_world.x, first_world.y)
                                      : v2(first_world.x, last_world.y);
        ImVec2 c = world_to_screen(e, corner);
        draw_dashed_line(dl, last, c, col32a(ACCENT_COL, 190), 1.5f, 6.0f, 4.0f);
        draw_dashed_line(dl, c, first, col32a(ACCENT_COL, 190), 1.5f, 6.0f, 4.0f);
    } else {
        ImVec2 np = world_to_screen(e, next);
        draw_dashed_line(dl, last, np, col32a(ACCENT_COL, 190), 1.5f, 6.0f, 4.0f);
    }

    for (int i = 0; i < g->point_count; i++) {
        ImVec2 p = world_to_screen(e, g->pts[i]);
        ImDrawList_AddCircleFilled(dl, p, 2.6f, col32(ACCENT_COL), 10);
    }
    ImDrawList_AddCircleEx(dl, first, near ? 9.0f : 6.0f,
                           near ? col32(ACCENT_COL) : col32a(ACCENT_COL, 150),
                           16, near ? 2.5f : 1.5f, 0);
    if (near) {
        ImDrawList_AddTextImFontPtrEx(dl, igGetFont(), 11.0f,
                                      (ImVec2){ first.x + 12.0f, first.y - 7.0f },
                                      col32(ACCENT_COL), "close", NULL, 0.0f, NULL);
    }
}

static void draw_polygon_preview(Editor* e, ImVec2 mouse) {
    ImDrawList* dl = igGetWindowDrawList();
    Gesture* g = &e->gesture;
    if (g->point_count <= 0) return;
    Vec2 cursor = screen_to_world(e, mouse);
    Vec2 next = snapped(e, cursor);
    ImVec2 pts[LEVEL_MAX_ROOM_PTS + 1];
    for (int i = 0; i < g->point_count; i++) pts[i] = world_to_screen(e, g->pts[i]);
    if (g->point_count >= 3) {
        ImDrawList_AddConcavePolyFilled(dl, pts, g->point_count,
                                         col32a(SELECT_COL, 36));
    }
    ImVec2 np = world_to_screen(e, next);
    ImDrawList_AddLineEx(dl, pts[g->point_count - 1], np,
                         col32a(ACCENT_COL, 180), 1.5f, 0);
    ImDrawList_AddLineEx(dl, np, pts[0], col32a(ACCENT_COL, 90), 1.0f, 0);
    for (int i = 0; i < g->point_count; i++) {
        ImDrawList_AddCircleFilled(dl, pts[i], 2.6f, col32(ACCENT_COL), 10);
    }
    ImVec2 first = pts[0];
    bool near = screen_dist(mouse, first) <= CLOSE_TOL_PX && g->point_count >= 3;
    ImDrawList_AddCircleEx(dl, first, near ? 9.0f : 6.0f,
                           near ? col32(ACCENT_COL) : col32a(ACCENT_COL, 150),
                           16, near ? 2.5f : 1.5f, 0);
    if (near) {
        ImDrawList_AddTextImFontPtrEx(dl, igGetFont(), 11.0f,
                                      (ImVec2){ first.x + 12.0f, first.y - 7.0f },
                                      col32(ACCENT_COL), "close", NULL, 0.0f, NULL);
    }
}

static void draw_door_preview(Editor* e, int hover_wall, float hover_t) {
    if (e->tool != TOOL_DOOR) return;
    if (hover_wall < 0) return;
    ImDrawList* dl = igGetWindowDrawList();
    const Wall* w = &e->level.walls[hover_wall];
    float len = wall_length(w);
    if (len <= 0.0001f) return;
    float t = e->snap ? snap_wall_t(w, hover_t, e->grid_size) : hover_t;
    float width = fminf(44.0f, len);
    float half = width / (2.0f * len);
    Vec2 a = segment_point(w->a, w->b, clampf(t - half, 0.0f, 1.0f));
    Vec2 b = segment_point(w->a, w->b, clampf(t + half, 0.0f, 1.0f));
    draw_door_bar(dl, e, w, a, b, col32a(SELECT_COL, 130), 2.0f);
}

static void draw_status_strip(Editor* e) {
    ImDrawList* dl = igGetWindowDrawList();
    float y0 = e->canvas.max.y - STATUS_H;
    ImVec2 pmin = { e->canvas.min.x, y0 };
    ImVec2 pmax = { e->canvas.max.x, e->canvas.max.y };
    ImDrawList_AddRectFilledEx(dl, pmin, pmax, col32a(WINDOW_BG, 244), 0.0f, 0);
    ImDrawList_AddLineEx(dl, pmin, (ImVec2){ pmax.x, y0 }, col32(BORDER_COL), 1.0f, 0);

    ImFont* font = igGetFont();
    const float size = 12.0f;
    float ty = y0 + (STATUS_H - ImFont_CalcTextSizeA(font, size, FLT_MAX, 0.0f, "Xg").y) * 0.5f;
    float x = pmin.x + 12.0f;

    char right[160];
    if (e->cursor_valid) {
        snprintf(right, sizeof right, "x %.1f   y %.1f      %.0f%%",
                 e->cursor_world.x, e->cursor_world.y, e->camera.zoom * 100.0f);
    } else {
        snprintf(right, sizeof right, "x --   y --      %.0f%%", e->camera.zoom * 100.0f);
    }
    ImVec2 rsz = ImFont_CalcTextSizeA(font, size, FLT_MAX, 0.0f, right);
    float right_x = pmax.x - 12.0f - rsz.x;
    ImDrawList_AddTextImFontPtrEx(dl, font, size,
                                  (ImVec2){ right_x, ty }, col32(0x8a9097), right,
                                  NULL, 0.0f, NULL);

    float clip_right = right_x - 14.0f;
    if (clip_right < pmin.x) clip_right = pmin.x;
    ImDrawList_PushClipRect(dl, pmin, (ImVec2){ clip_right, pmax.y }, true);
    const char* tool = TOOL_NAMES[e->tool];
    ImDrawList_AddTextImFontPtrEx(dl, font, size, (ImVec2){ x, ty },
                                  col32(SELECT_COL), tool, NULL, 0.0f, NULL);
    x += ImFont_CalcTextSizeA(font, size, FLT_MAX, 0.0f, tool).x + 2.0f;

    char rest[640];
    if (e->status_error && e->status[0]) {
        snprintf(rest, sizeof rest, "  %s", e->status);
    } else if (e->status[0]) {
        snprintf(rest, sizeof rest, "  %s   |   %s", e->status, TOOL_HINTS[e->tool]);
    } else {
        snprintf(rest, sizeof rest, "  %s", TOOL_HINTS[e->tool]);
    }
    ImDrawList_AddTextImFontPtrEx(dl, font, size, (ImVec2){ x, ty },
                                  col32(e->status_error ? ERROR_COL : MUTED_COL),
                                  rest, NULL, 0.0f, NULL);
    ImDrawList_PopClipRect(dl);
}

static int moved_objects(const Editor* e, int* left_behind) {
    const Selection* s = &e->selection;
    int moved = sel_count_kind(s, SEL_WALL) + sel_count_kind(s, SEL_ROOM) +
                sel_count_kind(s, SEL_LABEL) + sel_count_kind(s, SEL_STAIRS);
    int left = 0;
    for (int i = 0; i < e->level.door_count; i++) {
        int wall = level_find_wall_index(&e->level, e->level.doors[i].wall);
        if (wall >= 0 && sel_has(s, SEL_WALL, wall)) {
            moved++;
        } else if (sel_has(s, SEL_DOOR, i)) {
            left++;
        }
    }
    *left_behind = left;
    return moved;
}

static void finish_move(Editor* e) {
    if (e->gesture.moved) {
        int left = 0;
        int moved = moved_objects(e, &left);
        char message[256];
        if (left > 0) {
            snprintf(message, sizeof message,
                     "Moved %d object%s; %d selected door%s left behind "
                     "(host wall not selected)",
                     moved, moved == 1 ? "" : "s",
                     left, left == 1 ? "" : "s");
        } else {
            snprintf(message, sizeof message, "Moved %d object%s",
                     moved, moved == 1 ? "" : "s");
        }
        commit(e, message);
    }
    cancel_gesture(e);
}

static void begin_move(Editor* e, Vec2 world, Vec2 anchor) {
    e->gesture.kind = GESTURE_MOVE;
    e->gesture.press_world = world;
    e->gesture.anchor = anchor;
    e->gesture.applied = v2(0.0f, 0.0f);
    e->gesture.moved = false;
}

static void marquee_select(Editor* e, Rect rect, bool additive) {
    if (!additive) sel_clear(e);
    for (int i = 0; i < e->level.door_count; i++) {
        const Door* door = &e->level.doors[i];
        if (!level_wall_by_id_const(&e->level, door->wall)) continue;
        Vec2 a, b;
        level_door_span(&e->level, door, &a, &b);
        if (segment_intersects_rect(a, b, rect)) sel_add(e, SEL_DOOR, i);
    }
    for (int i = 0; i < e->level.label_count; i++) {
        if (rect_overlaps(label_world_rect(e, &e->level.labels[i]), rect)) {
            sel_add(e, SEL_LABEL, i);
        }
    }
    for (int i = 0; i < e->level.stairs_count; i++) {
        if (rect_overlaps(level_stairs_rect(&e->level.stairs[i]), rect)) {
            sel_add(e, SEL_STAIRS, i);
        }
    }
    for (int i = 0; i < e->level.wall_count; i++) {
        if (segment_intersects_rect(e->level.walls[i].a, e->level.walls[i].b, rect)) {
            sel_add(e, SEL_WALL, i);
        }
    }
    for (int i = 0; i < e->level.room_count; i++) {
        const Room* room = &e->level.rooms[i];
        if (polygon_intersects_rect(room->pts, room->count, rect)) {
            sel_add(e, SEL_ROOM, i);
        }
    }
    if (!sel_has(&e->selection, e->selection.primary.kind, e->selection.primary.index)) {
        sel_promote(e);
    }
    int n = sel_count(&e->selection);
    set_status(e, false, "%d object%s selected", n, n == 1 ? "" : "s");
}

static void update_drag(Editor* e) {
    ImGuiIO* io = igGetIO();
    Gesture* g = &e->gesture;
    if (!gesture_is_drag(e)) return;
    Vec2 world = screen_to_world(e, io->MousePos);
    bool down = io->MouseDown[0];

    switch (g->kind) {
        case GESTURE_MARQUEE: {
            g->cur_world = world;
            if (screen_dist(io->MousePos, v2_to_im(g->press_screen)) >= 4.0f) {
                g->moved = true;
            }
            if (down) break;
            if (g->moved) {
                marquee_select(e, rect_from_points(g->press_world, g->cur_world),
                               g->additive);
            } else {
                int n = sel_count(&e->selection);
                if (n == 0) set_status(e, false, "Selection cleared");
                else set_status(e, false, "%d object%s selected", n, n == 1 ? "" : "s");
            }
            cancel_gesture(e);
            break;
        }
        case GESTURE_MOVE: {
            if (down) {
                Vec2 raw = v2_sub(world, g->press_world);
                Vec2 delta = raw;
                if (e->snap) {
                    Vec2 goal = snap_grid(v2_add(g->anchor, raw), e->grid_size);
                    delta = v2_sub(goal, g->anchor);
                }
                Vec2 step = v2_sub(delta, g->applied);
                if (fabsf(step.x) > 0.0001f || fabsf(step.y) > 0.0001f) {
                    bool moved_any = false;
                    for (int i = 0; i < e->level.wall_count; i++) {
                        if (!sel_has(&e->selection, SEL_WALL, i)) continue;
                        if (level_move_wall(&e->level, i, step)) moved_any = true;
                    }
                    for (int i = 0; i < e->level.label_count; i++) {
                        if (!sel_has(&e->selection, SEL_LABEL, i)) continue;
                        if (level_move_label(&e->level, i,
                                             v2_add(e->level.labels[i].pos, step))) {
                            moved_any = true;
                        }
                    }
                    for (int i = 0; i < e->level.stairs_count; i++) {
                        if (!sel_has(&e->selection, SEL_STAIRS, i)) continue;
                        if (level_move_stairs(&e->level, i,
                                              v2_add(e->level.stairs[i].pos, step))) {
                            moved_any = true;
                        }
                    }
                    for (int i = 0; i < e->level.room_count; i++) {
                        if (!sel_has(&e->selection, SEL_ROOM, i)) continue;
                        if (level_move_room(&e->level, i, step)) moved_any = true;
                    }
                    if (moved_any) {
                        g->applied = delta;
                        g->moved = true;
                    }
                }
            }
            if (!down) finish_move(e);
            break;
        }
        case GESTURE_DOOR_SLIDE: {
            int idx = g->object_index;
            if (idx >= 0 && idx < e->level.door_count) {
                Door* d = &e->level.doors[idx];
                const Wall* w = level_wall_by_id_const(&e->level, d->wall);
                if (w) {
                    float t = point_segment_t(world, w->a, w->b);
                    if (e->snap) t = snap_wall_t(w, t, e->grid_size);
                    float old_t = d->t;
                    if (down) {
                        level_set_door_span(&e->level, idx, t, d->width);
                        if (fabsf(e->level.doors[idx].t - old_t) > 0.0001f) g->moved = true;
                    }
                }
            }
            if (!down) {
                if (g->moved) commit(e, "Door moved");
                cancel_gesture(e);
            }
            break;
        }
        case GESTURE_DOOR_RESIZE: {
            int idx = g->object_index;
            if (idx >= 0 && idx < e->level.door_count) {
                Door* d = &e->level.doors[idx];
                const Wall* w = level_wall_by_id_const(&e->level, d->wall);
                if (w) {
                    float len = wall_length(w);
                    if (len > 0.0001f) {
                        float tc = clampf(point_segment_t(world, w->a, w->b), 0.0f, 1.0f);
                        float width = fabsf(tc - g->fixed_t) * len;
                        if (e->snap) width = snap_scalar(width, e->grid_size);
                        width = clampf(width, DOOR_MIN_WIDTH, len);
                        float half = width / (2.0f * len);
                        float t = g->fixed_t + (tc >= g->fixed_t ? half : -half);
                        float old_t = d->t;
                        float old_w = d->width;
                        if (down) {
                            level_set_door_span(&e->level, idx, t, width);
                            if (fabsf(e->level.doors[idx].t - old_t) > 0.0001f ||
                                fabsf(e->level.doors[idx].width - old_w) > 0.0001f) {
                                g->moved = true;
                            }
                        }
                    }
                }
            }
            if (!down) {
                if (g->moved) commit(e, "Door resized");
                cancel_gesture(e);
            }
            break;
        }
        case GESTURE_NONE:
        case GESTURE_CHAIN:
        case GESTURE_POLYGON:
        default: break;
    }
}

static void close_wall_chain(Editor* e, bool via_enter) {
    Gesture* g = &e->gesture;
    if (g->kind != GESTURE_CHAIN) return;
    if (g->point_count < 3) {
        if (via_enter) set_status(e, false, "Need at least three points to close");
        return;
    }
    Vec2 first = g->pts[0];
    Vec2 last = g->pts[g->point_count - 1];
    bool same_x = fabsf(last.x - first.x) <= 0.001f;
    bool same_y = fabsf(last.y - first.y) <= 0.001f;
    int walls_needed = (same_x || same_y) ? 1 : 2;
    if (e->level.wall_count + walls_needed > LEVEL_MAX_WALLS) {
        set_status(e, true, "Cannot close: wall limit reached");
        return;
    }

    Vec2 poly[LEVEL_MAX_ROOM_PTS + 1];
    int poly_count = g->point_count;
    memcpy(poly, g->pts, (size_t)poly_count * sizeof(Vec2));
    bool added = false;

    if (same_x && same_y) {
        added = true;
    } else if (same_x || same_y) {
        added = level_add_wall(&e->level, last, first) >= 0;
    } else {
        /* The last click is not axis-aligned with the first. Close with an
           L-shaped pair so the loop still closes with orthogonal walls. */
        Vec2 prev = g->pts[g->point_count - 2];
        bool last_horizontal = fabsf(last.y - prev.y) <= 0.001f;
        Vec2 corner = last_horizontal ? v2(last.x, first.y) : v2(first.x, last.y);
        if (!v2_equal(corner, last, 0.0001f) &&
            level_add_wall(&e->level, last, corner) >= 0 &&
            !v2_equal(corner, first, 0.0001f) &&
            level_add_wall(&e->level, corner, first) >= 0) {
            added = true;
            if (poly_count < LEVEL_MAX_ROOM_PTS) poly[poly_count++] = corner;
        }
    }
    if (!added) {
        set_status(e, true, "Closing segment must be orthogonal and non-zero");
        return;
    }

    bool made_room = false;
    if (!g->truncated && poly_count >= 3 && poly_count <= LEVEL_MAX_ROOM_PTS) {
        int room = level_add_room(&e->level, poly, poly_count, e->fill_color);
        if (room >= 0) {
            made_room = true;
            sel_only(e, SEL_ROOM, room);
        } else {
            set_status(e, true, "Walls closed, but the fill could not be created");
        }
    } else if (g->truncated) {
        set_status(e, true, "Point limit reached: walls closed without a fill");
    }
    if (!made_room) sel_clear(e);
    cancel_gesture(e);
    commit(e, made_room ? "Room fill added" : "Walls closed");
}

static void close_polygon(Editor* e) {
    Gesture* g = &e->gesture;
    if (g->kind != GESTURE_POLYGON || g->point_count < 3) {
        set_status(e, true, "A room needs at least three points");
        return;
    }
    int room = level_add_room(&e->level, g->pts, g->point_count, e->fill_color);
    cancel_gesture(e);
    if (room < 0) {
        set_status(e, true, "Could not create room: level is full or polygon is degenerate");
        return;
    }
    sel_only(e, SEL_ROOM, room);
    commit(e, "Room added");
}

static void cancel_transient(Editor* e) {
    if (e->gesture.kind == GESTURE_MARQUEE) {
        cancel_gesture(e);
        set_status(e, false, "Marquee cancelled");
    } else if (e->gesture.kind == GESTURE_CHAIN || e->gesture.kind == GESTURE_POLYGON) {
        cancel_gesture(e);
        set_status(e, false, "Draft cancelled");
    } else if (gesture_is_drag(e)) {
        bool restored = history_restore_current(&e->history, &e->level);
        sel_prune(e);
        cancel_gesture(e);
        set_status(e, false, restored ? "Drag cancelled" : "Drag cancelled, no history");
    }
}

static void begin_select(Editor* e, Vec2 world, ImVec2 mouse) {
    ImGuiIO* io = igGetIO();
    bool shift = io->KeyShift;
    bool ctrl = io->KeyCtrl;
    Hit hit = hit_test(e, world);
    if (hit.kind == SEL_NONE) {
        cancel_gesture(e);
        if (!shift) sel_clear(e);
        Gesture* g = &e->gesture;
        g->kind = GESTURE_MARQUEE;
        g->press_world = world;
        g->cur_world = world;
        g->press_screen = v2(mouse.x, mouse.y);
        g->additive = shift;
        g->moved = false;
        set_status(e, false, shift ? "Additive marquee" : "Marquee");
        return;
    }

    if (ctrl) {
        cancel_gesture(e);
        sel_toggle(e, hit.kind, hit.index);
        int n = sel_count(&e->selection);
        if (n == 0) set_status(e, false, "Selection cleared");
        else set_status(e, false, "%d object%s selected", n, n == 1 ? "" : "s");
        return;
    }

    if (shift) {
        cancel_gesture(e);
        sel_add(e, hit.kind, hit.index);
        int n = sel_count(&e->selection);
        set_status(e, false, "%d object%s selected", n, n == 1 ? "" : "s");
        return;
    }

    bool in_group = sel_count(&e->selection) > 1 &&
                    sel_has(&e->selection, hit.kind, hit.index);
    if (in_group) sel_set_primary(e, hit.kind, hit.index);
    else sel_only(e, hit.kind, hit.index);
    cancel_gesture(e);

    if (hit.kind == SEL_WALL) {
        Vec2 anchor;
        if (!primary_anchor(e, &anchor)) return;
        begin_move(e, world, anchor);
        int n = sel_count(&e->selection);
        if (n > 1) {
            set_status(e, false, "%d objects selected", n);
        } else {
            set_status(e, false, "Wall %u selected",
                       (unsigned)e->level.walls[hit.index].id);
        }
        return;
    }
    if (hit.kind == SEL_DOOR) {
        Door* d = &e->level.doors[hit.index];
        const Wall* w = level_wall_by_id_const(&e->level, d->wall);
        if (!w) return;
        float len = wall_length(w);
        if (len <= 0.0001f) return;
        Vec2 a, b;
        level_door_span(&e->level, d, &a, &b);
        float da = screen_dist(mouse, world_to_screen(e, a));
        float db = screen_dist(mouse, world_to_screen(e, b));
        float half = d->width / (2.0f * len);
        if (da <= DRAG_END_PX) {
            e->gesture.kind = GESTURE_DOOR_RESIZE;
            e->gesture.fixed_t = d->t + half;
        } else if (db <= DRAG_END_PX) {
            e->gesture.kind = GESTURE_DOOR_RESIZE;
            e->gesture.fixed_t = d->t - half;
        } else if (sel_count(&e->selection) > 1) {
            Vec2 anchor;
            if (!primary_anchor(e, &anchor)) return;
            begin_move(e, world, anchor);
            set_status(e, false, "%d objects selected", sel_count(&e->selection));
            return;
        } else {
            e->gesture.kind = GESTURE_DOOR_SLIDE;
        }
        e->gesture.object_index = hit.index;
        e->gesture.moved = false;
        set_status(e, false, "Door %d selected", hit.index + 1);
        return;
    }

    Vec2 anchor;
    if (!primary_anchor(e, &anchor)) return;
    begin_move(e, world, anchor);
    set_status(e, false, "%s selected",
               hit.kind == SEL_ROOM ? "Room" :
               hit.kind == SEL_LABEL ? "Label" : "Stairs");
}

static void begin_wall_click(Editor* e, Vec2 world, ImVec2 mouse) {
    Gesture* g = &e->gesture;
    if (g->kind != GESTURE_CHAIN) {
        cancel_gesture(e);
        sel_clear(e);
        g = &e->gesture;
        g->kind = GESTURE_CHAIN;
        g->point_count = 1;
        g->pts[0] = snapped(e, world);
        g->wall_count = 0;
        g->truncated = false;
        set_status(e, false, "Wall chain started");
        return;
    }
    ImVec2 first = world_to_screen(e, g->pts[0]);
    if (g->point_count >= 3 && screen_dist(mouse, first) <= CLOSE_TOL_PX) {
        close_wall_chain(e, false);
        return;
    }
    Vec2 next = constrained(e, g->pts[g->point_count - 1], world);
    if (v2_equal(next, g->pts[g->point_count - 1], 0.0001f)) {
        set_status(e, false, "Click farther from the last point");
        return;
    }
    int wall = level_add_wall(&e->level, g->pts[g->point_count - 1], next);
    if (wall < 0) {
        set_status(e, true, "Could not add wall (capacity or invalid segment)");
        return;
    }
    g->wall_count++;
    commit(e, "Wall added");
    bool full = g->point_count >= LEVEL_MAX_ROOM_PTS;
    if (full) {
        g->truncated = true;
        set_status(e, true, "Chain point limit reached; the fill will be skipped");
    } else {
        g->pts[g->point_count++] = next;
    }
}

static void begin_room_click(Editor* e, Vec2 world, ImVec2 mouse) {
    Gesture* g = &e->gesture;
    if (g->kind != GESTURE_POLYGON) {
        cancel_gesture(e);
        sel_clear(e);
        g = &e->gesture;
        g->kind = GESTURE_POLYGON;
        g->point_count = 1;
        g->pts[0] = snapped(e, world);
        set_status(e, false, "Fill started");
        return;
    }
    ImVec2 first = world_to_screen(e, g->pts[0]);
    bool near = g->point_count >= 3 && screen_dist(mouse, first) <= CLOSE_TOL_PX;
    if (near || igGetMouseClickedCount(0) == 2) {
        if (g->point_count >= 3) {
            close_polygon(e);
        } else {
            set_status(e, true, "A room needs at least three points");
        }
        return;
    }
    if (g->point_count >= LEVEL_MAX_ROOM_PTS) {
        set_status(e, true, "Room point limit reached");
        return;
    }
    Vec2 p = snapped(e, world);
    if (v2_equal(p, g->pts[g->point_count - 1], 0.0001f)) return;
    g->pts[g->point_count++] = p;
}

static void begin_door_click(Editor* e, int hover_wall, float hover_t) {
    if (hover_wall < 0) {
        set_status(e, true, "Click near a wall to place a door");
        return;
    }
    const Wall* w = &e->level.walls[hover_wall];
    float len = wall_length(w);
    add_door_to_wall(e, w->id, hover_t, fminf(44.0f, len));
}

static void begin_label_click(Editor* e, Vec2 world) {
    Vec2 p = snapped(e, world);
    int idx = level_add_label(&e->level, p, "Room", 19.0f);
    if (idx < 0) {
        set_status(e, true, "Could not add label: level is full");
        return;
    }
    sel_only(e, SEL_LABEL, idx);
    e->focus_label_input = true;
    commit(e, "Label added");
}

static void begin_stairs_click(Editor* e, Vec2 world) {
    Vec2 p = snapped(e, world);
    int idx = level_add_stairs(&e->level, p, e->stairs_dir, e->stairs_steps);
    if (idx < 0) {
        set_status(e, true, "Could not add stairs: level is full");
        return;
    }
    sel_only(e, SEL_STAIRS, idx);
    commit(e, "Stairs added");
}

static void handle_canvas_click(Editor* e, Vec2 world, ImVec2 mouse, int hover_wall,
                                float hover_t) {
    switch (e->tool) {
        case TOOL_SELECT: begin_select(e, world, mouse); break;
        case TOOL_WALL: begin_wall_click(e, world, mouse); break;
        case TOOL_ROOM: begin_room_click(e, world, mouse); break;
        case TOOL_DOOR: begin_door_click(e, hover_wall, hover_t); break;
        case TOOL_LABEL: begin_label_click(e, world); break;
        case TOOL_STAIRS: begin_stairs_click(e, world); break;
        default: break;
    }
}

static void handle_canvas_input(Editor* e, bool hovered) {
    ImGuiIO* io = igGetIO();
    if (e->modal != MODAL_NONE) return;

    Vec2 world = screen_to_world(e, io->MousePos);
    if (hovered) {
        e->cursor_world = world;
        e->cursor_valid = true;
    } else if (!gesture_is_drag(e)) {
        e->cursor_valid = false;
    }

    if (hovered && io->MouseWheel != 0.0f) {
        Vec2 before = screen_to_world(e, io->MousePos);
        float old_zoom = e->camera.zoom;
        float nz = clampf(old_zoom * powf(1.18f, io->MouseWheel), 0.15f, 6.0f);
        if (fabsf(nz - old_zoom) > 0.00001f) {
            e->camera.zoom = nz;
            Vec2 after = screen_to_world(e, io->MousePos);
            e->camera.center = v2_add(e->camera.center, v2_sub(before, after));
        }
    }

    bool space = igIsKeyDown(ImGuiKey_Space);
    if (!e->pan_active && hovered &&
        (io->MouseDown[2] || (space && io->MouseDown[0] && !gesture_active(e)))) {
        e->pan_active = true;
    }
    if (e->pan_active) {
        bool held = io->MouseDown[2] || (space && io->MouseDown[0]);
        if (!held) {
            e->pan_active = false;
        } else {
            e->camera.center = v2_add(e->camera.center,
                                      v2_scale(v2(io->MouseDelta.x, io->MouseDelta.y),
                                               -1.0f / e->camera.zoom));
            return;
        }
    }

    update_drag(e);

    int hover_wall = -1;
    float hover_t = 0.0f;
    if (e->tool == TOOL_DOOR && hovered) {
        float tol = DOOR_TOL_PX / e->camera.zoom;
        hover_wall = level_nearest_wall(&e->level, world, tol, &hover_t);
    }

    if (hovered && igIsMouseClicked(1)) {
        cancel_transient(e);
        return;
    }
    if (hovered && igIsMouseClicked(0) && !e->pan_active) {
        handle_canvas_click(e, world, io->MousePos, hover_wall, hover_t);
    }
}

static void draw_canvas(Editor* e, ImVec2 mouse) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus;
    igPushStyleColorImVec4(ImGuiCol_WindowBg, v4c(CANVAS_BG, 1.0f));
    igPushStyleVarImVec2(ImGuiStyleVar_WindowPadding, (ImVec2){ 0.0f, 0.0f });
    igPushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    igSetNextWindowPos((ImVec2){ e->canvas.min.x, e->canvas.min.y }, ImGuiCond_Always);
    igSetNextWindowSize((ImVec2){ e->canvas.max.x - e->canvas.min.x,
                                  e->canvas.max.y - e->canvas.min.y },
                        ImGuiCond_Always);
    igBegin("##canvas", NULL, flags);
    igPopStyleColorEx(1);
    igPopStyleVarEx(2);

    igSetCursorScreenPos((ImVec2){ e->canvas.min.x, e->canvas.min.y });
    igInvisibleButton("##canvas_input",
                      (ImVec2){ e->canvas.max.x - e->canvas.min.x,
                                e->canvas.max.y - e->canvas.min.y },
                      0);
    bool hovered = igIsItemHovered(0);
    bool inside = rect_contains(e->canvas, v2(mouse.x, mouse.y));
    if (e->fit_pending) {
        fit_camera(e);
        e->fit_pending = false;
    }

    handle_canvas_input(e, hovered && inside);

    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_AddRectFilledEx(dl, v2_to_im(e->canvas.min), v2_to_im(e->canvas.max),
                               col32(CANVAS_BG), 0.0f, 0);
    draw_grid(e);
    draw_rooms(e);
    draw_stairs(e);
    draw_walls(e);
    draw_doors(e);
    draw_labels(e);

    int hover_wall = -1;
    float hover_t = 0.0f;
    if (e->tool == TOOL_DOOR && hovered) {
        Vec2 world = screen_to_world(e, mouse);
        hover_wall = level_nearest_wall(&e->level, world,
                                        DOOR_TOL_PX / e->camera.zoom, &hover_t);
    }
    draw_door_preview(e, hover_wall, hover_t);

    if (e->gesture.kind == GESTURE_CHAIN) draw_chain_preview(e, mouse);
    if (e->gesture.kind == GESTURE_POLYGON) draw_polygon_preview(e, mouse);
    draw_selection(e);
    draw_marquee(e);
    draw_status_strip(e);

    igEnd();
}

static void draw_menu(Editor* e) {
    if (igBeginMainMenuBar()) {
        if (igBeginMenu("File")) {
            if (igMenuItemEx("New", NULL, false, true)) new_level(e);
            if (igMenuItemEx("Open...", "Ctrl+O", false, true)) {
                e->request_modal = MODAL_OPEN;
                e->modal_error[0] = '\0';
            }
            igSeparator();
            bool can_save = e->current_path[0] != '\0';
            if (igMenuItemEx("Save", "Ctrl+S", false, can_save)) {
                save_level_as(e, e->current_path);
            }
            if (igMenuItemEx("Save As...", "Ctrl+Shift+S", false, true)) {
                e->request_modal = MODAL_SAVE_AS;
                snprintf(e->path_buf, sizeof e->path_buf, "%s",
                         e->current_path[0] ? e->current_path : "levels/re2-library.lvl");
                e->modal_error[0] = '\0';
            }
            igEndMenu();
        }
        if (igBeginMenu("Edit")) {
            bool busy = gesture_is_drag(e);
            bool undo_ok = history_can_undo(&e->history) && !busy;
            bool redo_ok = history_can_redo(&e->history) && !busy;
            if (igMenuItemEx("Undo", "Ctrl+Z", false, undo_ok)) do_undo(e);
            if (igMenuItemEx("Redo", "Ctrl+Shift+Z", false, redo_ok)) do_redo(e);
            igSeparator();
            if (igMenuItemEx("Delete", "Del", false, sel_count(&e->selection) > 0))
                delete_selection(e);
            if (igMenuItemEx("Deselect", "Esc", false, sel_count(&e->selection) > 0))
                sel_clear(e);
            igEndMenu();
        }
        if (igBeginMenu("View")) {
            if (igMenuItemEx("Fit level", "F", false, true)) e->fit_pending = true;
            if (igMenuItemEx("Show grid", "G", e->show_grid, true)) e->show_grid = !e->show_grid;
            igEndMenu();
        }
        if (igBeginMenu("Help")) {
            if (igMenuItemEx("Controls", "H", false, true)) {
                e->request_modal = MODAL_HELP;
                e->modal_error[0] = '\0';
            }
            igEndMenu();
        }
        e->menu_height = igGetWindowHeight();
        igEndMainMenuBar();
    } else {
        e->menu_height = igGetFrameHeight();
    }
}

static void draw_left_panel(Editor* e) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus;
    igPushStyleColorImVec4(ImGuiCol_WindowBg, v4c(PANEL_BG, 1.0f));
    igSetNextWindowPos((ImVec2){ 0.0f, e->menu_height }, ImGuiCond_Always);
    igSetNextWindowSize((ImVec2){ PANEL_LEFT_W, e->display_h - e->menu_height },
                        ImGuiCond_Always);
    igBegin("##tools", NULL, flags);
    igPopStyleColorEx(1);

    heading("Tools");
    for (int i = 0; i < TOOL_COUNT; i++) {
        if (tool_button(e, (Tool)i)) switch_tool(e, (Tool)i);
    }

    igDummy((ImVec2){ 0.0f, 4.0f });
    heading("Tool options");

    static const float grid_sizes[4] = { 4.0f, 8.0f, 16.0f, 32.0f };
    int grid_index = 1;
    for (int i = 0; i < 4; i++) {
        if (fabsf(grid_sizes[i] - e->grid_size) < 0.01f) grid_index = i;
    }
    igSetNextItemWidth(88.0f);
    if (igCombo("Grid", &grid_index, "4\0" "8\0" "16\0" "32\0")) {
        e->grid_size = grid_sizes[grid_index];
    }
    igCheckbox("Snap", &e->snap);
    igSameLineEx(0.0f, 12.0f);
    igCheckbox("Show grid", &e->show_grid);

    igDummy((ImVec2){ 0.0f, 3.0f });
    igTextDisabled("Fill color");
    float half = (igGetContentRegionAvail().x - 6.0f) * 0.5f;
    if (swatch_button("##fillblue", "Blue", col32(ROOM_BLUE_COL),
                      e->fill_color == ROOM_BLUE, half)) {
        e->fill_color = ROOM_BLUE;
    }
    igSameLineEx(0.0f, 6.0f);
    if (swatch_button("##fillred", "Red", col32(ROOM_RED_COL),
                      e->fill_color == ROOM_RED, half)) {
        e->fill_color = ROOM_RED;
    }

    igDummy((ImVec2){ 0.0f, 3.0f });
    igTextDisabled("Door color");
    float third = (igGetContentRegionAvail().x - 12.0f) / 3.0f;
    if (swatch_button("##doorwhite", "White", col32(DOOR_WHITE_COL),
                      e->door_color == DOOR_WHITE, third)) {
        e->door_color = DOOR_WHITE;
    }
    igSameLineEx(0.0f, 6.0f);
    if (swatch_button("##doorred", "Red", col32(DOOR_RED_COL),
                      e->door_color == DOOR_RED, third)) {
        e->door_color = DOOR_RED;
    }
    igSameLineEx(0.0f, 6.0f);
    if (swatch_button("##doorblue", "Blue", col32(DOOR_BLUE_COL),
                      e->door_color == DOOR_BLUE, third)) {
        e->door_color = DOOR_BLUE;
    }

    igDummy((ImVec2){ 0.0f, 3.0f });
    igTextDisabled("Stairs direction");
    float stair_half = (igGetContentRegionAvail().x - 6.0f) * 0.5f;
    if (e->stairs_dir == STAIRS_H) {
        igPushStyleColorImVec4(ImGuiCol_Button, v4c(0x2b3f50, 1.0f));
        if (igButtonEx("Horiz", (ImVec2){ stair_half, 22.0f }))
            e->stairs_dir = STAIRS_H;
        igPopStyleColorEx(1);
        igSameLineEx(0.0f, 6.0f);
        if (igButtonEx("Vert", (ImVec2){ stair_half, 22.0f }))
            e->stairs_dir = STAIRS_V;
    } else {
        if (igButtonEx("Horiz", (ImVec2){ stair_half, 22.0f }))
            e->stairs_dir = STAIRS_H;
        igSameLineEx(0.0f, 6.0f);
        igPushStyleColorImVec4(ImGuiCol_Button, v4c(0x2b3f50, 1.0f));
        if (igButtonEx("Vert", (ImVec2){ stair_half, 22.0f }))
            e->stairs_dir = STAIRS_V;
        igPopStyleColorEx(1);
    }
    igSetNextItemWidth(120.0f);
    igSliderInt("Steps", &e->stairs_steps, STAIR_MIN_STEPS, STAIR_MAX_STEPS);

    igDummy((ImVec2){ 0.0f, 2.0f });
    heading("Selection");
    small_note("Shift-click adds. Ctrl-click toggles. Drag to marquee.");
    small_note("Labels, stairs, and rooms move together; snap follows the grid.");

    igEnd();
}

static void object_delete_button(Editor* e) {
    igPushStyleColorImVec4(ImGuiCol_Button, v4c(0x4a2028, 1.0f));
    igPushStyleColorImVec4(ImGuiCol_ButtonHovered, v4c(0x5c2832, 1.0f));
    igPushStyleColorImVec4(ImGuiCol_ButtonActive, v4c(0x6d303b, 1.0f));
    if (igButtonEx("Delete", (ImVec2){ -FLT_MIN, 28.0f })) delete_selection(e);
    igPopStyleColorEx(3);
}

static void inspector_wall(Editor* e, int idx) {
    Wall* w = &e->level.walls[idx];
    heading("Wall");
    igText("Wall id %u", (unsigned)w->id);
    igText("Axis %s", wall_is_vertical(w) ? "Vertical" : "Horizontal");
    igText("Length %.1f", wall_length(w));
    igText("From %.1f, %.1f", w->a.x, w->a.y);
    igText("To   %.1f, %.1f", w->b.x, w->b.y);
    igDummy((ImVec2){ 0.0f, 4.0f });
    if (igButtonEx("Add door at midpoint", (ImVec2){ -FLT_MIN, 28.0f })) {
        float len = wall_length(w);
        add_door_to_wall(e, w->id, 0.5f, fminf(44.0f, len));
    }
    object_delete_button(e);
}

static void inspector_room(Editor* e, int idx) {
    Room* room = &e->level.rooms[idx];
    heading("Room");
    igText("Room %d of %d", idx + 1, e->level.room_count);
    igTextDisabled("Fill color");
    bool red = room->color == ROOM_RED;
    float half = (igGetContentRegionAvail().x - 6.0f) * 0.5f;
    if (swatch_button("##roomblue", "Blue", col32(ROOM_BLUE_COL), !red, half)) {
        level_set_room_color(&e->level, idx, ROOM_BLUE);
        commit(e, "Room color changed");
    }
    igSameLineEx(0.0f, 6.0f);
    if (swatch_button("##roomred", "Red", col32(ROOM_RED_COL), red, half)) {
        level_set_room_color(&e->level, idx, ROOM_RED);
        commit(e, "Room color changed");
    }
    igText("Vertices %d", room->count);
    object_delete_button(e);
}

static void inspector_door(Editor* e, int idx) {
    Door* d = &e->level.doors[idx];
    const Wall* w = level_wall_by_id_const(&e->level, d->wall);
    if (!w) return;
    float len = wall_length(w);
    heading("Door");
    igText("Door %d of %d", idx + 1, e->level.door_count);
    igText("Wall id %u", (unsigned)d->wall);
    igTextDisabled("Color");
    bool white = d->color == DOOR_WHITE;
    bool red = d->color == DOOR_RED;
    bool blue = d->color == DOOR_BLUE;
    float third = (igGetContentRegionAvail().x - 12.0f) / 3.0f;
    if (swatch_button("##doorwhite", "White", col32(DOOR_WHITE_COL), white, third)) {
        level_set_door_color(&e->level, idx, DOOR_WHITE);
        commit(e, "Door color changed");
    }
    igSameLineEx(0.0f, 6.0f);
    if (swatch_button("##doorred", "Red", col32(DOOR_RED_COL), red, third)) {
        level_set_door_color(&e->level, idx, DOOR_RED);
        commit(e, "Door color changed");
    }
    igSameLineEx(0.0f, 6.0f);
    if (swatch_button("##doorblue", "Blue", col32(DOOR_BLUE_COL), blue, third)) {
        level_set_door_color(&e->level, idx, DOOR_BLUE);
        commit(e, "Door color changed");
    }

    float half = d->width / (2.0f * fmaxf(len, 0.0001f));
    float lo = half;
    float hi = 1.0f - half;
    if (hi < lo) { lo = hi = 0.5f; }
    float t = d->t;
    igSetNextItemWidth(128.0f);
    if (igSliderFloatEx("Position", &t, lo, hi, "%.3f", 0)) {
        level_set_door_span(&e->level, idx, t, d->width);
    }
    if (igIsItemDeactivatedAfterEdit()) commit(e, "Door position changed");

    float width = d->width;
    float wmax = fmaxf(len, DOOR_MIN_WIDTH);
    igSetNextItemWidth(128.0f);
    if (igSliderFloatEx("Width", &width, DOOR_MIN_WIDTH, wmax, "%.1f", 0)) {
        level_set_door_span(&e->level, idx, d->t, width);
    }
    if (igIsItemDeactivatedAfterEdit()) commit(e, "Door width changed");
    igText("Thickness %.0f", DOOR_THICKNESS);
    object_delete_button(e);
}

static void inspector_label(Editor* e, int idx) {
    Label* label = &e->level.labels[idx];
    heading("Label");
    igText("Label %d of %d", idx + 1, e->level.label_count);
    if (!e->label_buf_ready) {
        snprintf(e->label_buf, sizeof e->label_buf, "%s", label->text);
        e->label_buf_ready = true;
    }
    if (e->focus_label_input) {
        igSetKeyboardFocusHere();
    }
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
    if (e->focus_label_input) flags |= ImGuiInputTextFlags_AutoSelectAll;
    igSetNextItemWidth(-FLT_MIN);
    bool enter = igInputText("Text", e->label_buf, sizeof e->label_buf, flags);
    bool deactivated = igIsItemDeactivatedAfterEdit();
    if (enter || deactivated) {
        level_set_label_text(&e->level, idx, e->label_buf);
        commit(e, "Label text changed");
    }
    e->focus_label_input = false;

    float size = label->size;
    igSetNextItemWidth(128.0f);
    if (igSliderFloatEx("Size", &size, 8.0f, 48.0f, "%.0f", 0)) {
        level_set_label_size(&e->level, idx, size);
    }
    if (igIsItemDeactivatedAfterEdit()) commit(e, "Label size changed");
    igText("Position %.1f, %.1f", label->pos.x, label->pos.y);
    object_delete_button(e);
}

static void inspector_stairs(Editor* e, int idx) {
    Stairs* s = &e->level.stairs[idx];
    heading("Stairs");
    igText("Stairs %d of %d", idx + 1, e->level.stairs_count);
    igTextDisabled("Direction");
    float dir_half = (igGetContentRegionAvail().x - 6.0f) * 0.5f;
    if (s->dir == STAIRS_H) {
        igPushStyleColorImVec4(ImGuiCol_Button, v4c(0x2b3f50, 1.0f));
        if (igButtonEx("Horizontal", (ImVec2){ dir_half, 24.0f })) {
            level_set_stairs(&e->level, idx, STAIRS_H, s->steps);
        }
        igPopStyleColorEx(1);
        igSameLineEx(0.0f, 6.0f);
        if (igButtonEx("Vertical", (ImVec2){ dir_half, 24.0f })) {
            level_set_stairs(&e->level, idx, STAIRS_V, s->steps);
            commit(e, "Stairs direction changed");
        }
    } else {
        if (igButtonEx("Horizontal", (ImVec2){ dir_half, 24.0f })) {
            level_set_stairs(&e->level, idx, STAIRS_H, s->steps);
            commit(e, "Stairs direction changed");
        }
        igSameLineEx(0.0f, 6.0f);
        igPushStyleColorImVec4(ImGuiCol_Button, v4c(0x2b3f50, 1.0f));
        if (igButtonEx("Vertical", (ImVec2){ dir_half, 24.0f })) {
            level_set_stairs(&e->level, idx, STAIRS_V, s->steps);
        }
        igPopStyleColorEx(1);
    }
    int steps = s->steps;
    igSetNextItemWidth(128.0f);
    if (igSliderInt("Steps", &steps, STAIR_MIN_STEPS, STAIR_MAX_STEPS)) {
        level_set_stairs(&e->level, idx, s->dir, steps);
    }
    if (igIsItemDeactivatedAfterEdit()) commit(e, "Stairs step count changed");
    igText("Position %.1f, %.1f", s->pos.x, s->pos.y);
    object_delete_button(e);
}

static void inspector_many(Editor* e) {
    Selection* s = &e->selection;
    heading("Selection");
    igText("%d objects selected", sel_count(s));
    igDummy((ImVec2){ 0.0f, 4.0f });
    if (sel_count_kind(s, SEL_WALL)) igText("Walls   %d", sel_count_kind(s, SEL_WALL));
    if (sel_count_kind(s, SEL_ROOM)) igText("Rooms   %d", sel_count_kind(s, SEL_ROOM));
    if (sel_count_kind(s, SEL_DOOR)) igText("Doors   %d", sel_count_kind(s, SEL_DOOR));
    if (sel_count_kind(s, SEL_LABEL)) igText("Labels  %d", sel_count_kind(s, SEL_LABEL));
    if (sel_count_kind(s, SEL_STAIRS)) igText("Stairs  %d", sel_count_kind(s, SEL_STAIRS));
    igDummy((ImVec2){ 0.0f, 6.0f });
    small_note("Labels, stairs, and rooms move together. Doors slide individually.");
    igDummy((ImVec2){ 0.0f, 6.0f });
    object_delete_button(e);
}

static void draw_inspector(Editor* e) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus;
    igPushStyleColorImVec4(ImGuiCol_WindowBg, v4c(PANEL_BG, 1.0f));
    igSetNextWindowPos((ImVec2){ e->display_w - PANEL_RIGHT_W, e->menu_height },
                       ImGuiCond_Always);
    igSetNextWindowSize((ImVec2){ PANEL_RIGHT_W, e->display_h - e->menu_height },
                        ImGuiCond_Always);
    igBegin("##inspector", NULL, flags);
    igPopStyleColorEx(1);

    sel_prune(e);
    SelRef single;
    if (sel_single(&e->selection, &single)) {
        switch (single.kind) {
            case SEL_WALL: inspector_wall(e, single.index); break;
            case SEL_ROOM: inspector_room(e, single.index); break;
            case SEL_DOOR: inspector_door(e, single.index); break;
            case SEL_LABEL: inspector_label(e, single.index); break;
            case SEL_STAIRS: inspector_stairs(e, single.index); break;
            case SEL_NONE:
            default: break;
        }
    } else if (sel_count(&e->selection) > 1) {
        inspector_many(e);
    } else {
        heading("Inspector");
        igTextDisabled("Nothing selected");
        igDummy((ImVec2){ 0.0f, 4.0f });
        heading("Level");
        igText("Walls   %d", e->level.wall_count);
        igText("Rooms   %d", e->level.room_count);
        igText("Doors   %d", e->level.door_count);
        igText("Labels  %d", e->level.label_count);
        igText("Stairs  %d", e->level.stairs_count);
        igDummy((ImVec2){ 0.0f, 4.0f });
        heading("Controls");
        igPushFontFloat(igGetFont(), 12.0f);
        igTextDisabled("1-6 switch tools");
        igTextDisabled("F fit level, G toggle grid");
        igTextDisabled("Shift-click adds; Ctrl-click toggles");
        igTextDisabled("Drag empty canvas to marquee");
        igTextDisabled("Drag labels, stairs, or rooms together");
        igTextDisabled("Drag a door to slide; drag an end to resize");
        igTextDisabled("Delete removes every selected object");
        igTextDisabled("Esc deselects then cancels a draft");
        igTextDisabled("Ctrl+Z undo, Ctrl+Shift+Z redo");
        igTextDisabled("Ctrl+S save, Ctrl+O open");
        igTextDisabled("Middle-drag or Space+drag pans");
        igTextDisabled("Wheel zooms around the cursor");
        igPopFont();
        igDummy((ImVec2){ 0.0f, 6.0f });
        igTextDisabled("%s", e->current_path);
    }
    igEnd();
}

static int scan_levels(char paths[][512], int max) {
    DIR* dir = opendir("levels");
    if (!dir) return 0;
    int n = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && n < max) {
        const char* name = ent->d_name;
        size_t len = strlen(name);
        if (name[0] == '.' || len <= 4) continue;
        if (strcmp(name + len - 4, ".lvl") != 0) continue;
        snprintf(paths[n], 512, "levels/%s", name);
        n++;
    }
    closedir(dir);
    for (int i = 1; i < n; i++) {
        char key[512];
        snprintf(key, sizeof key, "%s", paths[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(paths[j], key) > 0) {
            snprintf(paths[j + 1], 512, "%s", paths[j]);
            j--;
        }
        snprintf(paths[j + 1], 512, "%s", key);
    }
    return n;
}

static const char* modal_name(Modal m) {
    switch (m) {
        case MODAL_OPEN: return "Open level";
        case MODAL_SAVE_AS: return "Save level as";
        case MODAL_HELP: return "Controls";
        case MODAL_NONE:
        default: return "";
    }
}

static void open_path_modal(Editor* e, Modal m) {
    e->request_modal = m;
    e->modal_error[0] = '\0';
    if (m == MODAL_SAVE_AS) {
        snprintf(e->path_buf, sizeof e->path_buf, "%s",
                 e->current_path[0] ? e->current_path : "levels/re2-library.lvl");
    }
}

static void draw_help_modal(Editor* e) {
    igTextColored(v4c(ACCENT_COL, 1.0f), "Ink Ribbon Level Editor");
    igSeparator();
    igText("Tools");
    igBullet();
    igText("1 Select   2 Wall   3 Room");
    igBullet();
    igText("4 Door   5 Label   6 Stairs");
    igText("Canvas");
    igBullet();
    igText("Left click uses the active tool.");
    igBullet();
    igText("Middle-drag or Space+left-drag pans.");
    igBullet();
    igText("Wheel zooms around the cursor.");
    igBullet();
    igText("F fits the level, G toggles the grid.");
    igText("Editing");
    igBullet();
    igText("Delete removes every selected object.");
    igBullet();
    igText("Shift-click adds to the selection; Ctrl-click toggles an object.");
    igBullet();
    igText("Drag empty canvas to marquee a group.");
    igBullet();
    igText("Drag labels, stairs, or rooms to move the group.");
    igBullet();
    igText("Drag a door to slide it; drag an end to resize.");
    igBullet();
    igText("Esc deselects, then cancels a draft.");
    igBullet();
    igText("Enter closes a wall chain or room draft.");
    igBullet();
    igText("Backspace removes the last chain wall.");
    igText("File");
    igBullet();
    igText("Ctrl+S save, Ctrl+Shift+S save as.");
    igBullet();
    igText("Ctrl+O open, Ctrl+Z undo, Ctrl+Y redo.");
    igDummy((ImVec2){ 0.0f, 6.0f });
    if (igButtonEx("Close", (ImVec2){ -FLT_MIN, 28.0f })) {
        igCloseCurrentPopup();
        e->modal = MODAL_NONE;
    }
}

static void draw_level_list(Editor* e) {
    char paths[64][512];
    int n = scan_levels(paths, 64);
    igTextDisabled("Levels");
    if (igBeginChild("##level_list", (ImVec2){ 0.0f, 132.0f }, 0, 0)) {
        if (n == 0) igTextDisabled("No .lvl files in levels/");
        for (int i = 0; i < n; i++) {
            const char* name = paths[i] + strlen("levels/");
            if (igSelectableEx(name, strcmp(e->path_buf, paths[i]) == 0, 0,
                               (ImVec2){ 0.0f, 0.0f })) {
                size_t len = strlen(paths[i]);
                if (len >= sizeof e->path_buf) len = sizeof e->path_buf - 1;
                memcpy(e->path_buf, paths[i], len);
                e->path_buf[len] = '\0';
                e->modal_error[0] = '\0';
            }
        }
    }
    igEndChild();
}

static void draw_modals(Editor* e) {
    if (e->request_modal != MODAL_NONE) {
        e->modal = e->request_modal;
        e->request_modal = MODAL_NONE;
        e->path_focus = true;
        igOpenPopup(modal_name(e->modal), 0);
    }
    if (e->modal == MODAL_NONE) return;

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (igBeginPopupModal(modal_name(e->modal), NULL, flags)) {
        if (igIsKeyPressedEx(ImGuiKey_Escape, false)) {
            igCloseCurrentPopup();
            e->modal = MODAL_NONE;
        }
        if (e->modal == MODAL_HELP) {
            draw_help_modal(e);
        } else if (e->modal == MODAL_OPEN || e->modal == MODAL_SAVE_AS) {
            igText("%s", e->modal == MODAL_OPEN ? "Open a level file" : "Save the level to a path");
            if (e->path_focus) igSetKeyboardFocusHere();
            ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue;
            if (e->path_focus) input_flags |= ImGuiInputTextFlags_AutoSelectAll;
            igSetNextItemWidth(360.0f);
            bool enter = igInputText("Path", e->path_buf, sizeof e->path_buf, input_flags);
            e->path_focus = false;
            draw_level_list(e);
            if (e->modal_error[0]) {
                igTextColored(v4c(ERROR_COL, 1.0f), "%s", e->modal_error);
            }
            igDummy((ImVec2){ 0.0f, 4.0f });
            const char* action = e->modal == MODAL_OPEN ? "Open" : "Save";
            bool do_action = enter;
            if (igButtonEx(action, (ImVec2){ 120.0f, 28.0f })) do_action = true;
            igSameLineEx(0.0f, 6.0f);
            if (igButtonEx("Cancel", (ImVec2){ 100.0f, 28.0f })) {
                igCloseCurrentPopup();
                e->modal = MODAL_NONE;
            }
            if (do_action && e->modal != MODAL_NONE) {
                bool ok = e->modal == MODAL_OPEN ? open_level(e, e->path_buf)
                                                 : save_level_as(e, e->path_buf);
                if (ok) {
                    igCloseCurrentPopup();
                    e->modal = MODAL_NONE;
                }
            }
        }
        igEndPopup();
    } else {
        e->modal = MODAL_NONE;
    }
}

static void handle_shortcuts(Editor* e) {
    ImGuiIO* io = igGetIO();
    if (e->modal != MODAL_NONE) return;
    if (io->WantTextInput) return;

    if (io->KeyCtrl) {
        bool shift = io->KeyShift;
        if (igIsKeyPressedEx(ImGuiKey_Z, false)) {
            if (shift) do_redo(e);
            else do_undo(e);
            return;
        }
        if (igIsKeyPressedEx(ImGuiKey_Y, false)) {
            do_redo(e);
            return;
        }
        if (igIsKeyPressedEx(ImGuiKey_S, false)) {
            if (shift) open_path_modal(e, MODAL_SAVE_AS);
            else if (e->current_path[0]) save_level_as(e, e->current_path);
            else open_path_modal(e, MODAL_SAVE_AS);
            return;
        }
        if (igIsKeyPressedEx(ImGuiKey_O, false)) {
            open_path_modal(e, MODAL_OPEN);
            return;
        }
    }

    for (int i = 0; i < TOOL_COUNT; i++) {
        if (igIsKeyPressedEx(ImGuiKey_1 + i, false)) {
            switch_tool(e, (Tool)i);
            return;
        }
    }
    if (igIsKeyPressedEx(ImGuiKey_F, false)) {
        e->fit_pending = true;
        set_status(e, false, "Fit level");
    }
    if (igIsKeyPressedEx(ImGuiKey_G, false)) e->show_grid = !e->show_grid;
    if (igIsKeyPressedEx(ImGuiKey_H, false)) open_path_modal(e, MODAL_HELP);
    if (igIsKeyPressedEx(ImGuiKey_Delete, false)) {
        if (gesture_is_drag(e)) return;
        delete_selection(e);
    }
    if (igIsKeyPressedEx(ImGuiKey_Backspace, false)) {
        if (e->gesture.kind == GESTURE_CHAIN) {
            Gesture* g = &e->gesture;
            if (g->wall_count > 0 && e->level.wall_count > 0) {
                if (level_delete_wall(&e->level, e->level.wall_count - 1)) {
                    g->wall_count--;
                    if (!g->truncated && g->point_count > 1) g->point_count--;
                    commit(e, "Last chain wall removed");
                }
            }
            if (g->wall_count == 0) {
                cancel_gesture(e);
                set_status(e, false, "Chain cleared");
            }
        }
    }
    if (igIsKeyPressedEx(ImGuiKey_Enter, false)) {
        if (e->gesture.kind == GESTURE_CHAIN) close_wall_chain(e, true);
        else if (e->gesture.kind == GESTURE_POLYGON) close_polygon(e);
    }
    if (igIsKeyPressedEx(ImGuiKey_Escape, false)) {
        if (gesture_is_drag(e)) {
            cancel_transient(e);
        } else if (sel_count(&e->selection) > 0) {
            sel_clear(e);
            set_status(e, false, "Selection cleared");
        } else if (gesture_active(e)) {
            cancel_transient(e);
        }
    }
}

Editor* editor_create(const char* startup_path) {
    Editor* e = calloc(1, sizeof *e);
    if (!e) return NULL;

    apply_theme();
    level_init(&e->level);
    if (!history_init(&e->history)) {
        set_status(e, true, "Undo history unavailable");
    }
    history_reset(&e->history, &e->level);

    e->camera.center = v2(0.0f, 0.0f);
    e->camera.zoom = 1.0f;
    e->tool = TOOL_SELECT;
    e->selection.primary.kind = SEL_NONE;
    e->selection.primary.index = -1;
    e->grid_size = 8.0f;
    e->snap = true;
    e->show_grid = true;
    e->fill_color = ROOM_BLUE;
    e->door_color = DOOR_WHITE;
    e->stairs_dir = STAIRS_H;
    e->stairs_steps = 8;
    e->fit_pending = true;
    e->modal = MODAL_NONE;
    e->request_modal = MODAL_NONE;
    e->menu_height = 23.0f;
    snprintf(e->current_path, sizeof e->current_path, "%s", "levels/re2-library.lvl");
    snprintf(e->path_buf, sizeof e->path_buf, "%s", e->current_path);

    const char* path = (startup_path && startup_path[0]) ? startup_path
                                                          : "levels/re2-library.lvl";
    bool loaded = false;
    if (file_exists(path)) {
        LevelError err;
        if (level_load(&e->level, path, &err)) {
            loaded = true;
            snprintf(e->current_path, sizeof e->current_path, "%s", path);
            snprintf(e->path_buf, sizeof e->path_buf, "%s", path);
        } else {
            set_status(e, true, "Startup level failed: %s", err.message);
        }
    } else {
        set_status(e, true, "Startup level not found: %s", path);
    }

    if (!loaded && strcmp(path, "levels/re2-library.lvl") != 0 &&
        file_exists("levels/re2-library.lvl")) {
        LevelError err;
        if (level_load(&e->level, "levels/re2-library.lvl", &err)) {
            loaded = true;
            snprintf(e->current_path, sizeof e->current_path, "%s",
                     "levels/re2-library.lvl");
            snprintf(e->path_buf, sizeof e->path_buf, "%s", e->current_path);
            set_status(e, false, "Loaded demo level instead");
        }
    }

    history_reset(&e->history, &e->level);
    if (loaded) {
        set_status(e, false, "Loaded %s", e->current_path);
    } else if (!e->status[0]) {
        set_status(e, true, "No level loaded");
    }
    return e;
}

void editor_destroy(Editor* e) {
    if (!e) return;
    history_shutdown(&e->history);
    free(e);
}

void editor_frame(Editor* e) {
    if (!e) return;
    ImGuiIO* io = igGetIO();
    e->display_w = io->DisplaySize.x;
    e->display_h = io->DisplaySize.y;

    draw_menu(e);

    float bottom = e->display_h;
    float canvas_x0 = PANEL_LEFT_W;
    float canvas_x1 = e->display_w - PANEL_RIGHT_W;
    if (canvas_x1 < canvas_x0 + 80.0f) canvas_x1 = canvas_x0 + 80.0f;
    e->canvas = rect_from_points(v2(canvas_x0, e->menu_height), v2(canvas_x1, bottom));

    draw_left_panel(e);
    draw_inspector(e);
    draw_canvas(e, io->MousePos);
    handle_shortcuts(e);
    draw_modals(e);
}
