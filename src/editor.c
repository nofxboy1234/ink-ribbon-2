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
/* Measured on the reference screenshot: floor blue #172832, red #391010,
   canvas black #040404. The wall colour stays as shipped. */
#define CANVAS_BG 0x040404
#define WALL_COL 0x8b9097
#define ROOM_BLUE_COL 0x172832
#define ROOM_RED_COL 0x391010
#define DOOR_WHITE_COL 0xf2f2f2
#define DOOR_RED_COL 0xb3323b
#define DOOR_BLUE_COL 0x3d7fb5
#define LABEL_COL 0xe6e6e6
/* The reference stair treads read as a warm tan (#5b534f in the compressed
   screenshot, dimmed by JPEG); this is that tan at the wall's brightness. */
#define STAIR_COL 0x9a8770
#define STAIR_FILL 0x1c2026
#define ACCENT_COL 0x5aa0c8
#define SELECT_COL 0x5aa0c8
#define VERTEX_SELECT_COL 0xe0a33c
#define TEXT_COL 0xd9dadd
#define MUTED_COL 0x7f868e
#define ERROR_COL 0xd07373
#define PANEL_BG 0x16181c
#define WINDOW_BG 0x101114
#define FRAME_BG 0x1d2025
#define BORDER_COL 0x2a2e34
#define ACCENT_UI 0x4e7f9e

/* Four tools own the mouse. Select edits what is there, Line draws walls,
   Label places text, Room drags a rectangle. The tool is the only owner of
   which gesture a press starts, so there is no second mode flag to keep in
   sync. */
typedef enum {
    TOOL_SELECT = 0,
    TOOL_LINE,
    TOOL_LABEL,
    TOOL_ROOM,
    TOOL_COUNT,
} Tool;

/* The Fill palette. One table drives the left panel swatches, the context Mode
   menu, and the fill a closing Line loop takes. Floor fills paint in pass 0
   under every room; the rest paint in creation order. */
typedef struct {
    const char* id;
    const char* label;
    ShapeFill fill;
    unsigned rgb;
    bool crossed;
} FillChoice;

static const FillChoice FILL_CHOICES[] = {
    { "##fillblue", "Blue", SHAPE_FILL_BLUE, ROOM_BLUE_COL, false },
    { "##fillred", "Red", SHAPE_FILL_RED, ROOM_RED_COL, false },
    { "##fillfloorblue", "Floor Blue", SHAPE_FILL_FLOOR_BLUE, ROOM_BLUE_COL, false },
    { "##fillfloorred", "Floor Red", SHAPE_FILL_FLOOR_RED, ROOM_RED_COL, false },
    { "##fillhole", "Hole", SHAPE_FILL_HOLE, CANVAS_BG, false },
    { "##fillempty", "Empty", SHAPE_FILL_EMPTY, CANVAS_BG, true },
};

#define FILL_CHOICE_COUNT ((int)(sizeof FILL_CHOICES / sizeof FILL_CHOICES[0]))

/* One clipped piece of a wall kept from the last marquee. Wall ids are stable
   across wall deletes, so a span survives an unrelated edit. */
typedef struct {
    WallId wall;
    float t0, t1;
} WallSpan;

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
    HIT_NONE = 0,
    HIT_WALL,
    HIT_SHAPE_INTERIOR,
    HIT_ROOM,
    HIT_DOOR,
    HIT_LABEL,
    HIT_STAIRS,
} HitKind;

typedef enum {
    MODAL_NONE = 0,
    MODAL_OPEN,
    MODAL_SAVE_AS,
    MODAL_HELP,
} Modal;

typedef enum {
    GESTURE_NONE = 0,
    GESTURE_CHAIN,
    GESTURE_MARQUEE,
    GESTURE_MOVE,
    GESTURE_RESHAPE,
    GESTURE_DOOR_SLIDE,
    GESTURE_DOOR_RESIZE,
    GESTURE_STAIRS_RESIZE,
    /* The Room tool drags a new rectangle, or Alt+drag copies one. */
    GESTURE_ROOM_DRAW,
    GESTURE_ROOM_COPY,
    /* A Line press waits for the release to tell a click from a plain drag. */
    GESTURE_LINE_PENDING,
} GestureKind;

/* Which edge of a stair footprint a resize drag moves. The opposite edge stays
   fixed, so the drag only ever changes one extent. */
typedef enum {
    STAIR_EDGE_NONE = 0,
    STAIR_EDGE_LEFT,
    STAIR_EDGE_RIGHT,
    STAIR_EDGE_TOP,
    STAIR_EDGE_BOTTOM,
} StairEdge;

/* What a press landed on. A shape interior and a single edge both resolve to
   a wall set so selection and gestures have one path. */
typedef struct {
    SelKind kind;
    int index;
    int walls[LEVEL_MAX_WALLS];
    int wall_count;
} Target;

typedef struct {
    GestureKind kind;
    Vec2 pts[LEVEL_MAX_WALLS + 1];
    int point_count;
    int wall_count;
    ShapeId shape;
    Vec2 press_screen;
    Vec2 press_world;
    Vec2 cur_world;
    Vec2 anchor;
    Vec2 move_applied;
    int object_index;
    bool walls[LEVEL_MAX_WALLS];
    int ref_wall_x;
    int ref_wall_y;
    float ref_start_x;
    float ref_start_y;
    Target collapse;
    bool collapse_on_release;
    bool additive;
    bool moved;
    float fixed_t;
    int stair_edge;
    float stair_fixed;
    /* The Line press interrupted an active click-click chain, so its points
       survive under the pending gesture and the release continues it. */
    bool chain_active;
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
    HitKind kind;
    int index;
} Hit;

/* One snapshot of what a right-click may act on. The line marquee and the
   select-mode selection both feed it, and the menu reads only this, so the
   popup cannot disagree with the selection it was opened from. */
typedef struct {
    bool open;
    bool can_fill;
    ShapeId fill_shape;
    WallSpan door_spans[LEVEL_MAX_WALLS];
    int door_span_count;
    bool can_stairs;
    Rect stairs_rect;
    Vec2 drag_from;
    Vec2 drag_to;
} ContextMenu;

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

    /* Line marquee result: clipped wall spans plus the snapped box the menu
       needs for stairs. Select reuses the box when its marquee caught nothing. */
    WallSpan spans[LEVEL_MAX_WALLS];
    int span_count;
    Rect line_box;
    bool line_box_valid;
    Vec2 line_drag_from;
    Vec2 line_drag_to;

    ContextMenu ctx;

    float grid_size;
    bool snap;
    bool show_grid;
    ShapeFill shape_fill;
    float label_size;

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
    "Select", "Line", "Label", "Room",
};

static const char* TOOL_HINTS[TOOL_COUNT] = {
    "click an object to select it, drag empty canvas to marquee, drag a wall to reshape",
    "click-click draws, Shift+drag marquees, right-click for Mode, Door, and Stairs",
    "click to place a label, then type its text",
    "drag an axis-aligned rectangle; right-click a room for Mode",
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

/* Only Floor Blue is an underlay. Floor Red paints in creation order so a red
   room can sit on top of a hole, as the reference's West Storage Room does. */
static bool shape_fill_is_floor(ShapeFill f) {
    return f == SHAPE_FILL_FLOOR_BLUE;
}

static bool shape_fill_paints(ShapeFill f) {
    return f != SHAPE_FILL_NONE && f != SHAPE_FILL_EMPTY;
}

static ImU32 door_color(DoorColor c) {
    switch (c) {
        case DOOR_RED: return col32(DOOR_RED_COL);
        case DOOR_BLUE: return col32(DOOR_BLUE_COL);
        case DOOR_WHITE:
        default: return col32(DOOR_WHITE_COL);
    }
}

static ImU32 shape_fill_color(ShapeFill fill) {
    switch (fill) {
        case SHAPE_FILL_BLUE:
        case SHAPE_FILL_FLOOR_BLUE: return col32(ROOM_BLUE_COL);
        case SHAPE_FILL_RED:
        case SHAPE_FILL_FLOOR_RED: return col32(ROOM_RED_COL);
        case SHAPE_FILL_HOLE: return col32(CANVAS_BG);
        case SHAPE_FILL_EMPTY:
        case SHAPE_FILL_NONE:
        default: return 0;
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

/* ImGui reports the pointer as -FLT_MAX when it has none, which is what a focus
   loss does. That is not a world position, so treat it as "no pointer". */
static bool mouse_available(ImVec2 p) {
    return isfinite(p.x) && isfinite(p.y) &&
           p.x > -0.5f * FLT_MAX && p.y > -0.5f * FLT_MAX;
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
    return e->gesture.kind == GESTURE_MOVE || e->gesture.kind == GESTURE_RESHAPE ||
           e->gesture.kind == GESTURE_DOOR_SLIDE ||
           e->gesture.kind == GESTURE_DOOR_RESIZE ||
           e->gesture.kind == GESTURE_STAIRS_RESIZE ||
           e->gesture.kind == GESTURE_ROOM_DRAW ||
           e->gesture.kind == GESTURE_ROOM_COPY ||
           e->gesture.kind == GESTURE_LINE_PENDING ||
           e->gesture.kind == GESTURE_MARQUEE;
}

static void cancel_gesture(Editor* e) {
    memset(&e->gesture, 0, sizeof e->gesture);
}

/* A chain creates its shape on the first click, so a chain cancelled before
   its first wall would otherwise leave an empty shape behind. */
static void discard_empty_chain_shape(Editor* e) {
    Gesture* g = &e->gesture;
    if (g->kind != GESTURE_CHAIN || g->shape == 0) return;
    int first_wall;
    if (level_shape_wall_indices(&e->level, g->shape, &first_wall, 1) > 0) return;
    level_delete_shape(&e->level, g->shape);
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

static bool target_selected(const Editor* e, const Target* target) {
    if (target->kind == SEL_WALL) {
        if (target->wall_count <= 0) return false;
        for (int i = 0; i < target->wall_count; i++) {
            if (!sel_has(&e->selection, SEL_WALL, target->walls[i])) return false;
        }
        return true;
    }
    if (target->kind == SEL_NONE) return false;
    return sel_has(&e->selection, target->kind, target->index);
}

static void target_add(Editor* e, const Target* target) {
    if (target->kind == SEL_WALL) {
        for (int i = 0; i < target->wall_count; i++) {
            sel_add(e, SEL_WALL, target->walls[i]);
        }
        return;
    }
    sel_add(e, target->kind, target->index);
}

static void target_remove(Editor* e, const Target* target) {
    if (target->kind == SEL_WALL) {
        for (int i = 0; i < target->wall_count; i++) {
            sel_remove(e, SEL_WALL, target->walls[i]);
        }
        return;
    }
    sel_remove(e, target->kind, target->index);
}

static void target_toggle(Editor* e, const Target* target) {
    if (target_selected(e, target)) target_remove(e, target);
    else target_add(e, target);
}

static void target_only(Editor* e, const Target* target) {
    sel_clear(e);
    target_add(e, target);
}

/* A press that never moved is a click: narrow a multi-selection to the object
   under the pointer, while a real drag keeps the group it started on. */
static void collapse_click(Editor* e) {
    Gesture* g = &e->gesture;
    if (g->moved || !g->collapse_on_release || g->collapse.kind == SEL_NONE) {
        return;
    }
    target_only(e, &g->collapse);
    if (g->collapse.index >= 0) {
        sel_set_primary(e, g->collapse.kind, g->collapse.index);
    }
    int n = sel_count(&e->selection);
    if (n == 1) set_status(e, false, "One object selected");
    else set_status(e, false, "%d objects selected", n);
}

static void target_from_hit(const Editor* e, Hit hit, Target* out) {
    memset(out, 0, sizeof *out);
    out->index = -1;
    switch (hit.kind) {
        case HIT_WALL:
            out->kind = SEL_WALL;
            out->index = hit.index;
            out->walls[0] = hit.index;
            out->wall_count = 1;
            break;
        case HIT_SHAPE_INTERIOR: {
            out->kind = SEL_WALL;
            if (hit.index < 0 || hit.index >= e->level.shape_count) break;
            out->wall_count = level_shape_wall_indices(
                &e->level, e->level.shapes[hit.index].id, out->walls,
                LEVEL_MAX_WALLS);
            if (out->wall_count > 0) out->index = out->walls[0];
            else out->wall_count = 0;
            break;
        }
        case HIT_ROOM: out->kind = SEL_ROOM; out->index = hit.index; break;
        case HIT_DOOR: out->kind = SEL_DOOR; out->index = hit.index; break;
        case HIT_LABEL: out->kind = SEL_LABEL; out->index = hit.index; break;
        case HIT_STAIRS: out->kind = SEL_STAIRS; out->index = hit.index; break;
        case HIT_NONE:
        default: out->kind = SEL_NONE; break;
    }
}

/* The whole shape selected means a rigid move; any other wall set reshapes. */
static bool selection_walls_whole_shape(const Editor* e) {
    const Selection* s = &e->selection;
    int selected = sel_count_kind(s, SEL_WALL);
    if (selected <= 0 || sel_count(s) != selected) return false;
    ShapeId shape = 0;
    int total = 0;
    for (int i = 0; i < e->level.wall_count; i++) {
        if (!s->walls[i]) continue;
        if (shape == 0) shape = e->level.walls[i].shape;
        else if (e->level.walls[i].shape != shape) return false;
    }
    if (shape == 0) return false;
    for (int i = 0; i < e->level.wall_count; i++) {
        if (e->level.walls[i].shape == shape) total++;
    }
    return total == selected;
}

static void switch_tool(Editor* e, Tool tool) {
    if (e->tool == tool) return;
    if (gesture_is_drag(e) && e->gesture.kind != GESTURE_MARQUEE) {
        history_restore_current(&e->history, &e->level);
        sel_prune(e);
    }
    if (e->gesture.kind == GESTURE_CHAIN) discard_empty_chain_shape(e);
    cancel_gesture(e);
    e->span_count = 0;
    e->line_box_valid = false;
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
    Hit hit = { HIT_NONE, -1 };
    float tol = HIT_TOL_PX / e->camera.zoom;

    for (int i = e->level.door_count - 1; i >= 0; i--) {
        Vec2 a, b;
        level_door_span(&e->level, &e->level.doors[i], &a, &b);
        float d = point_segment_dist(world, a, b);
        if (d <= tol + DOOR_THICKNESS * 0.5f) {
            hit.kind = HIT_DOOR;
            hit.index = i;
            return hit;
        }
    }
    for (int i = e->level.label_count - 1; i >= 0; i--) {
        if (rect_contains(label_world_rect(e, &e->level.labels[i]), world)) {
            hit.kind = HIT_LABEL;
            hit.index = i;
            return hit;
        }
    }
    for (int i = e->level.stairs_count - 1; i >= 0; i--) {
        if (rect_contains(level_stairs_rect(&e->level.stairs[i]), world)) {
            hit.kind = HIT_STAIRS;
            hit.index = i;
            return hit;
        }
    }
    for (int i = e->level.wall_count - 1; i >= 0; i--) {
        const Wall* w = &e->level.walls[i];
        if (point_segment_dist(world, w->a, w->b) <= tol) {
            hit.kind = HIT_WALL;
            hit.index = i;
            return hit;
        }
    }
    int room = level_room_at(&e->level, world);
    if (room >= 0) {
        hit.kind = HIT_ROOM;
        hit.index = room;
        return hit;
    }
    int shape = level_shape_at(&e->level, world);
    if (shape >= 0) {
        hit.kind = HIT_SHAPE_INTERIOR;
        hit.index = shape;
    }
    return hit;
}

static void delete_selection(Editor* e) {
    sel_prune(e);
    if (sel_count(&e->selection) == 0) return;
    Selection* s = &e->selection;
    bool wall_delete[LEVEL_MAX_WALLS] = { false };
    for (int i = 0; i < e->level.wall_count; i++) {
        if (s->walls[i]) wall_delete[i] = true;
    }
    int deleted = 0;
    for (int i = e->level.door_count - 1; i >= 0; i--) {
        int wall = level_find_wall_index(&e->level, e->level.doors[i].wall);
        bool hosted = wall >= 0 && wall_delete[wall];
        if ((s->doors[i] || hosted) && level_delete_door(&e->level, i)) deleted++;
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
        if (wall_delete[i] && level_delete_wall(&e->level, i)) deleted++;
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
    int walls, shapes, rooms, doors, labels, stairs;
} LevelCounts;

static LevelCounts level_counts(const Level* level) {
    return (LevelCounts){
        level->wall_count,
        level->shape_count,
        level->room_count,
        level->door_count,
        level->label_count,
        level->stairs_count,
    };
}

static bool counts_equal(LevelCounts a, LevelCounts b) {
    return a.walls == b.walls && a.shapes == b.shapes && a.rooms == b.rooms &&
           a.doors == b.doors && a.labels == b.labels && a.stairs == b.stairs;
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
    char target[512];
    snprintf(target, sizeof target, "%s", path);
    if (!level_save(&e->level, target, &err)) {
        snprintf(e->modal_error, sizeof e->modal_error, "%s", err.message);
        set_status(e, true, "Save failed: %s", err.message);
        return false;
    }
    snprintf(e->current_path, sizeof e->current_path, "%s", target);
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
                          bool crossed, float w) {
    float h = 22.0f;
    ImVec2 pos = igGetCursorScreenPos();
    bool pressed = igInvisibleButton(id, (ImVec2){ w, h }, 0);
    bool hovered = igIsItemHovered(0);
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 p1 = { pos.x + w, pos.y + h };
    ImDrawList_AddRectFilledEx(dl, pos, p1, fill, 3.0f, 0);
    ImU32 border = selected ? col32(ACCENT_COL) : (hovered ? col32(0x3a4048) : col32(BORDER_COL));
    ImDrawList_AddRectEx(dl, pos, p1, border, 3.0f, selected ? 2.0f : 1.0f, 0);
    if (crossed) {
        ImDrawList_AddLineEx(dl, (ImVec2){ pos.x + 4.0f, p1.y - 4.0f },
                             (ImVec2){ p1.x - 4.0f, pos.y + 4.0f },
                             col32(MUTED_COL), 1.5f, 0);
    }
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
    int guard = 0;
    while (at < len && guard++ < 4096) {
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
    /* The drawn lattice is a multiple of the snap lattice, so every line is a
       position an object can land on. It gets coarser only when the fine lines
       would smear together at this zoom. */
    float step = e->grid_size > 0.0f ? e->grid_size : 32.0f;
    while (step * e->camera.zoom < 4.0f) step *= 4.0f;
    int i0 = (int)floorf(minx / step);
    int i1 = (int)ceilf(maxx / step);
    int j0 = (int)floorf(miny / step);
    int j1 = (int)ceilf(maxy / step);
    if (i1 - i0 > 2000 || j1 - j0 > 2000) return;
    for (int i = i0; i <= i1; i++) {
        float x = i * step;
        ImVec2 a = world_to_screen(e, v2(x, miny));
        ImU32 col = col32a(0x333b45, 46);
        if (i % 4 == 0) col = col32a(0x3a4350, 120);
        if (i % 16 == 0) col = col32a(0x4a5563, 190);
        ImDrawList_AddLineVEx(dl, a.x, e->canvas.min.y, e->canvas.max.y, col, 1.0f, 0);
    }
    for (int j = j0; j <= j1; j++) {
        float y = j * step;
        ImVec2 a = world_to_screen(e, v2(minx, y));
        ImU32 col = col32a(0x333b45, 46);
        if (j % 4 == 0) col = col32a(0x3a4350, 120);
        if (j % 16 == 0) col = col32a(0x4a5563, 190);
        ImDrawList_AddLineHEx(dl, e->canvas.min.x, e->canvas.max.x, a.y, col, 1.0f, 0);
    }
}

/* ImGui's concave fill paints the bounding box for some non-convex outlines.
   Triangulating first keeps the fill inside the outline. */
static void fill_world_polygon(const Editor* e, const Vec2* pts, int count,
                               ImU32 col) {
    if (count < 3 || count > LEVEL_MAX_WALLS) return;
    Vec2 screen[LEVEL_MAX_WALLS];
    for (int i = 0; i < count; i++) {
        ImVec2 p = world_to_screen(e, pts[i]);
        screen[i] = v2(p.x, p.y);
    }
    int indices[(LEVEL_MAX_WALLS - 2) * 3];
    int triangles = polygon_triangulate(screen, count, indices, LEVEL_MAX_WALLS - 2);
    if (triangles < 0) return;
    ImDrawList* dl = igGetWindowDrawList();
    /* Adjacent anti-aliased triangles double-blend their shared edges and
       leave a visible seam, so fill the mesh without AA. */
    ImDrawList_PushDrawFlag(dl, ImDrawFlags_AAFill, false);
    for (int i = 0; i < triangles; i++) {
        ImDrawList_AddTriangleFilled(dl, v2_to_im(screen[indices[i * 3]]),
                                     v2_to_im(screen[indices[i * 3 + 1]]),
                                     v2_to_im(screen[indices[i * 3 + 2]]), col);
    }
    ImDrawList_PopDrawFlag(dl);
}

static void draw_shape_fills(const Editor* e) {
    Vec2 polygon[LEVEL_MAX_WALLS];
    /* Pass 0 paints the floor, pass 1 paints rooms and holes in creation
       order, so a hole sits on the floor and a red room can sit on a hole. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < e->level.shape_count; i++) {
            const Shape* shape = &e->level.shapes[i];
            if (!shape->closed || !shape_fill_paints(shape->fill)) continue;
            if ((pass == 0) != shape_fill_is_floor(shape->fill)) continue;
            int count = level_shape_polygon(&e->level, shape->id, polygon, LEVEL_MAX_WALLS);
            if (count < 3) continue;
            fill_world_polygon(e, polygon, count, shape_fill_color(shape->fill));
        }
    }
}

static void draw_polygon_outline(const Editor* e, const Vec2* pts, int count,
                                 ImU32 col, float thickness) {
    if (count < 2) return;
    ImVec2 screen[LEVEL_MAX_ROOM_PTS + 1];
    if (count > LEVEL_MAX_ROOM_PTS) count = LEVEL_MAX_ROOM_PTS;
    for (int i = 0; i < count; i++) screen[i] = world_to_screen(e, pts[i]);
    screen[count] = screen[0];
    ImDrawList_AddPolyline(igGetWindowDrawList(), screen, count + 1, col, thickness, 0);
}

static void draw_rooms(const Editor* e) {
    for (int i = 0; i < e->level.room_count; i++) {
        const Room* room = &e->level.rooms[i];
        if (room->count < 3) continue;
        fill_world_polygon(e, room->pts, room->count, room_color(room->color));
    }
}

/* ---- Room tool shapes ------------------------------------------------ */

static ShapeId create_shape_from_polygon(Editor* e, const Vec2* pts, int count,
                                         ShapeFill fill) {
    if (count < 3 || count > LEVEL_MAX_ROOM_PTS) return 0;
    ShapeId id = level_add_shape(&e->level);
    if (!id) return 0;
    for (int i = 0; i < count; i++) {
        if (level_add_wall_to_shape(&e->level, id, pts[i], pts[(i + 1) % count]) < 0) {
            level_delete_shape(&e->level, id);
            return 0;
        }
    }
    if (!level_close_shape(&e->level, id)) {
        level_delete_shape(&e->level, id);
        return 0;
    }
    level_set_shape_fill(&e->level, id, fill);
    return id;
}

static ShapeId create_rect_room(Editor* e, Vec2 a, Vec2 b, ShapeFill fill) {
    Vec2 pts[4] = { v2(a.x, a.y), v2(b.x, a.y), v2(b.x, b.y), v2(a.x, b.y) };
    return create_shape_from_polygon(e, pts, 4, fill);
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
        /* steps + 1 tread lines across the box: the run axis takes width/steps
           for a horizontal run and length/steps for a vertical one. The treads
           fade toward the bottom edge, which the drag started at. */
        bool horizontal = s->dir == STAIRS_H;
        float run = horizontal ? s->width : s->length;
        float spacing = run / (float)s->steps;
        for (int k = 0; k <= s->steps; k++) {
            float offset = (float)k * spacing;
            float frac = run > 0.0001f ? offset / run : 1.0f;
            if (!s->bottom) frac = 1.0f - frac;
            float alpha = 0.40f + 0.60f * frac;
            ImU32 tread = col32a(STAIR_COL, (unsigned)(alpha * 255.0f));
            if (horizontal) {
                ImVec2 p = world_to_screen(e, v2(r.min.x + offset, r.min.y));
                ImDrawList_AddLineVEx(dl, p.x, a.y, b.y, tread, 1.0f, 0);
            } else {
                ImVec2 p = world_to_screen(e, v2(r.min.x, r.min.y + offset));
                ImDrawList_AddLineHEx(dl, a.x, b.x, p.y, tread, 1.0f, 0);
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

/* An unfilled or open shape draws its lines faint, a filled closed shape
   draws them at full opacity. */
static unsigned wall_alpha_for(const Editor* e, const Wall* w) {
    const Shape* shape = level_shape_by_id_const(&e->level, w->shape);
    if (shape && shape->fill == SHAPE_FILL_EMPTY) return 70;
    if (shape && shape->closed && shape->fill != SHAPE_FILL_NONE) return 255;
    return 140;
}

static void draw_walls(const Editor* e) {
    ImDrawList* dl = igGetWindowDrawList();
    float wall_px = 2.0f;
    for (int i = 0; i < e->level.wall_count; i++) {
        const Wall* w = &e->level.walls[i];
        ImU32 wall_col = col32a(WALL_COL, wall_alpha_for(e, w));
        Span spans[LEVEL_MAX_DOORS];
        int n = wall_door_spans(e, w, spans);
        float cursor = 0.0f;
        for (int k = 0; k < n; k++) {
            if (spans[k].a > cursor) {
                draw_wall_segment(e, dl, w, cursor, spans[k].a, wall_col, wall_px);
            }
            if (spans[k].b > cursor) cursor = spans[k].b;
        }
        if (cursor < 1.0f) {
            draw_wall_segment(e, dl, w, cursor, 1.0f, wall_col, wall_px);
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
        case SEL_WALL:
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

static bool wall_selected(const Selection* s, int index) {
    return index >= 0 && index < LEVEL_MAX_WALLS && s->walls[index];
}

static bool endpoint_selected(const Editor* e, Vec2 p) {
    for (int i = 0; i < e->level.wall_count; i++) {
        if (!wall_selected(&e->selection, i)) continue;
        const Wall* w = &e->level.walls[i];
        if (v2_equal(w->a, p, 0.001f) || v2_equal(w->b, p, 0.001f)) return true;
    }
    return false;
}

static bool shape_selected(const Editor* e, ShapeId id) {
    for (int i = 0; i < e->level.wall_count; i++) {
        if (e->level.walls[i].shape == id && wall_selected(&e->selection, i)) {
            return true;
        }
    }
    return false;
}

static void add_vertex(Vec2* verts, int* count, Vec2 p) {
    if (*count >= LEVEL_MAX_WALLS * 2) return;
    for (int i = 0; i < *count; i++) {
        if (v2_equal(verts[i], p, 0.001f)) return;
    }
    verts[(*count)++] = p;
}

static void draw_selected_edges(const Editor* e) {
    ImDrawList* dl = igGetWindowDrawList();
    ImU32 accent = col32(SELECT_COL);
    ImU32 dim = col32a(SELECT_COL, 80);
    ImU32 amber = col32(VERTEX_SELECT_COL);
    for (int s = 0; s < e->level.shape_count; s++) {
        const Shape* shape = &e->level.shapes[s];
        if (!shape_selected(e, shape->id)) continue;

        int indices[LEVEL_MAX_WALLS];
        int n = level_shape_wall_indices(&e->level, shape->id, indices,
                                         LEVEL_MAX_WALLS);
        Vec2 verts[LEVEL_MAX_WALLS * 2];
        int vertex_count = 0;
        for (int k = 0; k < n; k++) {
            int index = indices[k];
            const Wall* w = &e->level.walls[index];
            bool selected = wall_selected(&e->selection, index);
            bool primary = selected && e->selection.primary.kind == SEL_WALL &&
                           e->selection.primary.index == index;
            float thickness = selected ? (primary ? 3.5f : 3.0f) : 1.5f;
            ImDrawList_AddLineEx(dl, world_to_screen(e, w->a),
                                 world_to_screen(e, w->b),
                                 selected ? accent : dim, thickness, 0);
            add_vertex(verts, &vertex_count, w->a);
            add_vertex(verts, &vertex_count, w->b);
        }
        for (int k = 0; k < vertex_count; k++) {
            bool selected = endpoint_selected(e, verts[k]);
            ImDrawList_AddCircleFilled(dl, world_to_screen(e, verts[k]),
                                       selected ? 3.6f : 2.4f,
                                       selected ? amber : dim, 12);
        }
    }
}

/* The clipped marquee selection: each span highlighted across its own wall. */
static void draw_spans(const Editor* e) {
    if (e->span_count <= 0) return;
    ImDrawList* dl = igGetWindowDrawList();
    ImU32 col = col32a(VERTEX_SELECT_COL, 240);
    for (int i = 0; i < e->span_count; i++) {
        const Wall* w = level_wall_by_id_const(&e->level, e->spans[i].wall);
        if (!w) continue;
        draw_wall_segment(e, dl, w, e->spans[i].t0, e->spans[i].t1,
                          col32a(VERTEX_SELECT_COL, 70), 10.0f);
        draw_wall_segment(e, dl, w, e->spans[i].t0, e->spans[i].t1, col, 4.0f);
    }
}

static void draw_selection(Editor* e) {
    sel_prune(e);
    draw_spans(e);
    draw_selected_edges(e);
    for (int k = SEL_WALL; k < SEL_KIND_COUNT; k++) {
        SelKind kind = (SelKind)k;
        if (kind == SEL_WALL) continue;
        int cap = sel_cap(kind);
        for (int i = 0; i < cap; i++) {
            if (!sel_has(&e->selection, kind, i)) continue;
            if (e->selection.primary.kind == kind && e->selection.primary.index == i) continue;
            draw_selection_object(e, kind, i, false);
        }
    }
    SelRef primary = e->selection.primary;
    if (primary.kind != SEL_WALL && sel_has(&e->selection, primary.kind, primary.index)) {
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
    char snap_text[32];
    if (e->snap) snprintf(snap_text, sizeof snap_text, "snap %.0f", e->grid_size);
    else snprintf(snap_text, sizeof snap_text, "snap off");
    if (e->cursor_valid) {
        snprintf(right, sizeof right, "x %.1f   y %.1f      %.0f%%   %s",
                 e->cursor_world.x, e->cursor_world.y, e->camera.zoom * 100.0f,
                 snap_text);
    } else {
        snprintf(right, sizeof right, "x --   y --      %.0f%%   %s",
                 e->camera.zoom * 100.0f, snap_text);
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
    const char* hint = TOOL_HINTS[e->tool];
    ImDrawList_AddTextImFontPtrEx(dl, font, size, (ImVec2){ x, ty },
                                  col32(SELECT_COL), tool, NULL, 0.0f, NULL);
    x += ImFont_CalcTextSizeA(font, size, FLT_MAX, 0.0f, tool).x + 2.0f;

    char rest[640];
    if (e->status_error && e->status[0]) {
        snprintf(rest, sizeof rest, "  %s", e->status);
    } else if (e->status[0]) {
        snprintf(rest, sizeof rest, "  %s   |   %s", e->status, hint);
    } else {
        snprintf(rest, sizeof rest, "  %s", hint);
    }
    ImDrawList_AddTextImFontPtrEx(dl, font, size, (ImVec2){ x, ty },
                                  col32(e->status_error ? ERROR_COL : MUTED_COL),
                                  rest, NULL, 0.0f, NULL);
    ImDrawList_PopClipRect(dl);
}

static int moved_objects(const Editor* e, int* left_behind) {
    const Selection* s = &e->selection;
    int moved = sel_count_kind(s, SEL_ROOM) + sel_count_kind(s, SEL_LABEL) +
                sel_count_kind(s, SEL_STAIRS);
    for (int i = 0; i < e->level.wall_count; i++) {
        if (e->gesture.walls[i]) moved++;
    }
    *left_behind = 0;
    for (int i = 0; i < e->level.door_count; i++) {
        if (!s->doors[i]) continue;
        int wall = level_find_wall_index(&e->level, e->level.doors[i].wall);
        if (wall >= 0 && e->gesture.walls[wall]) moved++;
        else (*left_behind)++;
    }
    return moved;
}

static void finish_move(Editor* e) {
    if (e->gesture.moved) {
        int left = 0;
        int moved = moved_objects(e, &left);
        char message[256];
        if (left > 0) {
            snprintf(message, sizeof message,
                     "Moved %d object%s; %d selected door%s stayed put",
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
    Gesture* g = &e->gesture;
    g->kind = GESTURE_MOVE;
    g->press_world = world;
    g->anchor = anchor;
    g->move_applied = v2(0.0f, 0.0f);
    g->moved = false;
    memcpy(g->walls, e->selection.walls, sizeof g->walls);
}

static void begin_reshape(Editor* e, int wall_index, Vec2 world) {
    Gesture* g = &e->gesture;
    const Wall* wall = &e->level.walls[wall_index];
    g->kind = GESTURE_RESHAPE;
    g->press_world = world;
    g->move_applied = v2(0.0f, 0.0f);
    g->moved = false;
    memcpy(g->walls, e->selection.walls, sizeof g->walls);
    g->ref_wall_x = -1;
    g->ref_wall_y = -1;
    bool vertical = wall_is_vertical(wall);
    if (vertical) {
        g->ref_wall_x = wall_index;
        g->ref_start_x = wall->a.x;
    } else {
        g->ref_wall_y = wall_index;
        g->ref_start_y = wall->a.y;
    }
    for (int i = 0; i < e->level.wall_count; i++) {
        if (!g->walls[i]) continue;
        bool v = wall_is_vertical(&e->level.walls[i]);
        if (v && g->ref_wall_x < 0) {
            g->ref_wall_x = i;
            g->ref_start_x = e->level.walls[i].a.x;
        } else if (!v && g->ref_wall_y < 0) {
            g->ref_wall_y = i;
            g->ref_start_y = e->level.walls[i].a.y;
        }
    }
}

/* Nearest stair edge within the screen-pixel grab tolerance, or
   STAIR_EDGE_NONE. Corners answer with whichever edge the press is closest to. */
static int stair_edge_at(const Editor* e, int idx, ImVec2 mouse) {
    if (idx < 0 || idx >= e->level.stairs_count) return STAIR_EDGE_NONE;
    Rect r = level_stairs_rect(&e->level.stairs[idx]);
    ImVec2 a = world_to_screen(e, r.min);
    ImVec2 b = world_to_screen(e, r.max);
    float tol = HIT_TOL_PX;
    int best = STAIR_EDGE_NONE;
    float best_dist = tol + 0.001f;
    if (mouse.y >= a.y - tol && mouse.y <= b.y + tol) {
        float d = fabsf(mouse.x - a.x);
        if (d <= tol && d < best_dist) { best = STAIR_EDGE_LEFT; best_dist = d; }
        d = fabsf(mouse.x - b.x);
        if (d <= tol && d < best_dist) { best = STAIR_EDGE_RIGHT; best_dist = d; }
    }
    if (mouse.x >= a.x - tol && mouse.x <= b.x + tol) {
        float d = fabsf(mouse.y - a.y);
        if (d <= tol && d < best_dist) { best = STAIR_EDGE_TOP; best_dist = d; }
        d = fabsf(mouse.y - b.y);
        if (d <= tol && d < best_dist) { best = STAIR_EDGE_BOTTOM; best_dist = d; }
    }
    return best;
}

/* The edge under the press moves; the opposite edge is pinned for the whole
   drag, so the box only ever grows or shrinks along one axis. */
static void begin_stairs_resize(Editor* e, int idx, int edge, Vec2 world) {
    Gesture* g = &e->gesture;
    Rect r = level_stairs_rect(&e->level.stairs[idx]);
    g->kind = GESTURE_STAIRS_RESIZE;
    g->object_index = idx;
    g->stair_edge = edge;
    g->press_world = world;
    g->move_applied = v2(0.0f, 0.0f);
    g->moved = false;
    switch (edge) {
        case STAIR_EDGE_LEFT: g->stair_fixed = r.max.x; break;
        case STAIR_EDGE_RIGHT: g->stair_fixed = r.min.x; break;
        case STAIR_EDGE_TOP: g->stair_fixed = r.max.y; break;
        case STAIR_EDGE_BOTTOM:
        default: g->stair_fixed = r.min.y; break;
    }
    set_status(e, false, "Drag the stair edge to resize");
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
        if (!segment_intersects_rect(e->level.walls[i].a, e->level.walls[i].b,
                                     rect)) {
            continue;
        }
        sel_add(e, SEL_WALL, i);
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

/* ---- Line tool ------------------------------------------------------- */

static void begin_select(Editor* e, Vec2 world, ImVec2 mouse);
static void begin_wall_click(Editor* e, Vec2 world, ImVec2 mouse);

/* Portion of an axis-aligned wall inside rect, as a t range. */
static bool clip_wall_span(const Wall* w, Rect r, float* out_t0, float* out_t1) {
    float t0 = 0.0f;
    float t1 = 1.0f;
    float dx = w->b.x - w->a.x;
    float dy = w->b.y - w->a.y;
    if (fabsf(dx) < 0.0001f) {
        if (w->a.x < r.min.x || w->a.x > r.max.x) return false;
    } else {
        float a = (r.min.x - w->a.x) / dx;
        float b = (r.max.x - w->a.x) / dx;
        if (a > b) {
            float tmp = a;
            a = b;
            b = tmp;
        }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
    }
    if (fabsf(dy) < 0.0001f) {
        if (w->a.y < r.min.y || w->a.y > r.max.y) return false;
    } else {
        float a = (r.min.y - w->a.y) / dy;
        float b = (r.max.y - w->a.y) / dy;
        if (a > b) {
            float tmp = a;
            a = b;
            b = tmp;
        }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
    }
    if (t1 - t0 <= 0.0001f) return false;
    *out_t0 = t0;
    *out_t1 = t1;
    return true;
}

/* A Shift+drag in Line mode clips every crossed wall to the dragged box. A
   box of at least one grid cell also stands on its own as a stair footprint,
   even when it touches a wall, because a stair flush with a wall is common. */
static void line_finish_marquee(Editor* e, Vec2 from, Vec2 to) {
    Rect raw = rect_from_points(from, to);
    Rect r = rect_from_points(snapped(e, raw.min), snapped(e, raw.max));
    e->span_count = 0;
    for (int i = 0; i < e->level.wall_count && e->span_count < LEVEL_MAX_WALLS; i++) {
        float t0, t1;
        if (!clip_wall_span(&e->level.walls[i], r, &t0, &t1)) continue;
        e->spans[e->span_count++] = (WallSpan){ e->level.walls[i].id, t0, t1 };
    }
    float w = r.max.x - r.min.x;
    float h = r.max.y - r.min.y;
    e->line_box = r;
    e->line_box_valid = w >= e->grid_size && h >= e->grid_size;
    e->line_drag_from = from;
    e->line_drag_to = to;
    sel_clear(e);
    if (e->line_box_valid) {
        set_status(e, false, "Box %.0f x %.0f: right-click for Stairs", w, h);
    } else if (e->span_count > 0) {
        set_status(e, false, "%d wall section%s selected", e->span_count,
                   e->span_count == 1 ? "" : "s");
    } else {
        set_status(e, false, "Marquee clipped no walls");
    }
}

/* A plain Line drag does nothing. A plain Line click draws, and a click while
   a chain is open continues it; the chain's points ride under the pending
   gesture so the release can tell the two apart. */
static void line_finish_pending(Editor* e, ImVec2 mouse, bool moved) {
    Gesture* g = &e->gesture;
    bool had_chain = g->chain_active;
    Vec2 press = g->press_world;
    if (had_chain) g->kind = GESTURE_CHAIN;
    else cancel_gesture(e);
    if (!moved) {
        begin_wall_click(e, press, mouse);
    } else if (had_chain) {
        set_status(e, false, "Line draft continues; click to add a wall");
    } else {
        set_status(e, false, "Click-click draws, Shift+drag marquees");
    }
}

static void update_drag(Editor* e, bool have_pointer) {
    ImGuiIO* io = igGetIO();
    Gesture* g = &e->gesture;
    if (!gesture_is_drag(e)) return;
    bool down = have_pointer && io->MouseDown[0];
    Vec2 world = have_pointer ? screen_to_world(e, io->MousePos) : g->anchor;

    switch (g->kind) {
        case GESTURE_MARQUEE: {
            if (!have_pointer) {
                cancel_gesture(e);
                break;
            }
            g->cur_world = world;
            if (screen_dist(io->MousePos, v2_to_im(g->press_screen)) >= 4.0f) {
                g->moved = true;
            }
            if (down) break;
            if (g->moved) {
                if (e->tool == TOOL_LINE) {
                    line_finish_marquee(e, g->press_world, g->cur_world);
                } else {
                    marquee_select(e, rect_from_points(g->press_world, g->cur_world),
                                   g->additive);
                    if (sel_count(&e->selection) == 0) {
                        Rect raw = rect_from_points(g->press_world, g->cur_world);
                        Rect box = rect_from_points(snapped(e, raw.min),
                                                    snapped(e, raw.max));
                        e->line_box = box;
                        e->line_box_valid =
                            box.max.x - box.min.x >= e->grid_size &&
                            box.max.y - box.min.y >= e->grid_size;
                        e->line_drag_from = g->press_world;
                        e->line_drag_to = g->cur_world;
                    }
                }
            } else {
                int n = sel_count(&e->selection);
                if (e->tool == TOOL_LINE) set_status(e, false, "Marquee");
                else if (n == 0) set_status(e, false, "Selection cleared");
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
                Vec2 step = v2_sub(delta, g->move_applied);
                if (fabsf(step.x) > 0.0001f || fabsf(step.y) > 0.0001f) {
                    bool moved_any = level_move_walls(&e->level, g->walls,
                                                      e->level.wall_count, step);
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
                        g->move_applied = delta;
                        g->moved = true;
                    }
                }
            }
            if (!down) {
                collapse_click(e);
                finish_move(e);
            }
            break;
        }
        case GESTURE_RESHAPE: {
            if (down) {
                Vec2 raw = v2_sub(world, g->press_world);
                Vec2 total = raw;
                if (e->snap) {
                    total.x = g->ref_wall_x >= 0
                                  ? snap_scalar(g->ref_start_x + raw.x,
                                                e->grid_size) - g->ref_start_x
                                  : 0.0f;
                    total.y = g->ref_wall_y >= 0
                                  ? snap_scalar(g->ref_start_y + raw.y,
                                                e->grid_size) - g->ref_start_y
                                  : 0.0f;
                }
                Vec2 step = v2_sub(total, g->move_applied);
                if (fabsf(step.x) > 0.0001f || fabsf(step.y) > 0.0001f) {
                    float before_x = g->ref_wall_x >= 0
                                         ? e->level.walls[g->ref_wall_x].a.x
                                         : 0.0f;
                    float before_y = g->ref_wall_y >= 0
                                         ? e->level.walls[g->ref_wall_y].a.y
                                         : 0.0f;
                    if (level_slide_edges(&e->level, g->walls,
                                          e->level.wall_count, step, 4.0f)) {
                        float after_x = g->ref_wall_x >= 0
                                            ? e->level.walls[g->ref_wall_x].a.x
                                            : 0.0f;
                        float after_y = g->ref_wall_y >= 0
                                            ? e->level.walls[g->ref_wall_y].a.y
                                            : 0.0f;
                        g->move_applied = v2_add(g->move_applied,
                                                 v2(after_x - before_x,
                                                    after_y - before_y));
                        g->moved = true;
                    }
                }
            }
            if (!down) {
                if (g->moved) commit(e, "Reshaped");
                else collapse_click(e);
                cancel_gesture(e);
            }
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
                else collapse_click(e);
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
                else collapse_click(e);
                cancel_gesture(e);
            }
            break;
        }
        case GESTURE_STAIRS_RESIZE: {
            int idx = g->object_index;
            if (idx < 0 || idx >= e->level.stairs_count) {
                cancel_gesture(e);
                break;
            }
            if (down) {
                Rect before = level_stairs_rect(&e->level.stairs[idx]);
                Rect next = before;
                /* Snap on keeps every edge on the grid, so the minimum is one
                   whole cell even when that is larger than STAIR_MIN_SIZE. */
                float min_size = e->snap ? fmaxf(STAIR_MIN_SIZE, e->grid_size)
                                         : STAIR_MIN_SIZE;
                if (g->stair_edge == STAIR_EDGE_LEFT ||
                    g->stair_edge == STAIR_EDGE_RIGHT) {
                    float x = e->snap ? snap_scalar(world.x, e->grid_size) : world.x;
                    if (g->stair_edge == STAIR_EDGE_LEFT) {
                        next.min.x = fminf(x, g->stair_fixed - min_size);
                    } else {
                        next.max.x = fmaxf(x, g->stair_fixed + min_size);
                    }
                } else if (g->stair_edge == STAIR_EDGE_TOP ||
                           g->stair_edge == STAIR_EDGE_BOTTOM) {
                    float y = e->snap ? snap_scalar(world.y, e->grid_size) : world.y;
                    if (g->stair_edge == STAIR_EDGE_TOP) {
                        next.min.y = fminf(y, g->stair_fixed - min_size);
                    } else {
                        next.max.y = fmaxf(y, g->stair_fixed + min_size);
                    }
                }
                if (level_set_stairs_rect(&e->level, idx, next)) {
                    Rect after = level_stairs_rect(&e->level.stairs[idx]);
                    if (fabsf(after.min.x - before.min.x) > 0.0001f ||
                        fabsf(after.min.y - before.min.y) > 0.0001f ||
                        fabsf(after.max.x - before.max.x) > 0.0001f ||
                        fabsf(after.max.y - before.max.y) > 0.0001f) {
                        g->moved = true;
                    }
                }
            }
            if (!down) {
                if (g->moved) commit(e, "Stairs resized");
                else collapse_click(e);
                cancel_gesture(e);
            }
            break;
        }
        case GESTURE_LINE_PENDING: {
            if (!have_pointer) {
                cancel_gesture(e);
                break;
            }
            g->cur_world = world;
            if (screen_dist(io->MousePos, v2_to_im(g->press_screen)) >= 4.0f) {
                g->moved = true;
            }
            if (!down) line_finish_pending(e, io->MousePos, g->moved);
            break;
        }
        case GESTURE_NONE:
        case GESTURE_CHAIN:
        default: break;
    }
}

static void close_wall_chain(Editor* e, bool via_enter) {
    Gesture* g = &e->gesture;
    if (g->kind != GESTURE_CHAIN) return;
    if (g->point_count < 3) {
        if (via_enter) set_status(e, true, "A closed shape needs at least four walls");
        return;
    }
    Vec2 first = g->pts[0];
    Vec2 last = g->pts[g->point_count - 1];
    bool same_x = fabsf(last.x - first.x) <= 0.001f;
    bool same_y = fabsf(last.y - first.y) <= 0.001f;
    int closing_walls = same_x && same_y ? 0 : (same_x || same_y ? 1 : 2);
    if (g->wall_count + closing_walls < 4) {
        set_status(e, true, "A closed shape needs at least four walls");
        return;
    }
    if (e->level.wall_count + closing_walls > LEVEL_MAX_WALLS) {
        set_status(e, true, "Cannot close: wall limit reached");
        return;
    }

    if (closing_walls == 1) {
        if (level_add_wall_to_shape(&e->level, g->shape, last, first) < 0) {
            set_status(e, true, "Closing segment must be orthogonal and non-zero");
            return;
        }
    } else if (closing_walls == 2) {
        /* The last click is not axis-aligned with the first. Close with an
           L-shaped pair so the loop still closes with orthogonal walls. */
        Vec2 prev = g->pts[g->point_count - 2];
        bool last_horizontal = fabsf(last.y - prev.y) <= 0.001f;
        Vec2 corner = last_horizontal ? v2(last.x, first.y) : v2(first.x, last.y);
        if (level_add_wall_to_shape(&e->level, g->shape, last, corner) < 0) {
            set_status(e, true, "Closing segment must be orthogonal and non-zero");
            return;
        }
        if (level_add_wall_to_shape(&e->level, g->shape, corner, first) < 0) {
            level_delete_wall(&e->level, e->level.wall_count - 1);
            set_status(e, true, "Closing segment must be orthogonal and non-zero");
            return;
        }
    }

    if (!level_close_shape(&e->level, g->shape)) {
        for (int i = 0; i < closing_walls; i++) {
            level_delete_wall(&e->level, e->level.wall_count - 1);
        }
        set_status(e, true, "A closed shape needs at least four connected walls");
        cancel_gesture(e);
        return;
    }
    level_set_shape_fill(&e->level, g->shape, e->shape_fill);

    cancel_gesture(e);
    commit(e, "Shape closed");
}

static void cancel_transient(Editor* e) {
    if (e->gesture.kind == GESTURE_LINE_PENDING) {
        cancel_gesture(e);
        set_status(e, false, "Cancelled");
    } else if (e->gesture.kind == GESTURE_MARQUEE) {
        cancel_gesture(e);
        set_status(e, false, "Marquee cancelled");
    } else if (e->gesture.kind == GESTURE_CHAIN) {
        discard_empty_chain_shape(e);
        cancel_gesture(e);
        set_status(e, false, "Draft cancelled");
    } else if (gesture_is_drag(e)) {
        bool restored = history_restore_current(&e->history, &e->level);
        sel_prune(e);
        cancel_gesture(e);
        set_status(e, false, restored ? "Drag cancelled" : "Drag cancelled, no history");
    }
}

/* ---- Room tool -------------------------------------------------------- */

static void begin_room_drag(Editor* e, Vec2 world, ImVec2 mouse) {
    ImGuiIO* io = igGetIO();
    Gesture* g = &e->gesture;
    cancel_gesture(e);
    g->kind = GESTURE_ROOM_DRAW;
    g->press_world = snapped(e, world);
    g->cur_world = g->press_world;
    g->press_screen = v2(mouse.x, mouse.y);
    if (io->KeyAlt) {
        int idx = level_shape_at(&e->level, world);
        if (idx >= 0 && e->level.shapes[idx].closed) {
            Vec2 poly[LEVEL_MAX_WALLS];
            int n = level_shape_polygon(&e->level, e->level.shapes[idx].id, poly,
                                        LEVEL_MAX_WALLS);
            if (n >= 3 && n <= LEVEL_MAX_ROOM_PTS) {
                memcpy(g->pts, poly, (size_t)n * sizeof(Vec2));
                g->point_count = n;
                g->shape = e->level.shapes[idx].id;
                g->kind = GESTURE_ROOM_COPY;
                set_status(e, false, "Alt+drag: copy room");
                return;
            }
        }
    }
    set_status(e, false, "Drag a rectangle room");
}

static void finish_room_drag(Editor* e, bool have_pointer) {
    Gesture* g = &e->gesture;
    if (g->kind != GESTURE_ROOM_DRAW && g->kind != GESTURE_ROOM_COPY) return;
    if (have_pointer) {
        g->cur_world = snapped(e, screen_to_world(e, igGetIO()->MousePos));
    }
    Vec2 a = snapped(e, g->press_world);
    Vec2 b = snapped(e, g->cur_world);
    float min_size = e->snap ? e->grid_size : 1.0f;
    if (g->kind == GESTURE_ROOM_DRAW) {
        if (fabsf(b.x - a.x) < min_size || fabsf(b.y - a.y) < min_size) {
            set_status(e, true, "Rectangle needs two grid points");
            cancel_gesture(e);
            return;
        }
        Rect r = rect_from_points(a, b);
        ShapeId id = create_rect_room(e, r.min, r.max, e->shape_fill);
        if (id) commit(e, "Rectangle room added");
        else set_status(e, true, "Could not add the room");
    } else {
        Shape* src = level_shape_by_id(&e->level, g->shape);
        ShapeFill fill = src ? src->fill : e->shape_fill;
        Vec2 delta = v2_sub(b, a);
        if (fabsf(delta.x) < min_size && fabsf(delta.y) < min_size) {
            set_status(e, true, "Drag to place the copy");
            cancel_gesture(e);
            return;
        }
        Vec2 moved[LEVEL_MAX_ROOM_PTS];
        for (int i = 0; i < g->point_count; i++) {
            moved[i] = v2_add(g->pts[i], delta);
        }
        ShapeId id = create_shape_from_polygon(e, moved, g->point_count, fill);
        if (id) commit(e, "Room duplicated");
        else set_status(e, true, "Could not duplicate the room");
    }
    cancel_gesture(e);
}

static void update_room_drag(Editor* e, bool have_pointer) {
    Gesture* g = &e->gesture;
    if (g->kind != GESTURE_ROOM_DRAW && g->kind != GESTURE_ROOM_COPY) return;
    bool down = have_pointer && igGetIO()->MouseDown[0];
    if (down) {
        if (have_pointer) {
            g->cur_world = snapped(e, screen_to_world(e, igGetIO()->MousePos));
        }
        return;
    }
    finish_room_drag(e, have_pointer);
}

static void draw_room_preview(const Editor* e) {
    const Gesture* g = &e->gesture;
    if (g->kind != GESTURE_ROOM_DRAW && g->kind != GESTURE_ROOM_COPY) return;
    ImDrawList* dl = igGetWindowDrawList();
    if (g->kind == GESTURE_ROOM_DRAW) {
        Rect r = rect_from_points(g->press_world, g->cur_world);
        ImVec2 a = world_to_screen(e, r.min);
        ImVec2 b = world_to_screen(e, r.max);
        ImDrawList_AddRectFilledEx(dl, a, b, col32a(0x5aa0c8, 34), 0.0f, 0);
        ImDrawList_AddRectEx(dl, a, b, col32(ACCENT_COL), 0.0f, 1.0f, 0);
    } else {
        Vec2 moved[LEVEL_MAX_ROOM_PTS];
        Vec2 delta = v2_sub(g->cur_world, g->press_world);
        for (int i = 0; i < g->point_count; i++) moved[i] = v2_add(g->pts[i], delta);
        draw_polygon_outline(e, moved, g->point_count, col32(ACCENT_COL), 1.0f);
    }
}

static void begin_select(Editor* e, Vec2 world, ImVec2 mouse) {
    ImGuiIO* io = igGetIO();
    bool shift = io->KeyShift;
    bool ctrl = io->KeyCtrl;
    Hit hit = hit_test(e, world);
    if (hit.kind == HIT_NONE) {
        /* A press just outside a stair box still grabs its edge, the way a wall
           line is grabbable from either side. */
        if (!shift && !ctrl) {
            for (int i = e->level.stairs_count - 1; i >= 0; i--) {
                int edge = stair_edge_at(e, i, mouse);
                if (edge == STAIR_EDGE_NONE) continue;
                sel_only(e, SEL_STAIRS, i);
                cancel_gesture(e);
                begin_stairs_resize(e, i, edge, world);
                return;
            }
        }
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

    Target target;
    target_from_hit(e, hit, &target);
    if (target.kind == SEL_NONE ||
        (target.kind == SEL_WALL && target.wall_count <= 0)) {
        cancel_gesture(e);
        set_status(e, true, "That object has no selectable contents");
        return;
    }

    bool already = target_selected(e, &target);
    if (ctrl) {
        cancel_gesture(e);
        target_toggle(e, &target);
        if (target.index >= 0) sel_set_primary(e, target.kind, target.index);
        int n = sel_count(&e->selection);
        if (n == 0) set_status(e, false, "Selection cleared");
        else set_status(e, false, "%d object%s selected", n, n == 1 ? "" : "s");
        return;
    }
    if (shift) {
        cancel_gesture(e);
        target_add(e, &target);
        if (target.index >= 0) sel_set_primary(e, target.kind, target.index);
        int n = sel_count(&e->selection);
        set_status(e, false, "%d object%s selected", n, n == 1 ? "" : "s");
        return;
    }

    bool in_group = sel_count(&e->selection) > 1 && already;
    if (in_group) {
        if (target.index >= 0) sel_set_primary(e, target.kind, target.index);
    } else {
        target_only(e, &target);
        if (target.index >= 0) sel_set_primary(e, target.kind, target.index);
    }
    cancel_gesture(e);
    e->gesture.collapse = target;
    e->gesture.collapse_on_release = in_group;

    if (target.kind == SEL_WALL) {
        int edges = sel_count_kind(&e->selection, SEL_WALL);
        bool only_walls = edges == sel_count(&e->selection);
        if (only_walls && !selection_walls_whole_shape(e)) {
            begin_reshape(e, target.index, world);
            if (edges == 1) set_status(e, false, "Edge selected");
            else set_status(e, false, "%d edges selected", edges);
            return;
        }
        Vec2 anchor;
        if (!primary_anchor(e, &anchor)) return;
        begin_move(e, world, anchor);
        if (edges == sel_count(&e->selection)) {
            set_status(e, false, "%d edges selected", edges);
        } else {
            set_status(e, false, "%d objects selected", sel_count(&e->selection));
        }
        return;
    }

    if (target.kind == SEL_DOOR) {
        int door_index = target.index;
        Door* d = &e->level.doors[door_index];
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
        e->gesture.object_index = door_index;
        e->gesture.moved = false;
        set_status(e, false, "Door %d selected", door_index + 1);
        return;
    }

    if (target.kind == SEL_STAIRS && !in_group) {
        int edge = stair_edge_at(e, target.index, mouse);
        if (edge != STAIR_EDGE_NONE) {
            begin_stairs_resize(e, target.index, edge, world);
            return;
        }
    }

    Vec2 anchor;
    if (!primary_anchor(e, &anchor)) return;
    begin_move(e, world, anchor);
    set_status(e, false, "%s selected",
               target.kind == SEL_ROOM ? "Room" :
               target.kind == SEL_LABEL ? "Label" : "Stairs");
}

static void begin_wall_click(Editor* e, Vec2 world, ImVec2 mouse) {
    Gesture* g = &e->gesture;
    if (g->kind != GESTURE_CHAIN) {
        cancel_gesture(e);
        sel_clear(e);
        ShapeId shape = level_add_shape(&e->level);
        if (shape == 0) {
            set_status(e, true, "Could not start a shape: level is full");
            return;
        }
        g = &e->gesture;
        g->kind = GESTURE_CHAIN;
        g->shape = shape;
        g->point_count = 1;
        g->pts[0] = snapped(e, world);
        g->wall_count = 0;
        set_status(e, false, "Shape started");
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
    int wall = level_add_wall_to_shape(&e->level, g->shape,
                                       g->pts[g->point_count - 1], next);
    if (wall < 0) {
        set_status(e, true, "Could not add wall (capacity or invalid segment)");
        return;
    }
    g->wall_count++;
    g->pts[g->point_count++] = next;
    commit(e, "Wall added");
}

static void begin_label_click(Editor* e, Vec2 world) {
    Vec2 p = snapped(e, world);
    int idx = level_add_label(&e->level, p, "Room", e->label_size);
    if (idx < 0) {
        set_status(e, true, "Could not add label: level is full");
        return;
    }
    sel_only(e, SEL_LABEL, idx);
    e->focus_label_input = true;
    commit(e, "Label added");
}

/* A Line press either starts a marquee (Shift), continues an open chain, or
   waits for the release so a plain drag stays a no-op. */
static void line_press(Editor* e, Vec2 world, ImVec2 mouse) {
    ImGuiIO* io = igGetIO();
    Gesture* g = &e->gesture;
    bool had_chain = g->kind == GESTURE_CHAIN;
    e->span_count = 0;
    e->line_box_valid = false;

    if (io->KeyShift) {
        if (had_chain) discard_empty_chain_shape(e);
        cancel_gesture(e);
        g = &e->gesture;
        g->kind = GESTURE_MARQUEE;
        g->press_world = world;
        g->cur_world = world;
        g->press_screen = v2(mouse.x, mouse.y);
        g->additive = false;
        g->moved = false;
        set_status(e, false, "Marquee");
        return;
    }

    if (!had_chain) cancel_gesture(e);
    g = &e->gesture;
    g->kind = GESTURE_LINE_PENDING;
    g->press_world = world;
    g->cur_world = world;
    g->press_screen = v2(mouse.x, mouse.y);
    g->moved = false;
    g->chain_active = had_chain;
    set_status(e, false, had_chain ? "Press" : "Press: click draws");
}

/* The right-click snapshot. Both tools feed one shape target, one set of wall
   spans, and one stair box; the popup then cannot read stale selection state. */
static void open_context_menu(Editor* e, Vec2 world) {
    ContextMenu* c = &e->ctx;
    memset(c, 0, sizeof *c);
    int shape = level_shape_at(&e->level, world);
    c->fill_shape = shape >= 0 ? e->level.shapes[shape].id : 0;

    if (e->tool == TOOL_LINE) {
        for (int i = 0; i < e->span_count; i++) {
            if (level_wall_by_id_const(&e->level, e->spans[i].wall)) {
                c->door_spans[c->door_span_count++] = e->spans[i];
            }
        }
        c->can_fill = c->fill_shape != 0 && c->door_span_count == 0;
        c->can_stairs = e->line_box_valid;
        c->stairs_rect = e->line_box;
        c->drag_from = e->line_drag_from;
        c->drag_to = e->line_drag_to;
    } else {
        for (int i = 0; i < e->level.wall_count; i++) {
            if (!sel_has(&e->selection, SEL_WALL, i)) continue;
            c->door_spans[c->door_span_count++] =
                (WallSpan){ e->level.walls[i].id, 0.0f, 1.0f };
        }
        if (c->door_span_count == 0) {
            float t = 0.0f;
            int wall = level_nearest_wall(&e->level, world,
                                          HIT_TOL_PX / e->camera.zoom, &t);
            if (wall >= 0) {
                c->door_spans[c->door_span_count++] =
                    (WallSpan){ e->level.walls[wall].id, 0.0f, 1.0f };
            }
        }
        c->can_fill = c->fill_shape != 0;
        c->can_stairs = e->line_box_valid && c->door_span_count == 0;
        c->stairs_rect = e->line_box;
        c->drag_from = e->line_drag_from;
        c->drag_to = e->line_drag_to;
    }
    c->open = true;
}

/* Applies a palette fill to the closed shape under the last right-click, and
   makes it the current choice so the next shape starts the same way. */
static void context_set_mode(Editor* e, ShapeFill fill) {
    Shape* shape = level_shape_by_id(&e->level, e->ctx.fill_shape);
    if (!shape) {
        set_status(e, true, "No closed shape under the click");
        return;
    }
    level_set_shape_fill(&e->level, shape->id, fill);
    e->shape_fill = fill;
    commit(e, "Room mode changed");
}

/* A thin door marquee can cross a short collinear wall that ends inside its
   span, such as a painted fill border painted over the same line. The door
   belongs to the longer span it already covers, so a contained span is
   skipped rather than adding a second, tiny door. */
static bool span_contains(const Wall* outer, float o0, float o1,
                          const Wall* inner, float i0, float i1) {
    Vec2 oa = segment_point(outer->a, outer->b, o0);
    Vec2 ob = segment_point(outer->a, outer->b, o1);
    Vec2 ia = segment_point(inner->a, inner->b, i0);
    Vec2 ib = segment_point(inner->a, inner->b, i1);
    return point_segment_dist(ia, oa, ob) <= 0.5f &&
           point_segment_dist(ib, oa, ob) <= 0.5f;
}

static void context_add_doors(Editor* e, DoorColor color) {
    ContextMenu* c = &e->ctx;
    int added = 0;
    for (int i = 0; i < c->door_span_count; i++) {
        const Wall* w = level_wall_by_id_const(&e->level, c->door_spans[i].wall);
        if (!w) continue;
        float len = wall_length(w);
        if (len <= 0.0001f) continue;
        bool covered = false;
        for (int j = 0; j < i; j++) {
            const Wall* prev =
                level_wall_by_id_const(&e->level, c->door_spans[j].wall);
            if (!prev) continue;
            if (span_contains(prev, c->door_spans[j].t0, c->door_spans[j].t1,
                              w, c->door_spans[i].t0, c->door_spans[i].t1)) {
                covered = true;
                break;
            }
        }
        if (covered) continue;
        float t = (c->door_spans[i].t0 + c->door_spans[i].t1) * 0.5f;
        float width = (c->door_spans[i].t1 - c->door_spans[i].t0) * len;
        /* level_add_door clamps to the wall and raises the width to the
           minimum, so the span only needs its centre and length. */
        if (level_add_door(&e->level, w->id, t, width, color) >= 0) added++;
    }
    e->span_count = 0;
    e->line_box_valid = false;
    if (added > 0) commit(e, "Door added");
    else set_status(e, true, "No wall section to door");
}

static void context_add_stairs(Editor* e) {
    ContextMenu* c = &e->ctx;
    Rect r = c->stairs_rect;
    float dx = c->drag_to.x - c->drag_from.x;
    float dy = c->drag_to.y - c->drag_from.y;
    StairsDir dir = fabsf(dy) >= fabsf(dx) ? STAIRS_V : STAIRS_H;
    float run = dir == STAIRS_H ? r.max.x - r.min.x : r.max.y - r.min.y;
    int steps = (int)lroundf(run / STAIR_STEP_DEPTH);
    if (steps < STAIR_MIN_STEPS) steps = STAIR_MIN_STEPS;
    if (steps > STAIR_MAX_STEPS) steps = STAIR_MAX_STEPS;
    int idx = level_add_stairs(&e->level, r, dir, steps);
    if (idx < 0) {
        set_status(e, true, "Could not add stairs: box is invalid");
        return;
    }
    /* The drag starts at the bottom edge. */
    e->level.stairs[idx].bottom = dir == STAIRS_H ? dx >= 0.0f : dy >= 0.0f;
    sel_only(e, SEL_STAIRS, idx);
    e->span_count = 0;
    e->line_box_valid = false;
    commit(e, "Stairs added");
}

static void context_menu(Editor* e) {
    ContextMenu* c = &e->ctx;
    if (c->door_span_count > 0 && igBeginMenu("Door")) {
        if (igMenuItemEx("White", NULL, false, true)) {
            context_add_doors(e, DOOR_WHITE);
            igCloseCurrentPopup();
        }
        if (igMenuItemEx("Red", NULL, false, true)) {
            context_add_doors(e, DOOR_RED);
            igCloseCurrentPopup();
        }
        if (igMenuItemEx("Blue", NULL, false, true)) {
            context_add_doors(e, DOOR_BLUE);
            igCloseCurrentPopup();
        }
        igEndMenu();
    }
    if (c->can_fill && igBeginMenu("Mode")) {
        for (int i = 0; i < FILL_CHOICE_COUNT; i++) {
            if (igMenuItemEx(FILL_CHOICES[i].label, NULL, false, true)) {
                context_set_mode(e, FILL_CHOICES[i].fill);
                igCloseCurrentPopup();
            }
        }
        igEndMenu();
    }
    if (c->can_stairs && igMenuItemEx("Stairs", NULL, false, true)) {
        context_add_stairs(e);
        igCloseCurrentPopup();
    }
    if (c->door_span_count > 0 || c->can_fill || c->can_stairs) {
        igSeparator();
    }
    if (igMenuItemEx("Clear selection", NULL, false, true)) {
        sel_clear(e);
        e->span_count = 0;
        e->line_box_valid = false;
        set_status(e, false, "Selection cleared");
        igCloseCurrentPopup();
    }
}

static void handle_canvas_input(Editor* e, bool hovered, bool have_pointer) {
    ImGuiIO* io = igGetIO();
    if (e->modal != MODAL_NONE) return;
    if (!have_pointer) {
        /* Nothing to hover or drag toward, but a drag that was under way still
           has to end at its last applied position. */
        update_drag(e, false);
        e->cursor_valid = false;
        return;
    }

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

    update_drag(e, true);
    update_room_drag(e, have_pointer);

    if (hovered && igIsMouseClicked(1)) {
        if (e->tool == TOOL_LABEL) cancel_transient(e);
        else open_context_menu(e, world);
        return;
    }
    if (hovered && igIsMouseClicked(0) && !e->pan_active) {
        switch (e->tool) {
            case TOOL_SELECT: begin_select(e, world, io->MousePos); break;
            case TOOL_LINE: line_press(e, world, io->MousePos); break;
            case TOOL_LABEL: begin_label_click(e, world); break;
            case TOOL_ROOM: begin_room_drag(e, world, io->MousePos); break;
            default: break;
        }
    }
}

static void draw_canvas(Editor* e, ImVec2 mouse, bool have_pointer) {
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
    bool hovered = have_pointer && igIsItemHovered(0);
    bool inside = have_pointer && rect_contains(e->canvas, v2(mouse.x, mouse.y));
    if (e->fit_pending) {
        fit_camera(e);
        e->fit_pending = false;
    }

    handle_canvas_input(e, hovered && inside, have_pointer);

    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_AddRectFilledEx(dl, v2_to_im(e->canvas.min), v2_to_im(e->canvas.max),
                               col32(CANVAS_BG), 0.0f, 0);
    draw_grid(e);
    draw_shape_fills(e);
    draw_rooms(e);
    draw_room_preview(e);
    draw_stairs(e);
    draw_walls(e);
    draw_doors(e);
    draw_labels(e);

    if (have_pointer && e->gesture.kind == GESTURE_CHAIN) draw_chain_preview(e, mouse);
    draw_selection(e);
    draw_marquee(e);
    draw_status_strip(e);

    if (e->ctx.open) {
        igOpenPopup("##canvas_ctx", 0);
        e->ctx.open = false;
    }
    if (igBeginPopup("##canvas_ctx", 0)) {
        context_menu(e);
        igEndPopup();
    }

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

/* One palette for the whole editor. The left panel sets the fill a new shape
   or rectangle takes; the inspector sets the selected shape's fill. Both draw
   the same table, so the choices cannot drift. Returns SHAPE_FILL_NONE when
   nothing was clicked. */
static ShapeFill fill_palette(ShapeFill current) {
    float half = (igGetContentRegionAvail().x - 8.0f) * 0.5f;
    ShapeFill chosen = SHAPE_FILL_NONE;
    for (int i = 0; i < FILL_CHOICE_COUNT; i++) {
        const FillChoice* choice = &FILL_CHOICES[i];
        if (swatch_button(choice->id, choice->label, col32(choice->rgb),
                          current == choice->fill, choice->crossed, half)) {
            chosen = choice->fill;
        }
        if (i % 2 == 0) igSameLineEx(0.0f, 8.0f);
    }
    return chosen;
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

    igDummy((ImVec2){ 0.0f, 4.0f });
    heading("Fill");
    ShapeFill chosen = fill_palette(e->shape_fill);
    if (chosen != SHAPE_FILL_NONE) e->shape_fill = chosen;
    small_note("Hole cuts to the canvas. Floor paints under the rooms.");

    switch (e->tool) {
        case TOOL_LINE:
            igDummy((ImVec2){ 0.0f, 3.0f });
            small_note("A closed chain takes the Fill choice.");
            break;
        case TOOL_LABEL: {
            igDummy((ImVec2){ 0.0f, 3.0f });
            igSetNextItemWidth(120.0f);
            igSliderFloatEx("Label size", &e->label_size, 8.0f, 48.0f, "%.1f", 0);
            small_note("Click the canvas to place a label, then type its text.");
            break;
        }
        case TOOL_SELECT:
            igDummy((ImVec2){ 0.0f, 3.0f });
            small_note("Click an object to select it. Shift adds, Ctrl toggles.");
            small_note("Drag a wall edge to reshape it; drag a group to move it.");
            small_note("Drag empty canvas to marquee. Drag a stair edge to resize.");
            small_note("Right-click for Mode, Door, or Stairs.");
            break;
        case TOOL_ROOM:
            igDummy((ImVec2){ 0.0f, 3.0f });
            small_note("Drag an axis-aligned rectangle to add a room.");
            small_note("Alt+drag a room to duplicate it. Right-click one for Mode.");
            break;
        case TOOL_COUNT:
        default: break;
    }

    if (e->tool == TOOL_LINE) {
        igDummy((ImVec2){ 0.0f, 6.0f });
        small_note("Right-click: Fill inside a closed shape, Door over a marquee-clipped wall, Stairs over a dragged box.");
        small_note("Enter closes the line. Esc ends it open.");
    }
    igEnd();
}

static void object_delete_button(Editor* e) {
    igPushStyleColorImVec4(ImGuiCol_Button, v4c(0x4a2028, 1.0f));
    igPushStyleColorImVec4(ImGuiCol_ButtonHovered, v4c(0x5c2832, 1.0f));
    igPushStyleColorImVec4(ImGuiCol_ButtonActive, v4c(0x6d303b, 1.0f));
    if (igButtonEx("Delete", (ImVec2){ -FLT_MIN, 28.0f })) delete_selection(e);
    igPopStyleColorEx(3);
}

static void inspector_edge(Editor* e, int idx) {
    if (idx < 0 || idx >= e->level.wall_count) return;
    const Wall* wall = &e->level.walls[idx];
    Shape* shape = level_shape_by_id(&e->level, wall->shape);
    if (!shape) return;
    int indices[LEVEL_MAX_WALLS];
    int walls = level_shape_wall_indices(&e->level, shape->id, indices,
                                         LEVEL_MAX_WALLS);
    int edge = 0;
    for (int i = 0; i < walls; i++) {
        if (indices[i] == idx) {
            edge = i + 1;
            break;
        }
    }
    heading("Wall shape");
    igText("Shape %u", (unsigned)shape->id);
    igText("%s", shape->closed ? "Closed" : "Open");
    igText("Walls %d", walls);
    igTextDisabled("Fill");
    ShapeFill chosen = fill_palette(shape->fill);
    if (chosen != SHAPE_FILL_NONE) {
        level_set_shape_fill(&e->level, shape->id, chosen);
        e->shape_fill = chosen;
        commit(e, "Shape fill changed");
    }
    igText("Edge %d of %d", edge, walls);
    igDummy((ImVec2){ 0.0f, 4.0f });
    object_delete_button(e);
}

static void inspector_room(Editor* e, int idx) {
    Room* room = &e->level.rooms[idx];
    heading("Room");
    igText("Room %d of %d", idx + 1, e->level.room_count);
    igTextDisabled("Room color");
    bool red = room->color == ROOM_RED;
    float half = (igGetContentRegionAvail().x - 6.0f) * 0.5f;
    if (swatch_button("##roomblue", "Blue", col32(ROOM_BLUE_COL), !red, false, half)) {
        level_set_room_color(&e->level, idx, ROOM_BLUE);
        commit(e, "Room color changed");
    }
    igSameLineEx(0.0f, 6.0f);
    if (swatch_button("##roomred", "Red", col32(ROOM_RED_COL), red, false, half)) {
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
    if (swatch_button("##doorwhite", "White", col32(DOOR_WHITE_COL), white, false, third)) {
        level_set_door_color(&e->level, idx, DOOR_WHITE);
        commit(e, "Door color changed");
    }
    igSameLineEx(0.0f, 6.0f);
    if (swatch_button("##doorred", "Red", col32(DOOR_RED_COL), red, false, third)) {
        level_set_door_color(&e->level, idx, DOOR_RED);
        commit(e, "Door color changed");
    }
    igSameLineEx(0.0f, 6.0f);
    if (swatch_button("##doorblue", "Blue", col32(DOOR_BLUE_COL), blue, false, third)) {
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
    igText("Width   %.1f", s->width);
    igText("Length  %.1f", s->length);
    igText("Position %.1f, %.1f", s->pos.x, s->pos.y);
    object_delete_button(e);
}

static void inspector_many(Editor* e) {
    Selection* s = &e->selection;
    heading("Selection");
    igText("%d objects selected", sel_count(s));
    igDummy((ImVec2){ 0.0f, 4.0f });
    if (sel_count_kind(s, SEL_WALL)) igText("Edges   %d", sel_count_kind(s, SEL_WALL));
    if (sel_count_kind(s, SEL_ROOM)) igText("Rooms   %d", sel_count_kind(s, SEL_ROOM));
    if (sel_count_kind(s, SEL_DOOR)) igText("Doors   %d", sel_count_kind(s, SEL_DOOR));
    if (sel_count_kind(s, SEL_LABEL)) igText("Labels  %d", sel_count_kind(s, SEL_LABEL));
    if (sel_count_kind(s, SEL_STAIRS)) igText("Stairs  %d", sel_count_kind(s, SEL_STAIRS));
    igDummy((ImVec2){ 0.0f, 6.0f });
    small_note("Walls and doors move with the group. Drag an edge to reshape the selected edges.");
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
            case SEL_WALL: inspector_edge(e, single.index); break;
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
        igText("Shapes  %d", e->level.shape_count);
        igText("Rooms   %d", e->level.room_count);
        igText("Doors   %d", e->level.door_count);
        igText("Labels  %d", e->level.label_count);
        igText("Stairs  %d", e->level.stairs_count);
        igDummy((ImVec2){ 0.0f, 4.0f });
        heading("Controls");
        igPushFontFloat(igGetFont(), 12.0f);
        igTextDisabled("1 Select, 2 Line, 3 Label");
        igTextDisabled("F fit level, G toggle grid");
        igTextDisabled("Shift-click adds; Ctrl-click toggles");
        igTextDisabled("Drag empty canvas to marquee");
        igTextDisabled("Drag selected edges to reshape them");
        igTextDisabled("Drag a stair edge to resize its footprint");
        igTextDisabled("Drag a group to move walls, doors, rooms, labels, or stairs");
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
    igText("1 Select   2 Line   3 Label");
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
    igText("Drag selected edges to reshape them.");
    igBullet();
    igText("Drag a group to move its walls, doors, rooms, labels, and stairs.");
    igBullet();
    igText("Drag a door to slide it; drag an end to resize.");
    igBullet();
    igText("Drag a stair edge to resize its footprint.");
    igBullet();
    igText("A closed wall shape fills with the Wall tool color.");
    igBullet();
    igText("Fill None leaves a closed shape unfilled.");
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
            int indices[LEVEL_MAX_WALLS];
            int n = g->wall_count > 0
                        ? level_shape_wall_indices(&e->level, g->shape, indices,
                                                   LEVEL_MAX_WALLS)
                        : 0;
            if (n > 0 && level_delete_wall(&e->level, indices[n - 1])) {
                g->wall_count--;
                if (g->point_count > 1) g->point_count--;
                commit(e, "Last chain wall removed");
            }
            if (g->wall_count == 0) {
                discard_empty_chain_shape(e);
                cancel_gesture(e);
                set_status(e, false, "Chain cleared");
            }
        }
    }
    if (igIsKeyPressedEx(ImGuiKey_Enter, false)) {
        if (e->gesture.kind == GESTURE_CHAIN) close_wall_chain(e, true);
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
    e->tool = TOOL_LINE;
    e->selection.primary.kind = SEL_NONE;
    e->selection.primary.index = -1;
    e->grid_size = 8.0f;
    e->snap = true;
    e->show_grid = true;
    /* Floor Blue is the base of the reference map; the palette changes it. */
    e->shape_fill = SHAPE_FILL_FLOOR_BLUE;
    e->label_size = 10.0f;
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
    draw_canvas(e, io->MousePos, mouse_available(io->MousePos));
    handle_shortcuts(e);
    draw_modals(e);
}
