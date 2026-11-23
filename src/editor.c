#include "editor.h"

#include "cimgui.h"
#include "geom.h"
#include "level.h"
#include "nav.h"
#include "player_sprite.h"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_imgui.h"

#include <dirent.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* PNG, JPEG, BMP, TGA, GIF, and the other stb formats are decoded in-process
   so the onion-skin image needs no external tool. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "../vendor/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define PANEL_LEFT_W 190.0f
#define PANEL_RIGHT_W 300.0f
#define FLOOR_MAX 3
#define FLOOR_DEFAULT 2
static const char* FLOOR_LABELS[FLOOR_MAX] = { "1F", "2F", "3F" };
#define STATUS_H 24.0f
#define FIT_MARGIN 40.0f
#define HIT_TOL_PX 7.0f
/* The Door tool answers from farther away than a Select click, so hovering a
   wall is easy without pixel-perfect aim. */
#define DOOR_HOVER_PX 22.0f
#define CLOSE_TOL_PX 12.0f
#define DRAG_END_PX 8.0f
/* A click may wobble by a pixel or two. A drag gesture only starts applying
   snapped positions once the pointer has actually travelled this far, so a
   plain Select click never moves an off-grid object. */
#define DRAG_START_PX 4.0f
/* How close, in screen pixels, a dragged edge or vertex must come to another
   wall line or endpoint before it snaps to it. */
#define OBJECT_SNAP_PX 8.0f
/* A wall corner is a small target, so it answers a press from this close. */
#define SPAN_END_GRAB_PX 9.0f
#define VERTEX_GRAB_PX 7.0f
#define PLAYER_SPEED 55.0f
#define PLAYER_TURN_RATE 10.0f
#define PLAYER_RADIUS 3.2f
#define PLAYER_SPRITE_WORLD 12.0f
#define PLAYER_MAX_WAYPOINTS 32
/* With Snap off a rectangle only has to be big enough to make four real
   walls; the one-cell rule applies only when the grid is active. */
#define RECT_MIN_SIZE 1.0f
/* One look means a near-black canvas and white lines. A wall and a stair
   tread draw at the same faint alpha, and nothing is ever filled. */
#define CANVAS_BG 0x040404
#define WALL_COL 0x8b9097
#define WALL_ALPHA 70
#define DOOR_WHITE_COL 0xf2f2f2
#define LABEL_COL 0xe6e6e6
#define STAIR_COL 0xc8ccd2
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

/* Six tools own the mouse, in this order and on keys 1 to 6. Select edits
   what is there, Line draws walls, Rectangle unions rectangles into outlines,
   Door places a door on a hovered wall, Stairs drags a stair footprint, and
   Label places text. The tool is the only owner of which gesture a press
   starts, so there is no second mode flag to keep in sync. */
typedef enum {
    TOOL_SELECT = 0,
    TOOL_LINE,
    TOOL_RECT,
    TOOL_DOOR,
    TOOL_STAIRS,
    TOOL_LABEL,
    TOOL_COUNT,
} Tool;

typedef enum {
    SEL_NONE = 0,
    SEL_WALL,
    SEL_DOOR,
    SEL_LABEL,
    SEL_STAIRS,
    SEL_KIND_COUNT,
} SelKind;

typedef enum {
    HIT_NONE = 0,
    HIT_WALL,
    HIT_SHAPE_INTERIOR,
    HIT_DOOR,
    HIT_LABEL,
    HIT_STAIRS,
} HitKind;

typedef enum {
    MODAL_NONE = 0,
    MODAL_OPEN,
    MODAL_SAVE_AS,
    MODAL_HELP,
    MODAL_IMAGE,
} Modal;

typedef enum {
    GESTURE_NONE = 0,
    GESTURE_CHAIN,
    GESTURE_MARQUEE,
    GESTURE_MOVE,
    /* A press on a wall corner moves that corner and the walls that meet it. */
    GESTURE_VERTEX,
    GESTURE_SPAN_END,
    GESTURE_RESHAPE,
    GESTURE_DOOR_SLIDE,
    GESTURE_DOOR_RESIZE,
    GESTURE_STAIRS_RESIZE,
    /* The Rectangle tool drags a new rectangle. The Stairs tool drags a stair
       footprint. */
    GESTURE_RECT_DRAW,
    GESTURE_STAIRS_DRAW,
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
    /* The current position of the corner a vertex drag grabbed. */
    Vec2 vertex;
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
    /* Which end of the marked section a span-end drag holds, 0 or 1. */
    int span_end;
} Gesture;

typedef struct {
    Vec2 center;
    float zoom;
} Camera;

typedef struct {
    SelKind kind;
    int index;
} SelRef;

/* A marked section of one wall. t0 and t1 order along the wall. */
typedef struct {
    WallId wall;
    float t0, t1;
} Span;

typedef struct {
    bool walls[LEVEL_MAX_WALLS];
    bool doors[LEVEL_MAX_DOORS];
    bool labels[LEVEL_MAX_LABELS];
    bool stairs[LEVEL_MAX_STAIRS];
    SelRef primary;
} Selection;

typedef struct {
    HitKind kind;
    int index;
} Hit;

struct Editor {
    Level level;
    /* The three reference floors live side by side. The active floor is
       e->level so every editing path keeps using one document; switching
       copies it into its slot and loads the target slot. History is kept per
       floor, so undo never crosses a floor. */
    Level floor_levels[FLOOR_MAX];
    History floor_history[FLOOR_MAX];
    char floor_paths[FLOOR_MAX][512];
    bool floor_loaded[FLOOR_MAX];
    int floor_count;
    int floor_index;
    /* The floor drawn as an onion skin under the active level. One at most;
       -1 hides it. onion_alpha scales every onion line's opacity. */
    int onion_floor;
    float onion_alpha;
    /* One external image can be skinned under the active floor at the same
       time as the floor skin. The image is fitted to the floor bounds. */
    bool image_onion_on;
    float image_onion_alpha;
    /* Transform relative to the aspect-fit rectangle: world offset and a
       uniform scale about the image centre. */
    Rect image_onion_fit;
    Vec2 image_onion_offset;
    float image_onion_scale;
    bool image_onion_edit;
    bool image_onion_dragging;
    Vec2 image_onion_drag_last;
    char image_onion_path[512];
    int image_onion_w;
    int image_onion_h;
    sg_image image_onion_image;
    sg_view image_onion_view;
    sg_sampler image_onion_sampler;
    ImTextureID image_onion_texture;
    /* A replaced texture stays alive until the next frame, after the draw
       list recorded with it has been submitted. */
    sg_image image_onion_retired_image;
    sg_view image_onion_retired_view;
    /* Which floor owns the single player marker. The marker lives only in
       that floor's Level, so viewing another floor never shows the player. */
    int player_floor;
    Camera camera;
    Rect canvas;
    float menu_height;
    float display_w;
    float display_h;

    Tool tool;
    Selection selection;
    Gesture gesture;
    /* The wall sections Select marked to cut. It sits beside the selection
       instead of inside it, so selecting, reshaping, and cutting never share
       a flag. Per wall the intervals are ordered and disjoint, so Delete cuts
       each marked piece once. */
    Span spans[LEVEL_MAX_WALLS];
    int span_count;
    bool fit_pending;
    bool pan_active;

    float grid_size;
    bool snap;
    bool snap_objects;
    bool show_grid;
    bool marquee_armed;
    /* Creation settings for the next object, shown only by the tool that
       uses them. */
    float door_width;
    float label_size;
    bool tool_locked;
    StairsDir stair_dir;
    int stair_steps;
    bool stair_flip;
    StairFade stair_fade;
    float stair_min_alpha;
    float stair_max_alpha;

    char current_path[512];
    char path_buf[512];
    char image_path_buf[512];
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

    /* Runtime player state: one marker in the level, an A* grid over the
       walls, and the current path. */
    bool play_mode;
    bool player_place_pending;
    bool nav_ready;
    NavGrid nav;
    Vec2 player_path[NAV_MAX_PATH];
    int player_path_count;
    Vec2 player_waypoints[PLAYER_MAX_WAYPOINTS];
    int player_waypoint_count;
    int player_path_index;
    bool player_moving;
    bool player_pivot;
    bool follow_player;
    bool camera_pan_active;
    Vec2 camera_pan_target;
    Vec2 preview_path[NAV_MAX_PATH];
    int preview_count;
    int preview_start;
    int preview_goal;
    int preview_wp_count;
    sg_image player_image;
    sg_view player_view;
    sg_sampler player_sampler;
    ImTextureID player_texture;
};

static const char* TOOL_NAMES[TOOL_COUNT] = {
    "Select", "Line", "Rectangle", "Door", "Stairs", "Label",
};

static const char* TOOL_HINTS[TOOL_COUNT] = {
    "click an object to select it, drag a box to mark wall sections, drag a wall edge to reshape, drag a corner to resize",
    "click to add a wall; click the first point or press Enter to close the chain",
    "drag a rectangle; overlapping rectangles union into one outline",
    "hover a wall and click to place a door; drag its ends with Select to resize",
    "drag a rectangle for a stair run; Flip reverses the fade",
    "click to place a label, then type its text",
};
static bool save_all_floors(Editor* e);
static bool enter_floor(Editor* e, int index);

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

static void set_status(Editor* e, bool error, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->status, sizeof e->status, fmt, ap);
    va_end(ap);
    e->status_error = error;
}

static void commit(Editor* e, const char* message) {
    history_commit(&e->floor_history[e->floor_index], &e->level);
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

/* Capture the aspect-fit rectangle once, when the image is loaded. Recomputing
   it from live level bounds would move the skin whenever drawing expands the
   map. */
static Rect compute_image_onion_fit(const Editor* e, int iw, int ih) {
    float fw = (float)iw;
    float fh = (float)ih;
    Rect b = level_bounds(&e->level);
    float bw = b.max.x - b.min.x;
    float bh = b.max.y - b.min.y;
    if (bw < 1.0f || bh < 1.0f) {
        /* An empty level has no bounds to fit into; show native pixels. */
        Vec2 c = e->camera.center;
        return (Rect){ v2(c.x - fw * 0.5f, c.y - fh * 0.5f),
                       v2(c.x + fw * 0.5f, c.y + fh * 0.5f) };
    }
    float aspect = fw / fh;
    float dw = bw;
    float dh = bh;
    if (bw / bh > aspect) {
        dw = bh * aspect;
    } else {
        dh = bw / aspect;
    }
    Vec2 c = v2((b.min.x + b.max.x) * 0.5f, (b.min.y + b.max.y) * 0.5f);
    return (Rect){ v2(c.x - dw * 0.5f, c.y - dh * 0.5f),
                   v2(c.x + dw * 0.5f, c.y + dh * 0.5f) };
}

static Rect image_onion_rect(const Editor* e) {
    return e->image_onion_fit;
}

static void scale_image_onion_about(Editor* e, Vec2 pivot, float factor) {
    float old_scale = e->image_onion_scale;
    float next_scale = clampf(old_scale * factor, 0.05f, 20.0f);
    if (fabsf(next_scale - old_scale) < 0.00001f) return;
    Rect base = image_onion_rect(e);
    Vec2 base_center = v2((base.min.x + base.max.x) * 0.5f,
                          (base.min.y + base.max.y) * 0.5f);
    Vec2 center = v2_add(base_center, e->image_onion_offset);
    float ratio = next_scale / old_scale;
    e->image_onion_offset = v2_add(
        e->image_onion_offset,
        v2_scale(v2_sub(pivot, center), 1.0f - ratio));
    e->image_onion_scale = next_scale;
}

static const char* path_leaf(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void flush_retired_image_onion(Editor* e) {
    if (e->image_onion_retired_view.id) {
        sg_destroy_view(e->image_onion_retired_view);
        e->image_onion_retired_view = (sg_view){ 0 };
    }
    if (e->image_onion_retired_image.id) {
        sg_destroy_image(e->image_onion_retired_image);
        e->image_onion_retired_image = (sg_image){ 0 };
    }
}

static void retire_image_onion(Editor* e) {
    flush_retired_image_onion(e);
    e->image_onion_retired_view = e->image_onion_view;
    e->image_onion_retired_image = e->image_onion_image;
    e->image_onion_view = (sg_view){ 0 };
    e->image_onion_image = (sg_image){ 0 };
    e->image_onion_texture = 0;
}

static void release_image_onion(Editor* e) {
    retire_image_onion(e);
    flush_retired_image_onion(e);
    e->image_onion_w = 0;
    e->image_onion_h = 0;
    e->image_onion_path[0] = '\0';
    e->image_onion_on = false;
    e->image_onion_edit = false;
    e->image_onion_dragging = false;
    e->image_onion_offset = v2(0.0f, 0.0f);
    e->image_onion_scale = 1.0f;
    e->image_onion_fit = (Rect){ v2(0.0f, 0.0f), v2(0.0f, 0.0f) };
}

static void reset_onion_editor(Editor* e) {
    /* New and Open run after the frame has recorded its draw list, so defer
       the old texture by one frame instead of deleting it immediately. */
    retire_image_onion(e);
    e->image_onion_texture = 0;
    e->image_onion_w = 0;
    e->image_onion_h = 0;
    e->image_onion_path[0] = '\0';
    e->image_onion_on = false;
    e->image_onion_edit = false;
    e->image_onion_dragging = false;
    e->image_onion_offset = v2(0.0f, 0.0f);
    e->image_onion_scale = 1.0f;
    e->image_onion_fit = (Rect){ v2(0.0f, 0.0f), v2(0.0f, 0.0f) };
    e->image_onion_alpha = 0.35f;
    e->onion_floor = -1;
    e->onion_alpha = 0.25f;
    e->image_path_buf[0] = '\0';
    e->modal_error[0] = '\0';
}

/* Decode a file and add its texture. The caller decides when to retire the
   previous image, so loading a saved floor does not keep stale resources. */
static bool upload_image_texture(Editor* e, const char* path, sg_image* out_image,
                                 sg_view* out_view, int* out_width,
                                 int* out_height) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* pixels = stbi_load(path, &width, &height, &channels, 4);
    if (!pixels) {
        const char* why = stbi_failure_reason();
        snprintf(e->modal_error, sizeof e->modal_error, "Could not load %s: %s",
                 path, why ? why : "unknown image error");
        set_status(e, true, "%s", e->modal_error);
        return false;
    }
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        stbi_image_free(pixels);
        snprintf(e->modal_error, sizeof e->modal_error,
                 "Image is too large (maximum 8192 by 8192)");
        set_status(e, true, "%s", e->modal_error);
        return false;
    }

    sg_image_desc image_desc = { 0 };
    image_desc.width = width;
    image_desc.height = height;
    image_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    image_desc.data.ptr = pixels;
    image_desc.data.size = (size_t)width * (size_t)height * 4u;
    sg_image image = sg_make_image(&image_desc);
    stbi_image_free(pixels);
    if (!image.id || sg_query_image_state(image) == SG_RESOURCESTATE_FAILED) {
        if (image.id) sg_destroy_image(image);
        snprintf(e->modal_error, sizeof e->modal_error,
                 "Could not upload the image texture");
        set_status(e, true, "%s", e->modal_error);
        return false;
    }
    sg_view_desc view_desc = { 0 };
    view_desc.texture.image = image;
    sg_view view = sg_make_view(&view_desc);
    if (!view.id) {
        sg_destroy_image(image);
        snprintf(e->modal_error, sizeof e->modal_error,
                 "Could not create the image texture view");
        set_status(e, true, "%s", e->modal_error);
        return false;
    }
    *out_image = image;
    *out_view = view;
    *out_width = width;
    *out_height = height;
    return true;
}

static void apply_texture(Editor* e, sg_image image, sg_view view, int width,
                          int height) {
    e->image_onion_image = image;
    e->image_onion_view = view;
    e->image_onion_texture = (ImTextureID)simgui_imtextureid_with_sampler(
        e->image_onion_view, e->image_onion_sampler);
    e->image_onion_w = width;
    e->image_onion_h = height;
}

/* Copy the live editor fields into the active floor document. Called before a
   save and before another floor's document replaces it. */
static void store_onion_state(Editor* e) {
    OnionSkin* o = &e->level.onion;
    o->image_present = e->image_onion_path[0] != '\0';
    snprintf(o->image_path, sizeof o->image_path, "%s", e->image_onion_path);
    o->image_show = e->image_onion_on;
    o->image_alpha = e->image_onion_alpha;
    o->image_offset = e->image_onion_offset;
    o->image_scale = e->image_onion_scale;
    o->image_fit = e->image_onion_fit;
    o->floor_index = e->onion_floor;
    o->floor_alpha = e->onion_alpha;
}

/* Load the active floor's overlay fields, replacing any image texture. */
static void restore_onion_state(Editor* e) {
    OnionSkin* o = &e->level.onion;
    retire_image_onion(e);
    e->image_onion_texture = 0;
    e->image_onion_w = 0;
    e->image_onion_h = 0;
    e->image_onion_path[0] = '\0';
    e->image_onion_on = o->image_show;
    e->image_onion_alpha = clampf(o->image_alpha, 0.05f, 1.0f);
    e->image_onion_offset = o->image_offset;
    e->image_onion_scale = clampf(o->image_scale, 0.05f, 20.0f);
    e->image_onion_fit = o->image_fit;
    e->onion_floor = o->floor_index;
    if (e->onion_floor < 0 || e->onion_floor >= e->floor_count) {
        e->onion_floor = -1;
    }
    e->onion_alpha = clampf(o->floor_alpha, 0.05f, 1.0f);
    e->image_onion_dragging = false;
    e->image_onion_edit = false;
    if (!o->image_present || !o->image_path[0]) return;
    snprintf(e->image_onion_path, sizeof e->image_onion_path, "%s",
             o->image_path);
    sg_image image;
    sg_view view;
    int width = 0;
    int height = 0;
    if (upload_image_texture(e, o->image_path, &image, &view, &width, &height)) {
        apply_texture(e, image, view, width, height);
    } else {
        /* Keep the saved path so the state can be saved again, but there is
           no texture to draw. */
        e->modal_error[0] = '\0';
    }
}

bool editor_load_onion_image(Editor* e, const char* path) {
    if (!e || !path || !path[0]) return false;
    sg_image image;
    sg_view view;
    int width = 0;
    int height = 0;
    if (!upload_image_texture(e, path, &image, &view, &width, &height)) {
        return false;
    }
    retire_image_onion(e);
    apply_texture(e, image, view, width, height);
    e->image_onion_fit = compute_image_onion_fit(e, width, height);
    e->image_onion_offset = v2(0.0f, 0.0f);
    e->image_onion_scale = 1.0f;
    e->image_onion_dragging = false;
    snprintf(e->image_onion_path, sizeof e->image_onion_path, "%s", path);
    e->image_onion_on = true;
    e->image_path_buf[0] = '\0';
    snprintf(e->image_path_buf, sizeof e->image_path_buf, "%s", path);
    e->modal_error[0] = '\0';
    set_status(e, false, "Image onion %s (%d x %d)", path_leaf(path), width,
               height);
    return true;
}

/* A native picker is optional; the path field in the modal always works. */
static bool pick_image_path(char* out, size_t cap) {
#if defined(__linux__)
    const char* command =
        "zenity --file-selection --title='Choose an onion-skin image' "
        "--file-filter='Images | *.png *.jpg *.jpeg *.bmp *.tga *.gif *.psd *.hdr' "
        "2>/dev/null";
    FILE* pipe = popen(command, "r");
    if (!pipe) return false;
    char buffer[512];
    size_t n = fread(buffer, 1, sizeof buffer - 1, pipe);
    int status = pclose(pipe);
    if (status != 0 || n == 0) return false;
    buffer[n] = '\0';
    while (n > 0 && (buffer[n - 1] == '\n' || buffer[n - 1] == '\r')) {
        buffer[--n] = '\0';
    }
    if (n == 0 || n >= cap) return false;
    memcpy(out, buffer, n + 1);
    return true;
#else
    (void)out;
    (void)cap;
    return false;
#endif
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
    return e->gesture.kind == GESTURE_MOVE || e->gesture.kind == GESTURE_VERTEX ||
           e->gesture.kind == GESTURE_SPAN_END ||
           e->gesture.kind == GESTURE_RESHAPE ||
           e->gesture.kind == GESTURE_DOOR_SLIDE ||
           e->gesture.kind == GESTURE_DOOR_RESIZE ||
           e->gesture.kind == GESTURE_STAIRS_RESIZE ||
           e->gesture.kind == GESTURE_RECT_DRAW ||
           e->gesture.kind == GESTURE_STAIRS_DRAW ||
           e->gesture.kind == GESTURE_LINE_PENDING ||
           e->gesture.kind == GESTURE_MARQUEE;
}

static void cancel_gesture(Editor* e) {
    memset(&e->gesture, 0, sizeof e->gesture);
}

/* Sections live in one canonical list: per wall the intervals are ordered and
   disjoint, so Delete cuts the far piece first and never cuts a piece twice. */
static void span_clear(Editor* e) {
    e->span_count = 0;
}

static bool span_touches(float a0, float a1, float b0, float b1) {
    return a0 <= b1 + 1e-5f && b0 <= a1 + 1e-5f;
}

static void span_add(Editor* e, WallId wall, float t0, float t1) {
    if (t0 > t1) {
        float swap = t0;
        t0 = t1;
        t1 = swap;
    }
    t0 = clampf(t0, 0.0f, 1.0f);
    t1 = clampf(t1, 0.0f, 1.0f);
    if (t1 - t0 < 1e-3f) return;

    bool merged = true;
    while (merged) {
        merged = false;
        for (int i = 0; i < e->span_count; i++) {
            Span* s = &e->spans[i];
            if (s->wall != wall || !span_touches(s->t0, s->t1, t0, t1)) continue;
            t0 = fminf(t0, s->t0);
            t1 = fmaxf(t1, s->t1);
            memmove(&e->spans[i], &e->spans[i + 1],
                    (size_t)(e->span_count - i - 1) * sizeof e->spans[0]);
            e->span_count--;
            merged = true;
            break;
        }
    }
    if (e->span_count >= LEVEL_MAX_WALLS) return;
    e->spans[e->span_count++] = (Span){ wall, t0, t1 };
}

/* Drops marks whose wall has gone, so an undo or a cut cannot leave a section
   pointing at nothing. */
static void span_prune(Editor* e) {
    int keep = 0;
    for (int i = 0; i < e->span_count; i++) {
        if (!level_wall_by_id_const(&e->level, e->spans[i].wall)) continue;
        e->spans[keep++] = e->spans[i];
    }
    e->span_count = keep;
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

static void sel_prune(Editor* e);

static bool object_locked(const Editor* e, SelKind kind, int index) {
    switch (kind) {
        case SEL_WALL:
            return index >= 0 && index < e->level.wall_count &&
                   e->level.walls[index].locked;
        case SEL_DOOR:
            return index >= 0 && index < e->level.door_count &&
                   e->level.doors[index].locked;
        case SEL_LABEL:
            return index >= 0 && index < e->level.label_count &&
                   e->level.labels[index].locked;
        case SEL_STAIRS:
            return index >= 0 && index < e->level.stairs_count &&
                   e->level.stairs[index].locked;
        case SEL_NONE:
        default:
            return false;
    }
}

static bool wall_has_locked_door(const Editor* e, int wall_index) {
    if (wall_index < 0 || wall_index >= e->level.wall_count) return false;
    WallId id = e->level.walls[wall_index].id;
    for (int i = 0; i < e->level.door_count; i++) {
        if (e->level.doors[i].wall == id && e->level.doors[i].locked) return true;
    }
    return false;
}

static bool selection_has_locked(const Editor* e) {
    const Selection* s = &e->selection;
    for (int k = SEL_WALL; k < SEL_KIND_COUNT; k++) {
        SelKind kind = (SelKind)k;
        const bool* bits = sel_pool_const(s, kind);
        int cap = sel_cap(kind);
        for (int i = 0; i < cap; i++) {
            if (!bits[i]) continue;
            if (object_locked(e, kind, i)) return true;
            if (kind == SEL_WALL && wall_has_locked_door(e, i)) return true;
        }
    }
    return false;
}

static bool selection_all_locked(const Editor* e) {
    const Selection* s = &e->selection;
    int n = 0;
    for (int k = SEL_WALL; k < SEL_KIND_COUNT; k++) {
        SelKind kind = (SelKind)k;
        const bool* bits = sel_pool_const(s, kind);
        int cap = sel_cap(kind);
        for (int i = 0; i < cap; i++) {
            if (!bits[i]) continue;
            n++;
            if (!object_locked(e, kind, i)) return false;
        }
    }
    return n > 0;
}

static void set_selection_locked(Editor* e, bool locked) {
    Selection* s = &e->selection;
    for (int k = SEL_WALL; k < SEL_KIND_COUNT; k++) {
        SelKind kind = (SelKind)k;
        bool* bits = sel_pool(s, kind);
        int cap = sel_cap(kind);
        for (int i = 0; i < cap; i++) {
            if (!bits[i]) continue;
            switch (kind) {
                case SEL_WALL: e->level.walls[i].locked = locked; break;
                case SEL_DOOR: e->level.doors[i].locked = locked; break;
                case SEL_LABEL: e->level.labels[i].locked = locked; break;
                case SEL_STAIRS: e->level.stairs[i].locked = locked; break;
                case SEL_NONE:
                default: break;
            }
        }
    }
    /* Locking a room's walls also locks the doors hosted by those walls. */
    for (int d = 0; d < e->level.door_count; d++) {
        if (s->doors[d]) continue;
        int wall = level_find_wall_index(&e->level, e->level.doors[d].wall);
        if (wall >= 0 && s->walls[wall]) e->level.doors[d].locked = locked;
    }
}

static void toggle_selection_lock(Editor* e) {
    sel_prune(e);
    int n = sel_count(&e->selection);
    if (n == 0) {
        set_status(e, true, "Select something before locking it");
        return;
    }
    bool lock = !selection_all_locked(e);
    set_selection_locked(e, lock);
    set_status(e, false, lock ? "Selection locked" : "Selection unlocked");
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
        history_restore_current(&e->floor_history[e->floor_index], &e->level);
        sel_prune(e);
    }
    if (e->gesture.kind == GESTURE_CHAIN) discard_empty_chain_shape(e);
    cancel_gesture(e);
    span_clear(e);
    e->tool = tool;
    set_status(e, false, "%s tool", TOOL_NAMES[tool]);
}

/* Q toggles the lock that keeps the active drawing tool after an action
   instead of handing back to Select. Typing Q into a label field is not a
   lock. The toggle lives on the Editor so it survives between actions. */
static bool q_lock_held(const Editor* e) {
    return e->tool_locked;
}

static void q_lock_toggle(Editor* e) {
    ImGuiIO* io = igGetIO();
    if (io->WantTextInput) return;
    e->tool_locked = !e->tool_locked;
    set_status(e, false, e->tool_locked ? "Tool locked" : "Tool lock off");
}

/* Called once a drawing action has committed and its gesture is cancelled. It
   assigns the tool directly, like a switch, but keeps the action's message in
   the status strip. */
static void auto_select_after_action(Editor* e) {
    if (q_lock_held(e)) return;
    e->tool = TOOL_SELECT;
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
    /* With several floors loaded, fit their union so every floor keeps the
       same on-screen scale and the floor selector never has to hide a wing. */
    if (e->floor_count > 1) {
        for (int i = 0; i < e->floor_count; i++) {
            if (i == e->floor_index || !e->floor_loaded[i]) continue;
            Rect r = level_bounds(&e->floor_levels[i]);
            if (r.min.x < b.min.x) b.min.x = r.min.x;
            if (r.min.y < b.min.y) b.min.y = r.min.y;
            if (r.max.x > b.max.x) b.max.x = r.max.x;
            if (r.max.y > b.max.y) b.max.y = r.max.y;
        }
    }
    float w = b.max.x - b.min.x;
    float h = b.max.y - b.min.y;
    if (w < 1.0f) w = 1.0f;
    if (h < 1.0f) h = 1.0f;
    float margin_l = FIT_MARGIN;
    float margin_r = FIT_MARGIN;
    if (e->floor_count > 1) margin_r = 96.0f;
    float zx = (cw - margin_l - margin_r) / w;
    float zy = (ch - FIT_MARGIN * 2.0f) / h;
    float z = zx < zy ? zx : zy;
    e->camera.zoom = clampf(z, 0.15f, 6.0f);
    e->camera.center = v2(
        (b.min.x + b.max.x) * 0.5f + (margin_r - margin_l) * 0.5f / e->camera.zoom,
        (b.min.y + b.max.y) * 0.5f);
}

static float angle_delta(float from, float to) {
    return atan2f(sinf(to - from), cosf(to - from));
}

static int nav_nearest_open_cell(const NavGrid* nav, int cell, int rings) {
    if (!nav_cell_valid(nav, cell)) return -1;
    if (nav_cell_is_open(nav, cell)) return cell;
    int cx = cell % nav->cols;
    int cy = cell / nav->cols;
    for (int r = 1; r <= rings; r++) {
        int best = -1;
        float best_dist = INFINITY;
        for (int y = cy - r; y <= cy + r; y++) {
            for (int x = cx - r; x <= cx + r; x++) {
                if (x < 0 || y < 0 || x >= nav->cols || y >= nav->rows) continue;
                if (x > cx - r && x < cx + r && y > cy - r && y < cy + r) continue;
                int idx = y * nav->cols + x;
                if (!nav_cell_is_open(nav, idx)) continue;
                float d = (float)((x - cx) * (x - cx) + (y - cy) * (y - cy));
                if (d < best_dist) {
                    best_dist = d;
                    best = idx;
                }
            }
        }
        if (best >= 0) return best;
    }
    return -1;
}

static void start_camera_pan_to_player(Editor* e) {
    if (!e->level.player_present) return;
    e->follow_player = true;
    e->camera_pan_target = e->level.player_pos;
    e->camera_pan_active =
        v2_dist(e->camera.center, e->camera_pan_target) > 0.01f;
    if (!e->camera_pan_active) e->camera.center = e->camera_pan_target;
}

static void recenter_player_camera(Editor* e) {
    if (!e->level.player_present) {
        set_status(e, true, "No player to center on");
        return;
    }
    start_camera_pan_to_player(e);
    set_status(e, false, "Centering on player");
}

static void set_play_mode(Editor* e, bool on) {
    if (e->play_mode == on) return;
    e->play_mode = on;
    e->player_place_pending = false;
    e->player_moving = false;
    e->player_pivot = false;
    e->player_path_count = 0;
    e->player_path_index = 0;
    e->player_waypoint_count = 0;
    e->camera_pan_active = false;
    e->preview_count = 0;
    cancel_gesture(e);
    sel_clear(e);
    span_clear(e);
    if (on) {
        e->nav_ready = nav_build(&e->nav, &e->level, NAV_CELL_SIZE,
                                 PLAYER_RADIUS);
        if (e->level.player_present) {
            int cell = nav_cell_at_world(&e->nav, e->level.player_pos);
            int open = nav_nearest_open_cell(&e->nav, cell, 3);
            if (open >= 0 && open != cell) {
                e->level.player_pos = nav_cell_center(&e->nav, open);
            }
            /* Entering play mode keeps the current pan. The camera pans to
               the player only when a path starts or C is pressed. */
            e->follow_player = false;
            set_status(e, false, "Play mode: click waypoints; double-click to walk");
        } else if (e->player_floor >= 0 && e->player_floor != e->floor_index) {
            set_status(e, false, "Player is on %s; switch to that floor",
                       FLOOR_LABELS[e->player_floor]);
        } else {
            set_status(e, false, "Play mode: click to place the player");
        }
    } else {
        e->follow_player = false;
        set_status(e, false, "Edit mode");
    }
}

static bool start_player_path(Editor* e, const int* cells, int raw) {
    if (!e->level.player_present || raw <= 0) return false;
    int count = nav_smooth_path(&e->nav, cells, raw, e->player_path,
                                NAV_MAX_PATH);
    if (count <= 0) return false;
    e->player_path_count = count;
    e->player_path_index = 0;
    while (e->player_path_index < count - 1 &&
           v2_dist(e->level.player_pos,
                   e->player_path[e->player_path_index]) < 0.75f) {
        e->player_path_index++;
    }
    e->player_moving = true;
    e->preview_count = 0;
    Vec2 first = e->player_path[e->player_path_index];
    Vec2 d = v2_sub(first, e->level.player_pos);
    float desired = atan2f(d.x, -d.y);
    float err = angle_delta(e->level.player_heading, desired);
    e->player_pivot = fabsf(err) > 0.05f;
    start_camera_pan_to_player(e);
    return true;
}

static bool start_player_vec_path(Editor* e, const Vec2* pts, int count) {
    if (!e->level.player_present || count <= 0) return false;
    for (int i = 0; i < count && i < NAV_MAX_PATH; i++) {
        e->player_path[i] = pts[i];
    }
    e->player_path_count = count < NAV_MAX_PATH ? count : NAV_MAX_PATH;
    e->player_path_index = 0;
    while (e->player_path_index < e->player_path_count - 1 &&
           v2_dist(e->level.player_pos,
                   e->player_path[e->player_path_index]) < 0.75f) {
        e->player_path_index++;
    }
    e->player_moving = true;
    e->preview_count = 0;
    Vec2 first = e->player_path[e->player_path_index];
    Vec2 d = v2_sub(first, e->level.player_pos);
    float desired = atan2f(d.x, -d.y);
    float err = angle_delta(e->level.player_heading, desired);
    e->player_pivot = fabsf(err) > 0.05f;
    start_camera_pan_to_player(e);
    return true;
}

static void player_command(Editor* e, Vec2 world) {
    if (!e->level.player_present && e->player_floor >= 0 &&
        e->player_floor != e->floor_index) {
        set_status(e, true, "Player is on %s; switch to that floor",
                   FLOOR_LABELS[e->player_floor]);
        return;
    }
    if (!e->nav_ready) {
        e->nav_ready = nav_build(&e->nav, &e->level, NAV_CELL_SIZE,
                                 PLAYER_RADIUS);
    }
    if (!e->nav_ready) {
        set_status(e, true, "No navigation grid for this level");
        return;
    }
    int target = nav_cell_at_world(&e->nav, world);
    if (!nav_cell_is_open(&e->nav, target)) {
        set_status(e, true, "Click a walkable nav cell");
        return;
    }
    Vec2 target_pos = nav_cell_center(&e->nav, target);
    if (!e->level.player_present) {
        level_place_player(&e->level, target_pos, 0.0f);
        e->player_floor = e->floor_index;
        e->follow_player = true;
        e->camera.center = target_pos;
        set_status(e, false, "Player placed");
        return;
    }
    int start = nav_cell_at_world(&e->nav, e->level.player_pos);
    int open_start = nav_nearest_open_cell(&e->nav, start, 3);
    if (open_start < 0) {
        set_status(e, true, "The player is not on the nav grid");
        return;
    }
    if (open_start != start) {
        e->level.player_pos = nav_cell_center(&e->nav, open_start);
        start = open_start;
    }
    int cells[NAV_MAX_PATH];
    int raw = nav_find_path(&e->nav, start, target, cells, NAV_MAX_PATH);
    if (!start_player_path(e, cells, raw)) {
        set_status(e, true, "No path to that cell");
        return;
    }
    set_status(e, false, "Walking to cell");
}

static void clear_waypoints(Editor* e) {
    e->player_waypoint_count = 0;
    e->preview_count = 0;
}

static bool add_waypoint(Editor* e, int cell) {
    if (!nav_cell_is_open(&e->nav, cell)) return false;
    Vec2 p = nav_cell_center(&e->nav, cell);
    if (e->player_waypoint_count > 0 &&
        v2_dist(e->player_waypoints[e->player_waypoint_count - 1], p) < 0.01f) {
        return true;
    }
    if (e->player_waypoint_count >= PLAYER_MAX_WAYPOINTS) {
        set_status(e, true, "Waypoint limit reached");
        return false;
    }
    int from = e->player_waypoint_count > 0
                   ? nav_cell_at_world(&e->nav,
                                       e->player_waypoints[e->player_waypoint_count - 1])
                   : nav_cell_at_world(&e->nav, e->level.player_pos);
    int leg[NAV_MAX_PATH];
    if (nav_find_path(&e->nav, from, cell, leg, NAV_MAX_PATH) <= 0) {
        set_status(e, true, "No path to that waypoint");
        return false;
    }
    e->player_waypoints[e->player_waypoint_count++] = p;
    e->preview_count = 0;
    set_status(e, false, "%d waypoint%s queued; double-click to walk",
               e->player_waypoint_count,
               e->player_waypoint_count == 1 ? "" : "s");
    return true;
}




static int build_waypoint_path(Editor* e, int extra, Vec2* out, int max) {
    int n = 0;
    int from = nav_cell_at_world(&e->nav, e->level.player_pos);
    if (!nav_cell_is_open(&e->nav, from)) {
        from = nav_nearest_open_cell(&e->nav, from, 3);
    }
    if (from < 0) return 0;
    int targets = e->player_waypoint_count + (extra >= 0 ? 1 : 0);
    if (targets <= 0) return 0;
    for (int i = 0; i < targets; i++) {
        int to = i < e->player_waypoint_count
                     ? nav_cell_at_world(&e->nav, e->player_waypoints[i])
                     : extra;
        if (!nav_cell_is_open(&e->nav, to)) return 0;
        int cells[NAV_MAX_PATH];
        int raw = nav_find_path(&e->nav, from, to, cells, NAV_MAX_PATH);
        if (raw <= 0) return 0;
        for (int k = 0; k < raw; k++) {
            Vec2 q = nav_cell_center(&e->nav, cells[k]);
            if (n == 0 || v2_dist(out[n - 1], q) > 0.01f) {
                if (n >= max) return 0;
                out[n++] = q;
            }
        }
        from = to;
    }
    return n;
}
static void execute_waypoints(Editor* e) {
    if (!e->level.player_present || e->player_waypoint_count <= 0) {
        set_status(e, true, "No waypoints queued");
        return;
    }
    Vec2 pts[NAV_MAX_PATH];
    int n = build_waypoint_path(e, -1, pts, NAV_MAX_PATH);
    if (n <= 0 || !start_player_vec_path(e, pts, n)) {
        set_status(e, true, "No path through those waypoints");
        return;
    }
    int waypoints = e->player_waypoint_count;
    set_status(e, false, "Walking %d waypoint%s", waypoints,
               waypoints == 1 ? "" : "s");
}

static void player_click_play(Editor* e, Vec2 world, bool double_click) {
    if (!e->level.player_present && e->player_floor >= 0 &&
        e->player_floor != e->floor_index) {
        set_status(e, true, "Player is on %s; switch to that floor",
                   FLOOR_LABELS[e->player_floor]);
        return;
    }
    if (!e->nav_ready) {
        e->nav_ready = nav_build(&e->nav, &e->level, NAV_CELL_SIZE,
                                 PLAYER_RADIUS);
    }
    int cell = nav_cell_at_world(&e->nav, world);
    if (!e->level.player_present) {
        player_command(e, world);
        return;
    }
    if (!nav_cell_is_open(&e->nav, cell)) {
        set_status(e, true, "Click a walkable nav cell");
        return;
    }
    if (e->player_moving) {
        e->player_moving = false;
        e->player_path_count = 0;
        clear_waypoints(e);
    }
    if (double_click) {
        if (e->player_waypoint_count == 0 ||
            v2_dist(e->player_waypoints[e->player_waypoint_count - 1],
                    nav_cell_center(&e->nav, cell)) > 0.01f) {
            if (!add_waypoint(e, cell)) return;
        }
        execute_waypoints(e);
        return;
    }
    add_waypoint(e, cell);
}

static void place_player_click(Editor* e, Vec2 world) {
    if (!e->nav_ready) {
        e->nav_ready = nav_build(&e->nav, &e->level, NAV_CELL_SIZE,
                                 PLAYER_RADIUS);
    }
    int cell = nav_cell_at_world(&e->nav, world);
    if (!nav_cell_is_open(&e->nav, cell)) {
        set_status(e, true, "Click a walkable nav cell");
        return;
    }
    if (!level_place_player(&e->level, nav_cell_center(&e->nav, cell), 0.0f)) {
        set_status(e, true, "Could not place the player");
        return;
    }
    if (e->player_floor >= 0 && e->player_floor != e->floor_index) {
        e->floor_levels[e->player_floor].player_present = false;
    }
    e->player_floor = e->floor_index;
    e->player_place_pending = false;
    e->follow_player = false;
    commit(e, "Player placed");
    set_status(e, false, "Player placed");
}

/* The player keeps moving on the floor that owns it, even while another floor
   is being viewed. The active document is only used when it is that floor. */
static Level* player_move_level(Editor* e) {
    if (e->player_floor >= 0 && e->player_floor < e->floor_count &&
        e->player_floor != e->floor_index) {
        return &e->floor_levels[e->player_floor];
    }
    return &e->level;
}

static void update_waypoint_progress(Editor* e, Level* level) {
    while (e->player_waypoint_count > 0 &&
           v2_dist(level->player_pos, e->player_waypoints[0]) < 1.2f) {
        for (int i = 1; i < e->player_waypoint_count; i++) {
            e->player_waypoints[i - 1] = e->player_waypoints[i];
        }
        e->player_waypoint_count--;
        e->preview_count = 0;
    }
}

static void update_player(Editor* e, float dt) {
    if (!e->play_mode || !e->player_moving) return;
    if (dt <= 0.0f) return;
    if (dt > 0.05f) dt = 0.05f;
    Level* level = player_move_level(e);
    if (!level->player_present || e->player_path_count <= 0) {
        e->player_moving = false;
        return;
    }
    Vec2 pos = level->player_pos;
    float heading = level->player_heading;
    while (e->player_path_index < e->player_path_count &&
           v2_dist(pos, e->player_path[e->player_path_index]) < 0.6f) {
        e->player_path_index++;
    }
    if (e->player_path_index >= e->player_path_count) {
        e->player_moving = false;
        e->player_pivot = false;
        e->player_path_count = 0;
        return;
    }
    Vec2 target = e->player_path[e->player_path_index];
    Vec2 delta = v2_sub(target, pos);
    float dist = v2_len(delta);
    if (dist <= 0.0001f) return;
    Vec2 dir = v2_scale(delta, 1.0f / dist);
    float desired = atan2f(dir.x, -dir.y);
    float err = angle_delta(heading, desired);
    float turn = PLAYER_TURN_RATE * dt;
    if (e->player_pivot) {
        if (fabsf(err) <= turn) {
            heading = desired;
            e->player_pivot = false;
        } else {
            heading += clampf(err, -turn, turn);
        }
    } else {
        /* Turn in place only for axis-behind targets; otherwise rotate toward
           the next path point while moving directly along the route. */
        heading += clampf(err, -turn, turn);
        float remaining = PLAYER_SPEED * dt;
        while (remaining > 0.0f &&
               e->player_path_index < e->player_path_count) {
            Vec2 next = e->player_path[e->player_path_index];
            Vec2 to = v2_sub(next, pos);
            float d = v2_len(to);
            if (d <= 0.0001f) {
                e->player_path_index++;
                continue;
            }
            if (d <= remaining) {
                pos = next;
                remaining -= d;
                e->player_path_index++;
            } else {
                pos = v2_add(pos, v2_scale(to, remaining / d));
                remaining = 0.0f;
            }
        }
    }
    level->player_pos = pos;
    level->player_heading = heading;
    update_waypoint_progress(e, level);
}

static void update_camera(Editor* e, float dt) {
    if (!e->play_mode || !e->level.player_present) {
        e->camera_pan_active = false;
        return;
    }
    if (e->camera_pan_active && e->follow_player) {
        Vec2 target = e->level.player_pos;
        e->camera_pan_target = target;
        float t = 1.0f - expf(-10.0f * dt);
        if (dt <= 0.0f) t = 1.0f;
        e->camera.center = v2_lerp(e->camera.center, target, t);
        if (v2_dist(e->camera.center, target) < 0.12f) {
            e->camera.center = target;
            e->camera_pan_active = false;
        }
    } else if (e->follow_player && e->player_moving) {
        e->camera.center = e->level.player_pos;
    }
}

static void update_player_preview(Editor* e) {
    if (!e->play_mode || !e->nav_ready || !e->level.player_present ||
        e->player_moving) {
        e->preview_count = 0;
        return;
    }
    int extra = -1;
    if (e->cursor_valid) {
        int cell = nav_cell_at_world(&e->nav, e->cursor_world);
        if (nav_cell_is_open(&e->nav, cell)) extra = cell;
    }
    if (e->player_waypoint_count == 0 && extra < 0) {
        e->preview_count = 0;
        return;
    }
    int start_cell = nav_cell_at_world(&e->nav, e->level.player_pos);
    if (start_cell == e->preview_start && extra == e->preview_goal &&
        e->player_waypoint_count == e->preview_wp_count &&
        e->preview_count > 0) {
        return;
    }
    int n = build_waypoint_path(e, extra, e->preview_path, NAV_MAX_PATH);
    if (n <= 0) {
        e->preview_count = 0;
        return;
    }
    e->preview_start = start_cell;
    e->preview_goal = extra;
    e->preview_wp_count = e->player_waypoint_count;
    e->preview_count = n;
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
        if (e->level.doors[i].hidden) continue;
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
    /* A small stair inside a big room loses to the room's interior hit unless
       its box is grabbable from just outside, so the box is inflated by the
       hit tolerance before the containment test. */
    float stair_pad = tol;
    for (int i = e->level.stairs_count - 1; i >= 0; i--) {
        Rect r = level_stairs_rect(&e->level.stairs[i]);
        r.min.x -= stair_pad; r.min.y -= stair_pad;
        r.max.x += stair_pad; r.max.y += stair_pad;
        if (rect_contains(r, world)) {
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
    int shape = level_shape_at(&e->level, world);
    if (shape >= 0) {
        hit.kind = HIT_SHAPE_INTERIOR;
        hit.index = shape;
    }
    return hit;
}

/* Removes the selected doors, labels, and stairs. Walls go only when asked,
   because a marked section owns the wall otherwise. */
static void delete_selected_objects(Editor* e, bool with_walls, int* doors,
                                    int* objects) {
    Selection* s = &e->selection;
    bool wall_delete[LEVEL_MAX_WALLS] = { false };
    if (with_walls) {
        for (int i = 0; i < e->level.wall_count; i++) {
            if (s->walls[i]) wall_delete[i] = true;
        }
    }
    *doors = 0;
    *objects = 0;
    for (int i = e->level.door_count - 1; i >= 0; i--) {
        int wall = level_find_wall_index(&e->level, e->level.doors[i].wall);
        bool hosted = wall >= 0 && wall_delete[wall];
        if ((s->doors[i] || hosted) && level_delete_door(&e->level, i)) {
            (*doors)++;
        }
    }
    for (int i = e->level.label_count - 1; i >= 0; i--) {
        if (s->labels[i] && level_delete_label(&e->level, i)) (*objects)++;
    }
    for (int i = e->level.stairs_count - 1; i >= 0; i--) {
        if (s->stairs[i] && level_delete_stairs(&e->level, i)) (*objects)++;
    }
    for (int i = e->level.wall_count - 1; i >= 0; i--) {
        if (wall_delete[i] && level_delete_wall(&e->level, i)) (*objects)++;
    }
}

/* Cuts every marked section in one history step. Sections on one wall run from
   the far end back: each cut keeps the near piece on the same wall id, so the
   next section's original parameters still map by dividing by the new end. */
static int cut_spans(Editor* e, int* dropped_doors, int* failed) {
    int cuts = 0;
    bool used[LEVEL_MAX_WALLS] = { false };
    *dropped_doors = 0;
    *failed = 0;
    for (int i = 0; i < e->span_count; i++) {
        if (used[i]) continue;
        WallId wall = e->spans[i].wall;
        int wall_index = level_find_wall_index(&e->level, wall);
        if (wall_index >= 0 && (e->level.walls[wall_index].locked ||
                                wall_has_locked_door(e, wall_index))) {
            continue;
        }
        float iv[2 * LEVEL_MAX_WALLS];
        int n = 0;
        for (int k = i; k < e->span_count; k++) {
            if (e->spans[k].wall != wall) continue;
            used[k] = true;
            iv[2 * n] = e->spans[k].t0;
            iv[2 * n + 1] = e->spans[k].t1;
            n++;
        }
        for (int k = 1; k < n; k++) {
            float k0 = iv[2 * k];
            float k1 = iv[2 * k + 1];
            int j = k - 1;
            while (j >= 0 && iv[2 * j] > k0) {
                iv[2 * j + 2] = iv[2 * j];
                iv[2 * j + 3] = iv[2 * j + 1];
                j--;
            }
            iv[2 * j + 2] = k0;
            iv[2 * j + 3] = k1;
        }
        float hi = 1.0f;
        for (int k = n - 1; k >= 0; k--) {
            int index = level_find_wall_index(&e->level, wall);
            if (index < 0) break;
            int dropped = 0;
            if (!level_cut_wall_span(&e->level, index, iv[2 * k] / hi,
                                     iv[2 * k + 1] / hi, &dropped)) {
                (*failed)++;
                continue;
            }
            *dropped_doors += dropped;
            cuts++;
            hi = iv[2 * k];
        }
    }
    return cuts;
}

/* Delete acts on both selections. It cuts every marked section and removes the
   selected doors, labels, and stairs, all in one history step. Whole selected
   walls go only when no section is marked, because a section owns its wall. */
static void delete_selection(Editor* e) {
    sel_prune(e);
    if (selection_has_locked(e)) {
        set_status(e, true, "Selection contains locked objects");
        return;
    }
    span_prune(e);
    bool have_objects = sel_count(&e->selection) > 0;
    bool have_sections = e->span_count > 0;
    if (!have_objects && !have_sections) return;

    int doors = 0;
    int objects = 0;
    if (have_objects) {
        delete_selected_objects(e, !have_sections, &doors, &objects);
    }

    int cuts = 0;
    int failed = 0;
    if (have_sections) {
        int cut_doors = 0;
        cuts = cut_spans(e, &cut_doors, &failed);
        doors += cut_doors;
    }

    if (cuts == 0 && have_sections) {
        /* Keep the marks so Delete can be tried again, and still record the
           objects that did go. No cut changes the level when it fails. */
        if (doors + objects > 0) {
            int deleted = doors + objects;
            char msg[96];
            snprintf(msg, sizeof msg, "Deleted %d object%s; sections not cut",
                     deleted, deleted == 1 ? "" : "s");
            commit(e, msg);
        }
        sel_clear(e);
        cancel_gesture(e);
        set_status(e, true, "Could not cut the marked sections");
        return;
    }
    span_clear(e);
    sel_clear(e);
    cancel_gesture(e);
    char message[192];
    if (cuts > 0) {
        snprintf(message, sizeof message,
                 "Cut %d section%s and removed %d door%s", cuts,
                 cuts == 1 ? "" : "s", doors, doors == 1 ? "" : "s");
        size_t n = strlen(message);
        if (objects > 0) {
            snprintf(message + n, sizeof message - n,
                     ", deleted %d object%s", objects,
                     objects == 1 ? "" : "s");
            n = strlen(message);
        }
        if (failed > 0) {
            snprintf(message + n, sizeof message - n,
                     "; %d could not be cut", failed);
        }
        commit(e, message);
        return;
    }
    if (doors + objects > 0) {
        int deleted = doors + objects;
        snprintf(message, sizeof message, "Deleted %d object%s", deleted,
                 deleted == 1 ? "" : "s");
        commit(e, message);
        return;
    }
    set_status(e, true, "Could not delete the selection");
}

typedef struct {
    int walls, shapes, doors, labels, stairs;
} LevelCounts;

static LevelCounts level_counts(const Level* level) {
    return (LevelCounts){
        level->wall_count,
        level->shape_count,
        level->door_count,
        level->label_count,
        level->stairs_count,
    };
}

static bool counts_equal(LevelCounts a, LevelCounts b) {
    return a.walls == b.walls && a.shapes == b.shapes &&
           a.doors == b.doors && a.labels == b.labels && a.stairs == b.stairs;
}

static void sel_after_history(Editor* e, LevelCounts before) {
    if (!counts_equal(before, level_counts(&e->level))) sel_clear(e);
    else sel_prune(e);
}

static void do_undo(Editor* e) {
    if (gesture_is_drag(e)) { set_status(e, false, "Finish the drag before undoing"); return; }
    if (!history_can_undo(&e->floor_history[e->floor_index])) { set_status(e, false, "Nothing to undo"); return; }
    LevelCounts before = level_counts(&e->level);
    cancel_gesture(e);
    history_undo(&e->floor_history[e->floor_index], &e->level);
    sel_after_history(e, before);
    span_clear(e);
    e->label_buf_ready = false;
    set_status(e, false, "Undo");
}

static void do_redo(Editor* e) {
    if (gesture_is_drag(e)) { set_status(e, false, "Finish the drag before redoing"); return; }
    if (!history_can_redo(&e->floor_history[e->floor_index])) { set_status(e, false, "Nothing to redo"); return; }
    LevelCounts before = level_counts(&e->level);
    cancel_gesture(e);
    history_redo(&e->floor_history[e->floor_index], &e->level);
    sel_after_history(e, before);
    span_clear(e);
    e->label_buf_ready = false;
    set_status(e, false, "Redo");
}

static void reset_player_runtime(Editor* e) {
    e->play_mode = false;
    e->player_place_pending = false;
    e->nav_ready = false;
    e->player_moving = false;
    e->player_pivot = false;
    e->player_path_count = 0;
    e->player_path_index = 0;
    e->player_waypoint_count = 0;
    e->follow_player = false;
    e->camera_pan_active = false;
    e->preview_count = 0;
    e->preview_start = -1;
    e->preview_goal = -1;
}

static void new_level(Editor* e) {
    reset_player_runtime(e);
    e->floor_count = 1;
    e->floor_index = 0;
    e->floor_loaded[0] = true;
    e->player_floor = -1;
    level_init(&e->level);
    reset_onion_editor(e);
    history_reset(&e->floor_history[e->floor_index], &e->level);
    sel_clear(e);
    span_clear(e);
    cancel_gesture(e);
    snprintf(e->current_path, sizeof e->current_path, "%s", "levels/untitled.lvl");
    e->fit_pending = true;
    set_status(e, false, "New level");
}

static bool save_level_as(Editor* e, const char* path) {
    LevelError err;
    char target[512];
    snprintf(target, sizeof target, "%s", path);
    store_onion_state(e);
    if (!level_save(&e->level, target, &err)) {
        snprintf(e->modal_error, sizeof e->modal_error, "%s", err.message);
        set_status(e, true, "Save failed: %s", err.message);
        return false;
    }
    snprintf(e->current_path, sizeof e->current_path, "%s", target);
    if (e->floor_index >= 0 && e->floor_index < FLOOR_MAX) {
        snprintf(e->floor_paths[e->floor_index],
                 sizeof e->floor_paths[e->floor_index], "%s", target);
        e->floor_loaded[e->floor_index] = true;
        e->floor_levels[e->floor_index] = e->level;
    }
    set_status(e, false, "Saved %s", path);
    e->modal_error[0] = '\0';
    return true;
}

static bool open_level(Editor* e, const char* path) {
    LevelError err;
    Level loaded;
    level_init(&loaded);
    if (!level_load(&loaded, path, &err)) {
        snprintf(e->modal_error, sizeof e->modal_error, "%s", err.message);
        set_status(e, true, "Open failed: %s", err.message);
        return false;
    }
    e->level = loaded;
    e->floor_count = 1;
    e->floor_index = 0;
    e->floor_loaded[0] = true;
    e->player_floor = e->level.player_present ? 0 : -1;
    snprintf(e->floor_paths[0], sizeof e->floor_paths[0], "%s", path);
    history_reset(&e->floor_history[e->floor_index], &e->level);
    restore_onion_state(e);
    reset_player_runtime(e);
    sel_clear(e);
    span_clear(e);
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

/* Tread lines across a stair box, faint at the start of the fade and
   brighter at the far end. bottom says the fade starts at the min end of the
   run; the Flip setting moves it to the max end. */
#define STAIR_TREAD_PX 3.0f

static float stair_fade_curve(StairFade fade, float frac) {
    frac = clampf(frac, 0.0f, 1.0f);
    switch (fade) {
        case STAIR_FADE_EASE_IN: return frac * frac;
        case STAIR_FADE_EASE_OUT: return 1.0f - (1.0f - frac) * (1.0f - frac);
        case STAIR_FADE_SMOOTH: return frac * frac * (3.0f - 2.0f * frac);
        case STAIR_FADE_LINEAR:
        default: return frac;
    }
}

static void stair_treads(const Editor* e, ImDrawList* dl, Rect r, StairsDir dir,
                         int steps, bool bottom, StairFade fade, float min_alpha,
                         float max_alpha, unsigned rgb) {
    ImVec2 a = world_to_screen(e, r.min);
    ImVec2 b = world_to_screen(e, r.max);
    bool horizontal = dir == STAIRS_H;
    float run = horizontal ? r.max.x - r.min.x : r.max.y - r.min.y;
    float spacing = steps > 0 ? run / (float)steps : run;
    for (int k = 0; k <= steps; k++) {
        float offset = (float)k * spacing;
        float frac = run > 0.0001f ? offset / run : 1.0f;
        if (!bottom) frac = 1.0f - frac;
        float alpha = min_alpha + (max_alpha - min_alpha) *
                                      stair_fade_curve(fade, frac);
        alpha = clampf(alpha, 0.0f, 1.0f);
        ImU32 tread = col32a(rgb, (unsigned)(alpha * 255.0f));
        if (horizontal) {
            ImVec2 p = world_to_screen(e, v2(r.min.x + offset, r.min.y));
            ImDrawList_AddLineVEx(dl, p.x, a.y, b.y, tread, STAIR_TREAD_PX, 0);
        } else {
            ImVec2 p = world_to_screen(e, v2(r.min.x, r.min.y + offset));
            ImDrawList_AddLineHEx(dl, a.x, b.x, p.y, tread, STAIR_TREAD_PX, 0);
        }
    }
}

static void draw_stairs_level(const Editor* e, const Level* level,
                              float alpha_scale) {
    ImDrawList* dl = igGetWindowDrawList();
    for (int i = 0; i < level->stairs_count; i++) {
        const Stairs* s = &level->stairs[i];
        stair_treads(e, dl, level_stairs_rect(s), s->dir, s->steps, s->bottom,
                     s->fade, s->min_alpha * alpha_scale,
                     s->max_alpha * alpha_scale, STAIR_COL);
    }
}

static void draw_stairs(const Editor* e) {
    draw_stairs_level(e, &e->level, 1.0f);
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

typedef struct {
    float a, b;
} DoorSpan;

static int wall_door_spans(const Level* level, const Wall* wall,
                           DoorSpan* spans) {
    float len = wall_length(wall);
    if (len <= 0.0001f) return 0;
    int n = 0;
    for (int i = 0; i < level->door_count; i++) {
        const Door* d = &level->doors[i];
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
        DoorSpan key = spans[i];
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

/* Every wall draws at the same faint alpha. A closed shape has no brighter
   or filled state, so history and selection cannot change a line's weight. */
static void draw_walls_level(const Editor* e, const Level* level,
                             float alpha_scale) {
    ImDrawList* dl = igGetWindowDrawList();
    float wall_px = 2.0f;
    unsigned alpha = (unsigned)(WALL_ALPHA * alpha_scale + 0.5f);
    ImU32 wall_col = col32a(WALL_COL, alpha);
    for (int i = 0; i < level->wall_count; i++) {
        const Wall* w = &level->walls[i];
        DoorSpan spans[LEVEL_MAX_DOORS];
        int n = wall_door_spans(level, w, spans);
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

static void draw_walls(const Editor* e) {
    draw_walls_level(e, &e->level, 1.0f);
}

static void draw_doors_level(const Editor* e, const Level* level,
                             float alpha_scale) {
    ImDrawList* dl = igGetWindowDrawList();
    unsigned alpha = (unsigned)(255.0f * alpha_scale + 0.5f);
    for (int i = 0; i < level->door_count; i++) {
        const Door* d = &level->doors[i];
        if (d->hidden) continue;
        const Wall* w = level_wall_by_id_const(level, d->wall);
        if (!w) continue;
        Vec2 a, b;
        level_door_span(level, d, &a, &b);
        draw_door_bar(dl, e, w, a, b, col32a(DOOR_WHITE_COL, alpha), 0.0f);
    }
}

static void draw_doors(const Editor* e) {
    draw_doors_level(e, &e->level, 1.0f);
}

/* Every marked section draws over its wall in the accent colour at double the
   wall width, with a square cap at each end. The wall itself is untouched. */
static void draw_spans(const Editor* e) {
    if (e->span_count == 0) return;
    ImDrawList* dl = igGetWindowDrawList();
    ImU32 col = col32(ACCENT_COL);
    const float h = 4.0f;
    for (int i = 0; i < e->span_count; i++) {
        const Span* span = &e->spans[i];
        const Wall* wall = level_wall_by_id_const(&e->level, span->wall);
        if (!wall) continue;
        float t0 = span->t0;
        float t1 = span->t1;
        if (t0 > t1) {
            float swap = t0;
            t0 = t1;
            t1 = swap;
        }
        if (t1 - t0 <= 0.0f) continue;
        ImVec2 a = world_to_screen(e, segment_point(wall->a, wall->b, t0));
        ImVec2 b = world_to_screen(e, segment_point(wall->a, wall->b, t1));
        ImDrawList_AddLineEx(dl, a, b, col, 4.0f, 0);
        ImDrawList_AddRectFilledEx(dl, (ImVec2){ a.x - h, a.y - h },
                                   (ImVec2){ a.x + h, a.y + h }, col, 0.0f, 0);
        ImDrawList_AddRectFilledEx(dl, (ImVec2){ b.x - h, b.y - h },
                                   (ImVec2){ b.x + h, b.y + h }, col, 0.0f, 0);
    }
}

/* The wall under the pointer and the centre parameter a door would take.
   Snap runs along the wall only, so the ghost stays on the wall line. */
static bool door_hover(const Editor* e, Vec2 world, int* out_wall, float* out_t) {
    float t = 0.0f;
    int wall = level_nearest_wall(&e->level, world,
                                  DOOR_HOVER_PX / e->camera.zoom, &t);
    if (wall < 0) return false;
    if (e->snap) t = snap_wall_t(&e->level.walls[wall], t, e->grid_size);
    if (out_wall) *out_wall = wall;
    if (out_t) *out_t = t;
    return true;
}

/* A white ghost at the current door width, centred on the projected point
   and clamped to the wall so it matches the door a click will place. */
static void draw_door_ghost(const Editor* e) {
    int wall = -1;
    float t = 0.0f;
    if (!door_hover(e, e->cursor_world, &wall, &t)) return;
    const Wall* w = &e->level.walls[wall];
    float len = wall_length(w);
    if (len <= 0.0001f) return;
    float width = fminf(e->door_width, len);
    width = fmaxf(width, fminf(DOOR_MIN_WIDTH, len));
    float half = width / (2.0f * len);
    float centre = clampf(t, half, 1.0f - half);
    Vec2 a = segment_point(w->a, w->b, centre - half);
    Vec2 b = segment_point(w->a, w->b, centre + half);
    draw_door_bar(igGetWindowDrawList(), e, w, a, b,
                  col32a(DOOR_WHITE_COL, 120), 0.0f);
}

static void draw_labels_level(const Editor* e, const Level* level,
                              float alpha_scale) {
    ImDrawList* dl = igGetWindowDrawList();
    unsigned alpha = (unsigned)(255.0f * alpha_scale + 0.5f);
    for (int i = 0; i < level->label_count; i++) {
        const Label* l = &level->labels[i];
        if (!l->text[0]) continue;
        ImVec2 p = world_to_screen(e, l->pos);
        ImDrawList_AddTextImFontPtrEx(dl, igGetFont(), l->size * e->camera.zoom, p,
                                      col32a(LABEL_COL, alpha), l->text, NULL,
                                      0.0f, NULL);
    }
}

static void draw_labels(const Editor* e) {
    draw_labels_level(e, &e->level, 1.0f);
}

/* The selected external image, fitted to the active floor. It draws under the
   floor skin so both onions can be visible at once. */
static void draw_image_onion(const Editor* e) {
    if (e->play_mode || !e->image_onion_on || !e->image_onion_texture ||
        e->image_onion_w <= 0 || e->image_onion_h <= 0) {
        return;
    }
    float alpha = clampf(e->image_onion_alpha, 0.0f, 1.0f);
    if (alpha <= 0.0f) return;
    Rect base = image_onion_rect(e);
    float half_w = (base.max.x - base.min.x) * 0.5f * e->image_onion_scale;
    float half_h = (base.max.y - base.min.y) * 0.5f * e->image_onion_scale;
    Vec2 center = v2_add(v2((base.min.x + base.max.x) * 0.5f,
                            (base.min.y + base.max.y) * 0.5f),
                         e->image_onion_offset);
    Rect r = { v2(center.x - half_w, center.y - half_h),
               v2(center.x + half_w, center.y + half_h) };
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 p1 = world_to_screen(e, r.min);
    ImVec2 p2 = world_to_screen(e, v2(r.max.x, r.min.y));
    ImVec2 p3 = world_to_screen(e, r.max);
    ImVec2 p4 = world_to_screen(e, v2(r.min.x, r.max.y));
    ImTextureRef tex = { 0 };
    tex._TexID = e->image_onion_texture;
    ImDrawList_AddImageQuadEx(dl, tex, p1, p2, p3, p4,
                              (ImVec2){ 0, 0 }, (ImVec2){ 1, 0 },
                              (ImVec2){ 1, 1 }, (ImVec2){ 0, 1 },
                              col32a(0xffffff,
                                     (unsigned)(alpha * 255.0f + 0.5f)));
    if (e->image_onion_edit) {
        ImU32 outline = col32a(ACCENT_COL, 160);
        ImDrawList_AddRectEx(dl, p1, p3, outline, 0.0f, 2.0f, 0);
        const float h = 4.0f;
        ImVec2 handles[4] = { p1, p2, p3, p4 };
        for (int i = 0; i < 4; i++) {
            ImDrawList_AddRectFilledEx(
                dl, (ImVec2){ handles[i].x - h, handles[i].y - h },
                (ImVec2){ handles[i].x + h, handles[i].y + h },
                col32(ACCENT_COL), 0.0f, 0);
        }
    }
}

/* A faint copy of the checked floor's linework, drawn under the active floor.
   The opacity slider scales every wall, tread, door, and label in the skin. */
static void draw_onion_skin(const Editor* e) {
    if (e->play_mode || e->floor_count <= 1 || e->onion_floor < 0 ||
        e->onion_floor >= e->floor_count ||
        !e->floor_loaded[e->onion_floor]) {
        return;
    }
    float alpha = clampf(e->onion_alpha, 0.0f, 1.0f);
    if (alpha <= 0.0f) return;
    const Level* level = e->onion_floor == e->floor_index
                             ? &e->level
                             : &e->floor_levels[e->onion_floor];
    draw_stairs_level(e, level, alpha);
    draw_walls_level(e, level, alpha);
    draw_doors_level(e, level, alpha);
    draw_labels_level(e, level, alpha);
}

static void draw_nav_hover(const Editor* e) {
    if ((!e->play_mode && !e->player_place_pending) || !e->nav_ready ||
        !e->cursor_valid) {
        return;
    }
    int cell = nav_cell_at_world(&e->nav, e->cursor_world);
    if (!nav_cell_valid(&e->nav, cell)) return;
    Vec2 c = nav_cell_center(&e->nav, cell);
    float half = e->nav.cell * 0.5f;
    ImVec2 a = world_to_screen(e, v2(c.x - half, c.y - half));
    ImVec2 b = world_to_screen(e, v2(c.x + half, c.y + half));
    bool open = nav_cell_is_open(&e->nav, cell);
    ImU32 fill = open ? col32a(0x2f8f5a, 30) : col32a(0x8f2f2f, 22);
    ImU32 edge = open ? col32a(0x58d68d, 220) : col32a(0xc06060, 170);
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_AddRectFilledEx(dl, a, b, fill, 0.0f, 0);
    ImDrawList_AddRectEx(dl, a, b, edge, 0.0f, 2.0f, 0);
}

static void draw_waypoints(const Editor* e) {
    if (!e->play_mode || e->player_waypoint_count <= 0) return;
    ImDrawList* dl = igGetWindowDrawList();
    for (int i = 0; i < e->player_waypoint_count; i++) {
        ImVec2 p = world_to_screen(e, e->player_waypoints[i]);
        ImU32 col = col32a(0x7df0aa, 220);
        ImDrawList_AddRectFilledEx(dl, (ImVec2){ p.x - 3, p.y - 3 },
                                   (ImVec2){ p.x + 3, p.y + 3 }, col, 0.0f, 0);
        ImDrawList_AddRectEx(dl, (ImVec2){ p.x - 5, p.y - 5 },
                             (ImVec2){ p.x + 5, p.y + 5 }, col32a(0x58d68d, 160),
                             0.0f, 1.0f, 0);
    }
}

static int append_draw_point(ImVec2* pts, int count, ImVec2 p, float min_px) {
    if (count > 0) {
        float dx = p.x - pts[count - 1].x;
        float dy = p.y - pts[count - 1].y;
        if (dx * dx + dy * dy < min_px * min_px) return count;
    }
    pts[count++] = p;
    return count;
}

static void draw_player_path(const Editor* e) {
    if (!e->play_mode || !e->level.player_present) return;
    ImVec2 pts[NAV_MAX_PATH + 2];
    int count = 0;
    const float min_px = 3.0f;
    if (e->player_moving && e->player_path_count > 0) {
        count = append_draw_point(pts, count,
                                  world_to_screen(e, e->level.player_pos),
                                  min_px);
        for (int i = e->player_path_index; i < e->player_path_count; i++) {
            count = append_draw_point(pts, count,
                                      world_to_screen(e, e->player_path[i]),
                                      min_px);
        }
    } else if (!e->player_moving && e->preview_count > 0) {
        count = append_draw_point(pts, count,
                                  world_to_screen(e, e->level.player_pos),
                                  min_px);
        for (int i = 0; i < e->preview_count; i++) {
            count = append_draw_point(pts, count,
                                      world_to_screen(e, e->preview_path[i]),
                                      min_px);
        }
    }
    if (count < 2) return;
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_AddPolyline(dl, pts, count, col32a(0x58d68d, 65), 5.0f, 0);
    /* A thin anti-aliased core can break into dotted pixels on near-vertical
       segments. Draw the bright core without AA over the soft glow. */
    ImDrawFlags flags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedLines;
    ImDrawList_AddPolyline(dl, pts, count, col32a(0x7df0aa, 220), 2.0f, 0);
    dl->Flags = flags;
}

static void draw_player(const Editor* e) {
    if (!e->level.player_present || !e->player_texture) return;
    if (e->player_floor >= 0 && e->player_floor != e->floor_index) return;
    ImDrawList* dl = igGetWindowDrawList();
    Vec2 pos = e->level.player_pos;
    float heading = e->level.player_heading;
    Vec2 fwd = v2(sinf(heading), -cosf(heading));
    Vec2 right = v2(cosf(heading), sinf(heading));
    float half = PLAYER_SPRITE_WORLD * 0.5f;
    Vec2 corners[4] = {
        v2_add(v2_add(pos, v2_scale(fwd, half)), v2_scale(right, -half)),
        v2_add(v2_add(pos, v2_scale(fwd, half)), v2_scale(right, half)),
        v2_add(v2_sub(pos, v2_scale(fwd, half)), v2_scale(right, half)),
        v2_add(v2_sub(pos, v2_scale(fwd, half)), v2_scale(right, -half)),
    };
    ImVec2 p[4];
    for (int i = 0; i < 4; i++) p[i] = world_to_screen(e, corners[i]);
    ImTextureRef tex = { 0 };
    tex._TexID = e->player_texture;
    ImDrawList_AddImageQuadEx(dl, tex, p[0], p[1], p[2], p[3],
                              (ImVec2){ 0, 0 }, (ImVec2){ 1, 0 },
                              (ImVec2){ 1, 1 }, (ImVec2){ 0, 1 },
                              IM_COL32_WHITE);
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

/* Wall endpoints draw as small hollow squares in Select, so the corner handles
   are visible without competing with the selection accents. Coincident corners
   draw once. */
static void draw_vertex_handles(const Editor* e) {
    if (e->tool != TOOL_SELECT) return;
    Vec2 points[LEVEL_MAX_WALLS * 2];
    int count = 0;
    for (int i = 0; i < e->level.wall_count; i++) {
        add_vertex(points, &count, e->level.walls[i].a);
        add_vertex(points, &count, e->level.walls[i].b);
    }
    if (count == 0) return;
    ImDrawList* dl = igGetWindowDrawList();
    ImU32 col = col32a(WALL_COL, 140);
    for (int i = 0; i < count; i++) {
        ImVec2 p = world_to_screen(e, points[i]);
        ImDrawList_AddRectEx(dl, (ImVec2){ p.x - 3.0f, p.y - 3.0f },
                             (ImVec2){ p.x + 3.0f, p.y + 3.0f }, col, 0.0f, 1.0f,
                             0);
    }
}

static void draw_selection(Editor* e) {
    sel_prune(e);
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
    int moved = sel_count_kind(s, SEL_LABEL) + sel_count_kind(s, SEL_STAIRS);
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

/* Two rectangle rooms that now overlap after a move or a resize become one
   shape. Rectangles are the only drawing primitive, so the moved shape's
   bounds are the rect to union in; the emptied shape is then deleted. */
/* True when the rect covers real area of the polygon, not just its bounding
   box. Every shape is built from grid cells, so a positive overlap of two
   grid-aligned outlines always contains a full cell; a rect that only touches
   an edge contains none. */
static bool rect_covers_shape_area(const Editor* e, Rect r, const Vec2* pts,
                                   int count) {
    float g = e->grid_size > 1.0f ? e->grid_size : 1.0f;
    for (float y = r.min.y + g * 0.5f; y < r.max.y; y += g) {
        for (float x = r.min.x + g * 0.5f; x < r.max.x; x += g) {
            if (point_in_polygon(pts, count, v2(x, y))) return true;
        }
    }
    return false;
}

/* A rectangle strictly inside an existing room is a new nested outline, not a
   union. Corners must be strictly inside so sharing an outer edge still joins
   the normal way. */
static bool rect_inside_shape(const Editor* e, Rect r, const Vec2* pts,
                              int count) {
    float g = e->grid_size > 1.0f ? e->grid_size : 1.0f;
    for (float y = r.min.y + g * 0.25f; y < r.max.y; y += g * 0.5f) {
        for (float x = r.min.x + g * 0.25f; x < r.max.x; x += g * 0.5f) {
            if (!point_in_polygon(pts, count, v2(x, y))) return false;
        }
    }
    Vec2 corners[4] = {
        r.min, v2(r.max.x, r.min.y), r.max, v2(r.min.x, r.max.y)
    };
    for (int c = 0; c < 4; c++) {
        if (!point_in_polygon(pts, count, corners[c])) return false;
        for (int i = 0; i < count; i++) {
            if (point_segment_dist(corners[c], pts[i], pts[(i + 1) % count]) <
                0.01f) {
                return false;
            }
        }
    }
    return true;
}

/* True when a's outline already covered real area of b's outline. The merge
   after a drag is for a move that makes rectangles overlap; a shape that was
   already containing another must not swallow it just because it moved a
   little. */
static bool shape_already_covers(const Level* level, ShapeId a, ShapeId b) {
    Vec2 pa[LEVEL_MAX_WALLS];
    Vec2 pb[LEVEL_MAX_WALLS];
    int na = level_shape_polygon(level, a, pa, LEVEL_MAX_WALLS);
    int nb = level_shape_polygon(level, b, pb, LEVEL_MAX_WALLS);
    if (na < 3 || nb < 3) return false;
    Rect rb = polygon_bounds(pb, nb);
    const float step = 4.0f;
    for (float y = rb.min.y + step * 0.5f; y < rb.max.y; y += step) {
        for (float x = rb.min.x + step * 0.5f; x < rb.max.x; x += step) {
            if (point_in_polygon(pa, na, v2(x, y))) return true;
        }
    }
    return false;
}

static int merge_overlapping_shapes(Editor* e, const Level* before) {
    Vec2 poly[LEVEL_MAX_WALLS];
    int merges = 0;
    for (int i = 0; i < e->level.shape_count && merges < 8; i++) {
        const Shape* a = &e->level.shapes[i];
        if (!a->closed) continue;
        Rect ra = polygon_bounds(poly, level_shape_polygon(&e->level, a->id, poly,
                                                           LEVEL_MAX_WALLS));
        for (int j = i + 1; j < e->level.shape_count; j++) {
            const Shape* b = &e->level.shapes[j];
            if (!b->closed) continue;
            Rect rb = polygon_bounds(poly, level_shape_polygon(&e->level, b->id, poly,
                                                               LEVEL_MAX_WALLS));
            if (!rect_overlaps_area(ra, rb)) continue;
            if (before && (shape_already_covers(before, a->id, b->id) ||
                           shape_already_covers(before, b->id, a->id))) {
                continue;
            }
            /* Bounds alone would treat the empty corner of an L as filled, so
               the moved rect must cover real area of the other outline. */
            int an = level_shape_polygon(&e->level, a->id, poly, LEVEL_MAX_WALLS);
            if (an < 3 || !rect_covers_shape_area(e, rb, poly, an)) continue;
            ShapeId loser = b->id;
            int dropped = 0;
            if (level_union_rect(&e->level, a->id, rb, &dropped) != LEVEL_UNION_OK) {
                continue;
            }
            level_delete_shape(&e->level, loser);
            merges++;
            break;
        }
    }
    return merges;
}

static void finish_move(Editor* e) {
    if (e->gesture.moved) {
        Level before;
        bool have_before = history_current_copy(&e->floor_history[e->floor_index], &before);
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
        if (merge_overlapping_shapes(e, have_before ? &before : NULL) > 0) {
            set_status(e, false, "Overlapping rectangles joined");
        }
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

/* The part of a wall the box crosses, in wall parameters. The box must pass
   over the wall line, not only touch it, so a snapped box edge that lands on a
   wall does not mark the whole line. */
static bool wall_section_in_rect(const Wall* wall, Rect box, float* out_t0,
                                 float* out_t1) {
    float len = wall_length(wall);
    if (len <= 1e-3f) return false;
    float lo, hi;
    if (wall_is_vertical(wall)) {
        float x = wall->a.x;
        if (!(box.min.x < x - 1e-3f && x + 1e-3f < box.max.x)) return false;
        lo = fmaxf(fminf(wall->a.y, wall->b.y), box.min.y);
        hi = fminf(fmaxf(wall->a.y, wall->b.y), box.max.y);
        if (hi - lo < 1e-3f) return false;
        *out_t0 = point_segment_t(v2(x, lo), wall->a, wall->b);
        *out_t1 = point_segment_t(v2(x, hi), wall->a, wall->b);
    } else {
        float y = wall->a.y;
        if (!(box.min.y < y - 1e-3f && y + 1e-3f < box.max.y)) return false;
        lo = fmaxf(fminf(wall->a.x, wall->b.x), box.min.x);
        hi = fminf(fmaxf(wall->a.x, wall->b.x), box.max.x);
        if (hi - lo < 1e-3f) return false;
        *out_t0 = point_segment_t(v2(lo, y), wall->a, wall->b);
        *out_t1 = point_segment_t(v2(hi, y), wall->a, wall->b);
    }
    if (*out_t0 > *out_t1) {
        float swap = *out_t0;
        *out_t0 = *out_t1;
        *out_t1 = swap;
    }
    /* A stub shorter than the level's minimum wall would make the cut fail, so
       a section that nearly reaches an end takes the whole end. */
    if (*out_t0 * len < 1e-3f) *out_t0 = 0.0f;
    if ((1.0f - *out_t1) * len < 1e-3f) *out_t1 = 1.0f;
    return (*out_t1 - *out_t0) * len >= 1e-3f;
}

/* Marks every wall the box crosses. The box corners are already snapped when
   Snap is on, and nothing else is snapped, so each section is exactly the part
   of the wall inside the box. */
static void collect_sections(Editor* e, Rect box, bool additive) {
    if (!additive) span_clear(e);
    for (int i = 0; i < e->level.wall_count; i++) {
        if (e->level.walls[i].locked || wall_has_locked_door(e, i)) continue;
        float t0, t1;
        if (!wall_section_in_rect(&e->level.walls[i], box, &t0, &t1)) continue;
        span_add(e, e->level.walls[i].id, t0, t1);
    }
}

/* A marquee selects whole non-wall objects inside the box and marks the
   crossed part of each wall for cutting. */
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
    if (!sel_has(&e->selection, e->selection.primary.kind, e->selection.primary.index)) {
        sel_promote(e);
    }
    collect_sections(e, rect, additive);
    int objects = sel_count(&e->selection);
    if (e->span_count > 0 && objects > 0) {
        set_status(e, false, "%d section%s and %d object%s selected",
                   e->span_count, e->span_count == 1 ? "" : "s", objects,
                   objects == 1 ? "" : "s");
    } else if (e->span_count > 0) {
        set_status(e, false, "%d section%s marked, press Delete", e->span_count,
                   e->span_count == 1 ? "" : "s");
    } else {
        set_status(e, false, "%d object%s selected", objects,
                   objects == 1 ? "" : "s");
    }
}

/* ---- Line tool ------------------------------------------------------- */

static void begin_select(Editor* e, Vec2 world, ImVec2 mouse);

static void begin_marquee(Editor* e, Vec2 world, ImVec2 mouse, bool additive) {
    cancel_gesture(e);
    if (!additive) sel_clear(e);
    Gesture* g = &e->gesture;
    g->kind = GESTURE_MARQUEE;
    g->press_world = snapped(e, world);
    g->cur_world = g->press_world;
    g->press_screen = v2(mouse.x, mouse.y);
    g->additive = additive;
    g->moved = false;
    set_status(e, false, additive ? "Additive marquee" : "Marquee");
}
static void begin_wall_click(Editor* e, Vec2 world, ImVec2 mouse);

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
        set_status(e, false, "Click to add a wall");
    }
}

/* Nearest unselected wall line or endpoint coordinate on one axis. */
static bool snap_axis_objects(const Editor* e, const bool* skip, float value,
                              bool x_axis, float tol, float* out) {
    bool found = false;
    float best = tol;
    for (int i = 0; i < e->level.wall_count; i++) {
        if (skip && skip[i]) continue;
        const Wall* w = &e->level.walls[i];
        bool vertical = wall_is_vertical(w);
        float line = x_axis ? w->a.x : w->a.y;
        bool line_matches = x_axis ? vertical : !vertical;
        if (line_matches && fabsf(line - value) < best) {
            best = fabsf(line - value);
            *out = line;
            found = true;
        }
        /* Endpoints act as vertex targets even on the perpendicular axis. */
        float va = x_axis ? w->a.x : w->a.y;
        float vb = x_axis ? w->b.x : w->b.y;
        if (fabsf(va - value) < best) {
            best = fabsf(va - value);
            *out = va;
            found = true;
        }
        if (fabsf(vb - value) < best) {
            best = fabsf(vb - value);
            *out = vb;
            found = true;
        }
    }
    return found;
}

/* Snap a point to an unselected vertex first, then to wall lines on each axis
   independently. out_hit_x/out_hit_y say which axes object snapping claimed. */
static Vec2 snap_point_objects(const Editor* e, const bool* skip, Vec2 p,
                               float tol, bool* out_hit_x, bool* out_hit_y) {
    Vec2 out = p;
    bool hit_x = false;
    bool hit_y = false;
    float best = tol;
    bool vertex = false;
    for (int i = 0; i < e->level.wall_count; i++) {
        if (skip && skip[i]) continue;
        const Wall* w = &e->level.walls[i];
        Vec2 ends[2] = { w->a, w->b };
        for (int k = 0; k < 2; k++) {
            float d = v2_dist(p, ends[k]);
            if (d < best) {
                best = d;
                out = ends[k];
                vertex = true;
            }
        }
    }
    if (vertex) {
        hit_x = true;
        hit_y = true;
    } else {
        float value;
        if (snap_axis_objects(e, skip, p.x, true, tol, &value)) {
            out.x = value;
            hit_x = true;
        }
        if (snap_axis_objects(e, skip, p.y, false, tol, &value)) {
            out.y = value;
            hit_y = true;
        }
    }
    if (out_hit_x) *out_hit_x = hit_x;
    if (out_hit_y) *out_hit_y = hit_y;
    return out;
}

/* Object snap wins on the axes it finds a target for; grid snap fills the
   rest. With both toggles off this returns p unchanged. */
static Vec2 snap_edit_point(const Editor* e, const bool* skip, Vec2 p) {
    Vec2 out = p;
    bool hit_x = false;
    bool hit_y = false;
    if (e->snap_objects) {
        float tol = OBJECT_SNAP_PX / (e->camera.zoom > 0.0001f ? e->camera.zoom : 1.0f);
        out = snap_point_objects(e, skip, p, tol, &hit_x, &hit_y);
    }
    if (e->snap) {
        Vec2 grid = snap_grid(p, e->grid_size);
        if (!hit_x) out.x = grid.x;
        if (!hit_y) out.y = grid.y;
    }
    return out;
}

static float snap_edit_axis(const Editor* e, const bool* skip, float value,
                            bool x_axis) {
    if (e->snap_objects) {
        float tol = OBJECT_SNAP_PX / (e->camera.zoom > 0.0001f ? e->camera.zoom : 1.0f);
        float snapped = value;
        if (snap_axis_objects(e, skip, value, x_axis, tol, &snapped)) {
            return snapped;
        }
    }
    if (e->snap) return snap_scalar(value, e->grid_size);
    return value;
}

/* Consider one moving edge coordinate as an object snap target and keep the
   translation offset of the nearest hit. */
static void snap_move_candidate(const Editor* e, const bool* skip, float base,
                                float shift, bool x_axis, float tol,
                                bool* found, float* best, float* offset) {
    float value = base + shift;
    float target;
    if (!snap_axis_objects(e, skip, value, x_axis, tol, &target)) return;
    float d = fabsf(target - value);
    if (!*found || d < *best) {
        *found = true;
        *best = d;
        *offset = target - value;
    }
}

/* Object snap for a rigid move checks every edge and endpoint that moves, not
   just the primary anchor, so a selection's top, left, right, and bottom can
   all land on a wall line or endpoint. The grid still snaps the primary
   anchor, which keeps a group's relative spacing intact. */
static Vec2 snap_move_point(const Editor* e, const bool* moving, Vec2 anchor,
                            Vec2 raw, Vec2 applied) {
    Vec2 shift = v2_sub(raw, applied);
    Vec2 goal = v2_add(anchor, raw);
    float tol = OBJECT_SNAP_PX / (e->camera.zoom > 0.0001f ? e->camera.zoom : 1.0f);
    bool found_x = false;
    bool found_y = false;
    float best_x = tol;
    float best_y = tol;
    float offset_x = 0.0f;
    float offset_y = 0.0f;

    if (e->snap_objects) {
        for (int i = 0; i < e->level.wall_count; i++) {
            if (!moving[i]) continue;
            const Wall* w = &e->level.walls[i];
            snap_move_candidate(e, moving, w->a.x, shift.x, true, tol,
                                &found_x, &best_x, &offset_x);
            snap_move_candidate(e, moving, w->b.x, shift.x, true, tol,
                                &found_x, &best_x, &offset_x);
            snap_move_candidate(e, moving, w->a.y, shift.y, false, tol,
                                &found_y, &best_y, &offset_y);
            snap_move_candidate(e, moving, w->b.y, shift.y, false, tol,
                                &found_y, &best_y, &offset_y);
        }
        for (int i = 0; i < e->level.label_count; i++) {
            if (!sel_has(&e->selection, SEL_LABEL, i)) continue;
            Rect r = label_world_rect(e, &e->level.labels[i]);
            snap_move_candidate(e, moving, r.min.x, shift.x, true, tol,
                                &found_x, &best_x, &offset_x);
            snap_move_candidate(e, moving, r.max.x, shift.x, true, tol,
                                &found_x, &best_x, &offset_x);
            snap_move_candidate(e, moving, r.min.y, shift.y, false, tol,
                                &found_y, &best_y, &offset_y);
            snap_move_candidate(e, moving, r.max.y, shift.y, false, tol,
                                &found_y, &best_y, &offset_y);
        }
        for (int i = 0; i < e->level.stairs_count; i++) {
            if (!sel_has(&e->selection, SEL_STAIRS, i)) continue;
            Rect r = level_stairs_rect(&e->level.stairs[i]);
            snap_move_candidate(e, moving, r.min.x, shift.x, true, tol,
                                &found_x, &best_x, &offset_x);
            snap_move_candidate(e, moving, r.max.x, shift.x, true, tol,
                                &found_x, &best_x, &offset_x);
            snap_move_candidate(e, moving, r.min.y, shift.y, false, tol,
                                &found_y, &best_y, &offset_y);
            snap_move_candidate(e, moving, r.max.y, shift.y, false, tol,
                                &found_y, &best_y, &offset_y);
        }
        /* The primary anchor stays a candidate so a door-only group keeps
           snapping from the door's centre. */
        snap_move_candidate(e, moving, anchor.x + applied.x, shift.x, true, tol,
                            &found_x, &best_x, &offset_x);
        snap_move_candidate(e, moving, anchor.y + applied.y, shift.y, false, tol,
                            &found_y, &best_y, &offset_y);
    }

    if (found_x) goal.x += offset_x;
    else if (e->snap) goal.x = snap_scalar(goal.x, e->grid_size);
    if (found_y) goal.y += offset_y;
    else if (e->snap) goal.y = snap_scalar(goal.y, e->grid_size);
    return goal;
}

/* True once the pointer has moved far enough for a press to count as a drag.
   Until then a press is a click and must leave the document untouched, even
   when the object is not on the current snap grid. */
static bool past_drag_start(const Editor* e, ImVec2 mouse) {
    return screen_dist(mouse, v2_to_im(e->gesture.press_screen)) >= DRAG_START_PX;
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
            g->cur_world = snapped(e, world);
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
                if (!have_pointer || !past_drag_start(e, io->MousePos)) break;
                Vec2 raw = v2_sub(world, g->press_world);
                Vec2 goal = snap_move_point(e, g->walls, g->anchor, raw,
                                            g->move_applied);
                Vec2 delta = v2_sub(goal, g->anchor);
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
        case GESTURE_SPAN_END: {
            if (g->object_index < 0 || g->object_index >= e->span_count) {
                cancel_gesture(e);
                break;
            }
            if (down && have_pointer && past_drag_start(e, io->MousePos)) {
                Span* span = &e->spans[g->object_index];
                const Wall* w = level_wall_by_id_const(&e->level, span->wall);
                if (!w) {
                    cancel_gesture(e);
                    break;
                }
                float t = point_segment_t(world, w->a, w->b);
                if (e->snap) t = snap_wall_t(w, t, e->grid_size);
                const float min_span = 0.02f;
                if (g->span_end == 0) {
                    if (t > span->t1 - min_span) t = span->t1 - min_span;
                    if (t < 0.0f) t = 0.0f;
                    span->t0 = t;
                } else {
                    if (t < span->t0 + min_span) t = span->t0 + min_span;
                    if (t > 1.0f) t = 1.0f;
                    span->t1 = t;
                }
                g->moved = true;
            }
            if (!down) {
                const Wall* w = level_wall_by_id_const(
                    &e->level, e->spans[g->object_index].wall);
                float len = w ? wall_length(w) *
                                    fabsf(e->spans[g->object_index].t1 -
                                          e->spans[g->object_index].t0) : 0.0f;
                set_status(e, false, "Section of %.0f units", len);
                cancel_gesture(e);
            }
            break;
        }
        case GESTURE_VERTEX: {
            if (down) {
                if (!have_pointer || !past_drag_start(e, io->MousePos)) break;
                bool skip[LEVEL_MAX_WALLS] = { false };
                for (int i = 0; i < e->level.wall_count; i++) {
                    const Wall* w = &e->level.walls[i];
                    if (v2_equal(w->a, g->vertex, 0.0001f) ||
                        v2_equal(w->b, g->vertex, 0.0001f)) {
                        skip[i] = true;
                    }
                }
                Vec2 target = snap_edit_point(e, skip, world);
                if (!v2_equal(target, g->vertex, 0.0001f) &&
                    level_move_vertex(&e->level, g->vertex, target)) {
                    g->vertex = target;
                    g->moved = true;
                }
            }
            if (!down) {
                if (g->moved) {
                    commit(e, "Moved a corner");
                } else if (g->object_index >= 0 &&
                           g->object_index < e->level.wall_count) {
                    sel_only(e, SEL_WALL, g->object_index);
                    sel_set_primary(e, SEL_WALL, g->object_index);
                    set_status(e, false, "Edge selected");
                }
                cancel_gesture(e);
            }
            break;
        }
        case GESTURE_RESHAPE: {
            if (down) {
                if (!have_pointer || !past_drag_start(e, io->MousePos)) break;
                Vec2 raw = v2_sub(world, g->press_world);
                Vec2 total = raw;
                total.x = g->ref_wall_x >= 0
                              ? snap_edit_axis(e, g->walls,
                                               g->ref_start_x + raw.x, true) -
                                    g->ref_start_x
                              : 0.0f;
                total.y = g->ref_wall_y >= 0
                              ? snap_edit_axis(e, g->walls,
                                               g->ref_start_y + raw.y, false) -
                                    g->ref_start_y
                              : 0.0f;
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
                if (g->moved) {
                    Level before;
                    bool have_before = history_current_copy(&e->floor_history[e->floor_index], &before);
                    commit(e, "Reshaped");
                    if (merge_overlapping_shapes(e, have_before ? &before : NULL) > 0) {
                        set_status(e, false, "Overlapping rectangles joined");
                    }
                } else {
                    collapse_click(e);
                }
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
                    if (down && have_pointer && past_drag_start(e, io->MousePos)) {
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
                        if (down && have_pointer &&
                            past_drag_start(e, io->MousePos)) {
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
                if (!have_pointer || !past_drag_start(e, io->MousePos)) break;
                Rect before = level_stairs_rect(&e->level.stairs[idx]);
                Rect next = before;
                /* Snap on keeps every edge on the grid, so the minimum is one
                   whole cell even when that is larger than STAIR_MIN_SIZE. */
                float min_size = e->snap ? fmaxf(STAIR_MIN_SIZE, e->grid_size)
                                         : STAIR_MIN_SIZE;
                if (g->stair_edge == STAIR_EDGE_LEFT ||
                    g->stair_edge == STAIR_EDGE_RIGHT) {
                    float x = snap_edit_axis(e, NULL, world.x, true);
                    if (g->stair_edge == STAIR_EDGE_LEFT) {
                        next.min.x = fminf(x, g->stair_fixed - min_size);
                    } else {
                        next.max.x = fmaxf(x, g->stair_fixed + min_size);
                    }
                } else if (g->stair_edge == STAIR_EDGE_TOP ||
                           g->stair_edge == STAIR_EDGE_BOTTOM) {
                    float y = snap_edit_axis(e, NULL, world.y, false);
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
    cancel_gesture(e);
    commit(e, "Shape closed");
    auto_select_after_action(e);
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
        bool restored = history_restore_current(&e->floor_history[e->floor_index], &e->level);
        sel_prune(e);
        cancel_gesture(e);
        set_status(e, false, restored ? "Drag cancelled" : "Drag cancelled, no history");
    }
}

/* ---- Rectangle and Stairs tools --------------------------------------- */

/* The Rectangle tool follows the same Snap setting as every other tool. */
static void begin_rect_drag(Editor* e, Vec2 world, ImVec2 mouse) {
    Gesture* g = &e->gesture;
    cancel_gesture(e);
    g->kind = GESTURE_RECT_DRAW;
    g->press_world = snapped(e, world);
    g->cur_world = g->press_world;
    g->press_screen = v2(mouse.x, mouse.y);
    set_status(e, false, "Drag a rectangle");
}

/* The newest closed shape whose area the rect overlaps takes the union. A rect
   that only touches an edge keeps its own outline, so two rooms that share a
   wall draw that wall once from each side instead of merging into one. */
static int union_rect_into_shapes(Editor* e, Rect r, int* dropped) {
    Vec2 polygon[LEVEL_MAX_WALLS];
    for (int i = e->level.shape_count - 1; i >= 0; i--) {
        const Shape* shape = &e->level.shapes[i];
        if (!shape->closed) continue;
        int count = level_shape_polygon(&e->level, shape->id, polygon,
                                        LEVEL_MAX_WALLS);
        if (count < 3) continue;
        if (rect_inside_shape(e, r, polygon, count)) continue;
        if (!rect_covers_shape_area(e, r, polygon, count)) continue;
        if (level_union_rect(&e->level, shape->id, r, dropped) == LEVEL_UNION_OK) {
            return 1;
        }
    }
    return 0;
}

static void finish_rect_drag(Editor* e, bool have_pointer) {
    Gesture* g = &e->gesture;
    if (g->kind != GESTURE_RECT_DRAW) return;
    if (have_pointer) {
        g->cur_world = snapped(e, screen_to_world(e, igGetIO()->MousePos));
    }
    Vec2 a = g->press_world;
    Vec2 b = g->cur_world;
    float min_size = e->snap ? e->grid_size : RECT_MIN_SIZE;
    if (fabsf(b.x - a.x) < min_size || fabsf(b.y - a.y) < min_size) {
        set_status(e, true,
                   e->snap ? "Rectangle needs one grid cell on each axis"
                           : "Rectangle is too small");
        cancel_gesture(e);
        return;
    }
    Rect r = rect_from_points(a, b);
    cancel_gesture(e);

    int dropped = 0;
    if (!union_rect_into_shapes(e, r, &dropped)) {
        if (level_add_rect_shape(&e->level, r) == 0) {
            set_status(e, true, "Could not add the rectangle");
            return;
        }
        commit(e, "Rectangle added");
        auto_select_after_action(e);
        return;
    }

    if (dropped > 0) {
        set_status(e, false, "Rectangle joined; %d door%s dropped",
                   dropped, dropped == 1 ? "" : "s");
        commit(e, NULL);
    } else {
        commit(e, "Rectangle joined");
    }
    auto_select_after_action(e);
}

static void begin_stairs_drag(Editor* e, Vec2 world, ImVec2 mouse) {
    Gesture* g = &e->gesture;
    cancel_gesture(e);
    g->kind = GESTURE_STAIRS_DRAW;
    g->press_world = snapped(e, world);
    g->cur_world = g->press_world;
    g->press_screen = v2(mouse.x, mouse.y);
    set_status(e, false, "Drag a stair footprint");
}

static void finish_stairs_drag(Editor* e, bool have_pointer) {
    Gesture* g = &e->gesture;
    if (g->kind != GESTURE_STAIRS_DRAW) return;
    if (have_pointer) {
        g->cur_world = snapped(e, screen_to_world(e, igGetIO()->MousePos));
    }
    Rect r = rect_from_points(g->press_world, g->cur_world);
    float min_size = e->snap ? e->grid_size : STAIR_MIN_SIZE;
    if (r.max.x - r.min.x < min_size || r.max.y - r.min.y < min_size) {
        set_status(e, true, "Stairs need one grid cell on each axis");
        cancel_gesture(e);
        return;
    }
    int idx = level_add_stairs(&e->level, r, e->stair_dir, e->stair_steps);
    if (idx < 0) {
        set_status(e, true, "Could not add the stairs");
        cancel_gesture(e);
        return;
    }
    /* The panel's Flip setting picks which end of the run the fade starts
       from; the inspector toggles it for one stair afterwards. */
    e->level.stairs[idx].bottom = e->stair_flip;
    level_set_stairs_fade(&e->level, idx, e->stair_fade, e->stair_min_alpha,
                          e->stair_max_alpha);
    sel_only(e, SEL_STAIRS, idx);
    cancel_gesture(e);
    commit(e, "Stairs added");
    auto_select_after_action(e);
}

static void update_shape_drag(Editor* e, bool have_pointer) {
    Gesture* g = &e->gesture;
    if (g->kind != GESTURE_RECT_DRAW && g->kind != GESTURE_STAIRS_DRAW) {
        return;
    }
    if (have_pointer && igGetIO()->MouseDown[0]) {
        g->cur_world = snapped(e, screen_to_world(e, igGetIO()->MousePos));
        return;
    }
    if (g->kind == GESTURE_STAIRS_DRAW) finish_stairs_drag(e, have_pointer);
    else finish_rect_drag(e, have_pointer);
}

static void draw_shape_preview(const Editor* e) {
    const Gesture* g = &e->gesture;
    if (g->kind != GESTURE_RECT_DRAW && g->kind != GESTURE_STAIRS_DRAW) {
        return;
    }
    Rect r = rect_from_points(g->press_world, g->cur_world);
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 a = world_to_screen(e, r.min);
    ImVec2 b = world_to_screen(e, r.max);
    if (g->kind == GESTURE_STAIRS_DRAW) {
        stair_treads(e, dl, r, e->stair_dir, e->stair_steps, e->stair_flip,
                     e->stair_fade, e->stair_min_alpha, e->stair_max_alpha,
                     ACCENT_COL);
    }
    ImDrawList_AddRectEx(dl, a, b, col32(ACCENT_COL), 0.0f, 1.0f, 0);
}

/* Nearest wall endpoint to the pointer within the grab radius. Coincident
   corners are one handle, so the wall that answers only names a click target;
   the move itself covers every wall at that point. */
/* The marked section's end handle under the pointer, or 0. A section end is a
   draggable handle, so it is picked before the wall and before the clear that a
   plain press would otherwise do. */
static int nearest_span_end(const Editor* e, ImVec2 mouse, int* out_span,
                            int* out_end) {
    float best = SPAN_END_GRAB_PX + 0.001f;
    int found = 0;
    for (int i = 0; i < e->span_count; i++) {
        const Wall* wall = level_wall_by_id_const(&e->level, e->spans[i].wall);
        if (!wall) continue;
        float ends[2] = { e->spans[i].t0, e->spans[i].t1 };
        for (int k = 0; k < 2; k++) {
            ImVec2 p = world_to_screen(e, segment_point(wall->a, wall->b, ends[k]));
            float d = screen_dist(mouse, p);
            if (d < best) {
                best = d;
                found = 1;
                if (out_span) *out_span = i;
                if (out_end) *out_end = k;
            }
        }
    }
    return found;
}

static void begin_span_end(Editor* e, int span_index, int end) {
    Gesture* g = &e->gesture;
    cancel_gesture(e);
    g->kind = GESTURE_SPAN_END;
    g->object_index = span_index;
    g->span_end = end;
    g->moved = false;
    set_status(e, false, "Drag to resize the section");
}

static int nearest_vertex(const Editor* e, ImVec2 mouse, Vec2* out) {
    int best = -1;
    float best_dist = VERTEX_GRAB_PX + 0.001f;
    for (int i = e->level.wall_count - 1; i >= 0; i--) {
        const Wall* wall = &e->level.walls[i];
        Vec2 ends[2] = { wall->a, wall->b };
        for (int k = 0; k < 2; k++) {
            float d = screen_dist(mouse, world_to_screen(e, ends[k]));
            if (d < best_dist) {
                best_dist = d;
                best = i;
                if (out) *out = ends[k];
            }
        }
    }
    return best;
}

static void begin_vertex(Editor* e, int wall_index, Vec2 vertex) {
    Gesture* g = &e->gesture;
    cancel_gesture(e);
    g->kind = GESTURE_VERTEX;
    g->object_index = wall_index;
    g->vertex = vertex;
    g->moved = false;
    set_status(e, false, "Drag the corner");
}

static void begin_select(Editor* e, Vec2 world, ImVec2 mouse) {
    ImGuiIO* io = igGetIO();
    bool shift = io->KeyShift;
    bool ctrl = io->KeyCtrl;

    if (e->marquee_armed && !ctrl) {
        e->marquee_armed = false;
        begin_marquee(e, world, mouse, shift);
        return;
    }

    /* A section end is a handle: grabbing it resizes the section instead of
       clearing it or starting a marquee. */
    if (!shift && !ctrl) {
        int span_index = -1;
        int end = -1;
        if (nearest_span_end(e, mouse, &span_index, &end)) {
            const Wall* span_wall = level_wall_by_id_const(
                &e->level, e->spans[span_index].wall);
            if (span_wall && !span_wall->locked) {
                begin_span_end(e, span_index, end);
                e->gesture.press_screen = v2(mouse.x, mouse.y);
                return;
            }
        }
    }

    /* A plain press drops the marked sections. Shift and Ctrl presses keep
       them, because those add to the selection instead of starting over. */
    if (!shift && !ctrl) span_clear(e);

    /* A corner wins over the wall it ends, because a reshape press would
       otherwise swallow the small handle. */
    if (!shift && !ctrl) {
        Vec2 vertex = world;
        int wall = nearest_vertex(e, mouse, &vertex);
        if (wall >= 0 && !e->level.walls[wall].locked) {
            begin_vertex(e, wall, vertex);
            e->gesture.press_screen = v2(mouse.x, mouse.y);
            return;
        }
    }

    Hit hit = hit_test(e, world);
    if (hit.kind == HIT_NONE) {
        /* A press just outside a stair box still grabs its edge, the way a wall
           line is grabbable from either side. */
        if (!shift && !ctrl) {
            for (int i = e->level.stairs_count - 1; i >= 0; i--) {
                int edge = stair_edge_at(e, i, mouse);
                if (edge == STAIR_EDGE_NONE) continue;
                sel_only(e, SEL_STAIRS, i);
                if (e->level.stairs[i].locked) {
                    set_status(e, false, "Selection is locked; press L to unlock");
                    return;
                }
                cancel_gesture(e);
                begin_stairs_resize(e, i, edge, world);
                e->gesture.press_screen = v2(mouse.x, mouse.y);
                return;
            }
        }
        begin_marquee(e, world, mouse, shift);
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
    e->gesture.press_screen = v2(mouse.x, mouse.y);
    e->gesture.collapse = target;
    e->gesture.collapse_on_release = in_group;
    if (selection_has_locked(e)) {
        set_status(e, false, "Selection is locked; press L to unlock");
        return;
    }

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
    int idx = level_add_label(&e->level, p, "Label", e->label_size);
    if (idx < 0) {
        set_status(e, true, "Could not add label: level is full");
        return;
    }
    sel_only(e, SEL_LABEL, idx);
    e->focus_label_input = true;
    commit(e, "Label added");
    auto_select_after_action(e);
}

/* A Line press either continues an open chain or waits for the release so a
   plain drag stays a no-op. */
static void line_press(Editor* e, Vec2 world, ImVec2 mouse) {
    Gesture* g = &e->gesture;
    bool had_chain = g->kind == GESTURE_CHAIN;

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

/* The wall under the pointer answers. A click places a white door centred on
   the projected point. No marquee and no menu. */
static void begin_door_click(Editor* e, Vec2 world) {
    int wall = -1;
    float t = 0.0f;
    if (!door_hover(e, world, &wall, &t)) {
        set_status(e, false, "Hover a wall to place a door");
        return;
    }
    int idx = level_add_door(&e->level, e->level.walls[wall].id, t, e->door_width);
    if (idx < 0) {
        set_status(e, true, "Could not add the door: level is full");
        return;
    }
    sel_only(e, SEL_DOOR, idx);
    commit(e, "Door added");
    auto_select_after_action(e);
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

    /* In image-transform mode the canvas drives the image transform instead
       of the camera or a tool. */
    if (hovered && !e->play_mode && e->image_onion_edit &&
        e->image_onion_on && e->image_onion_texture) {
        if (io->MouseWheel != 0.0f) {
            scale_image_onion_about(e, world,
                                    powf(1.18f, io->MouseWheel));
            e->camera_pan_active = false;
            return;
        }
        if (!e->pan_active && io->MouseDown[0]) {
            if (!e->image_onion_dragging) {
                e->image_onion_dragging = true;
                e->image_onion_drag_last = world;
            }
            e->image_onion_offset = v2_add(
                e->image_onion_offset,
                v2_sub(world, e->image_onion_drag_last));
            e->image_onion_drag_last = world;
            return;
        }
        if (e->image_onion_dragging && !io->MouseDown[0]) {
            e->image_onion_dragging = false;
            return;
        }
    }

    if (hovered && io->MouseWheel != 0.0f) {
        Vec2 before = screen_to_world(e, io->MousePos);
        float old_zoom = e->camera.zoom;
        float nz = clampf(old_zoom * powf(1.18f, io->MouseWheel), 0.15f, 6.0f);
        if (fabsf(nz - old_zoom) > 0.00001f) {
            e->camera.zoom = nz;
            Vec2 after = screen_to_world(e, io->MousePos);
            e->camera.center = v2_add(e->camera.center, v2_sub(before, after));
            e->camera_pan_active = false;
            if (e->play_mode && e->follow_player && e->level.player_present) {
                e->camera.center = e->level.player_pos;
            }
        }
    }

    bool space = igIsKeyDown(ImGuiKey_Space);
    if (!e->pan_active && hovered &&
        (io->MouseDown[2] || (space && io->MouseDown[0] && !gesture_active(e)))) {
        e->pan_active = true;
        e->follow_player = false;
        e->camera_pan_active = false;
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
    update_shape_drag(e, have_pointer);

    if (hovered && igIsMouseClicked(0) && !e->pan_active) {
        if (e->play_mode) {
            player_click_play(e, world, igIsMouseDoubleClicked(0) != 0);
            return;
        }
        if (e->player_place_pending) {
            place_player_click(e, world);
            return;
        }
        switch (e->tool) {
            case TOOL_SELECT: begin_select(e, world, io->MousePos); break;
            case TOOL_LINE: line_press(e, world, io->MousePos); break;
            case TOOL_RECT: begin_rect_drag(e, world, io->MousePos); break;
            case TOOL_DOOR: begin_door_click(e, world); break;
            case TOOL_STAIRS: begin_stairs_drag(e, world, io->MousePos); break;
            case TOOL_LABEL: begin_label_click(e, world); break;
            default: break;
        }
    }
}


/* The in-game map screen puts a vertical 3F / 2F / 1F stack to the right of
   the plan. The editor draws the same stack as a canvas overlay, so it is
   present and clickable in edit and play mode. */
#define FLOOR_TAB_W 52.0f
#define FLOOR_TAB_H 46.0f
#define FLOOR_TAB_GAP 2.0f
#define FLOOR_TAB_RIGHT 18.0f
/* Each floor tab gets an onion-skin checkbox on its left edge. */
#define FLOOR_CHECK_W 18.0f
#define FLOOR_CHECK_GAP 8.0f

static bool floor_tab_rect(const Editor* e, int index, ImVec2* out_min,
                           ImVec2* out_max) {
    if (e->floor_count <= 1 || index < 0 || index >= e->floor_count) return false;
    float total = e->floor_count * FLOOR_TAB_H +
                  (e->floor_count - 1) * FLOOR_TAB_GAP;
    float x1 = e->canvas.max.x - FLOOR_TAB_RIGHT;
    float x0 = x1 - FLOOR_TAB_W;
    float y0 = (e->canvas.min.y + e->canvas.max.y) * 0.5f - total * 0.5f;
    /* Slot 0 is 1F at the bottom, matching the reference stack. */
    int row = e->floor_count - 1 - index;
    float top = y0 + row * (FLOOR_TAB_H + FLOOR_TAB_GAP);
    *out_min = (ImVec2){ x0, top };
    *out_max = (ImVec2){ x1, top + FLOOR_TAB_H };
    return true;
}

static bool floor_check_rect(const Editor* e, int index, ImVec2* out_min,
                            ImVec2* out_max) {
    ImVec2 a, b;
    if (!floor_tab_rect(e, index, &a, &b)) return false;
    float cy = (a.y + b.y) * 0.5f;
    *out_min = (ImVec2){ a.x - FLOOR_CHECK_GAP - FLOOR_CHECK_W,
                         cy - FLOOR_CHECK_W * 0.5f };
    *out_max = (ImVec2){ a.x - FLOOR_CHECK_GAP,
                         cy + FLOOR_CHECK_W * 0.5f };
    return true;
}

static int floor_check_at(const Editor* e, ImVec2 p) {
    for (int i = 0; i < e->floor_count; i++) {
        ImVec2 a, b;
        if (!floor_check_rect(e, i, &a, &b)) continue;
        if (p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y) return i;
    }
    return -1;
}

static int floor_tab_at(const Editor* e, ImVec2 p) {
    for (int i = 0; i < e->floor_count; i++) {
        ImVec2 a, b;
        if (!floor_tab_rect(e, i, &a, &b)) continue;
        if (p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y) return i;
    }
    return -1;
}

static void draw_floor_selector(Editor* e, ImVec2 mouse, bool have_pointer) {
    if (e->floor_count <= 1) return;
    ImDrawList* dl = igGetWindowDrawList();
    int hover = have_pointer ? floor_tab_at(e, mouse) : -1;
    int check_hover = have_pointer && !e->play_mode
                          ? floor_check_at(e, mouse)
                          : -1;
    ImFont* font = igGetFont();
    for (int i = 0; i < e->floor_count; i++) {
        ImVec2 a, b;
        if (!floor_tab_rect(e, i, &a, &b)) continue;
        bool active = i == e->floor_index;
        bool lit = active || i == hover;
        ImU32 fill = active ? col32a(0x20252b, 245) : col32a(0x0d0f12, 210);
        ImU32 border = active ? col32(0xe9e9e9)
                              : (i == hover ? col32(0x8b9097)
                                            : col32(0x3a4048));
        ImDrawList_AddRectFilled(dl, a, b, fill);
        ImDrawList_AddRectEx(dl, a, b, border, 2.0f, active ? 2.0f : 1.0f, 0);
        const char* label = FLOOR_LABELS[i];
        ImVec2 sz = ImFont_CalcTextSizeA(font, 18.0f, FLT_MAX, 0.0f, label);
        ImVec2 p = { (a.x + b.x - sz.x) * 0.5f, (a.y + b.y - sz.y) * 0.5f };
        ImDrawList_AddTextImFontPtrEx(dl, font, 18.0f, p,
                                      lit ? col32(0xf3f3f3) : col32(0x9aa0a7),
                                      label, NULL, 0.0f, NULL);

        ImVec2 ca, cb;
        if (e->play_mode) continue;
        if (!floor_check_rect(e, i, &ca, &cb)) continue;
        bool checked = e->onion_floor == i;
        bool check_lit = checked || i == check_hover;
        ImU32 box_fill = checked ? col32a(ACCENT_UI, 230)
                                 : col32a(0x0d0f12, 210);
        ImU32 box_border = checked ? col32(ACCENT_COL)
                                   : (check_lit ? col32(0x8b9097)
                                                : col32(0x3a4048));
        ImDrawList_AddRectFilled(dl, ca, cb, box_fill);
        ImDrawList_AddRectEx(dl, ca, cb, box_border, 3.0f, 1.0f, 0);
        if (checked) {
            ImVec2 c0 = { ca.x + 4.0f, ca.y + 9.0f };
            ImVec2 c1 = { ca.x + 7.0f, ca.y + 13.0f };
            ImVec2 c2 = { ca.x + 14.0f, ca.y + 5.0f };
            ImDrawList_AddLineEx(dl, c0, c1, col32(0xf3f3f3), 2.2f, 0);
            ImDrawList_AddLineEx(dl, c1, c2, col32(0xf3f3f3), 2.2f, 0);
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

    bool over_floors = false;
    if (have_pointer && e->floor_count > 1) {
        int check = e->play_mode ? -1 : floor_check_at(e, mouse);
        int tab = floor_tab_at(e, mouse);
        if (check >= 0) {
            over_floors = true;
            if (igIsMouseClicked(0)) {
                e->onion_floor = e->onion_floor == check ? -1 : check;
            }
        } else if (tab >= 0) {
            over_floors = true;
            if (igIsMouseClicked(0)) enter_floor(e, tab);
        }
    }
    handle_canvas_input(e, hovered && inside && !over_floors, have_pointer);

    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_AddRectFilledEx(dl, v2_to_im(e->canvas.min), v2_to_im(e->canvas.max),
                               col32(CANVAS_BG), 0.0f, 0);
    draw_grid(e);
    draw_image_onion(e);
    draw_onion_skin(e);
    draw_nav_hover(e);
    draw_shape_preview(e);
    draw_stairs(e);
    draw_walls(e);
    draw_doors(e);
    draw_spans(e);
    draw_vertex_handles(e);
    if (e->tool == TOOL_DOOR && e->cursor_valid && !gesture_active(e)) {
        draw_door_ghost(e);
    }
    draw_labels(e);
    draw_player_path(e);
    draw_waypoints(e);
    draw_player(e);

    if (have_pointer && e->gesture.kind == GESTURE_CHAIN) draw_chain_preview(e, mouse);
    draw_selection(e);
    draw_marquee(e);
    draw_floor_selector(e, mouse, have_pointer);
    draw_status_strip(e);
    /* Frame rate sits in the top right corner of the canvas, away from the
       status strip, so it never moves while the status text changes. */
    {
        char fps[32];
        snprintf(fps, sizeof fps, "%.0f fps", (double)igGetIO()->Framerate);
        float w = igCalcTextSize(fps).x;
        ImVec2 p = { e->canvas.max.x - w - 10.0f, e->canvas.min.y + 6.0f };
        ImDrawList_AddTextImFontPtrEx(dl, igGetFont(), 13.0f, p,
                                      col32(0x5abf6a), fps, NULL, 0.0f, NULL);
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
                save_all_floors(e);
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
            bool undo_ok = history_can_undo(&e->floor_history[e->floor_index]) && !busy;
            bool redo_ok = history_can_redo(&e->floor_history[e->floor_index]) && !busy;
            if (igMenuItemEx("Undo", "Ctrl+Z", false, undo_ok)) do_undo(e);
            if (igMenuItemEx("Redo", "Ctrl+Shift+Z", false, redo_ok)) do_redo(e);
            igSeparator();
            bool has_marks = e->span_count > 0;
            if (igMenuItemEx("Delete", "Del", false,
                             has_marks || sel_count(&e->selection) > 0)) {
                delete_selection(e);
            }
            if (igMenuItemEx("Deselect", "Esc", false,
                             has_marks || sel_count(&e->selection) > 0)) {
                sel_clear(e);
                span_clear(e);
            }
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

static bool dir_button(const char* label, bool active, float w) {
    igPushStyleColorImVec4(ImGuiCol_Button,
                           active ? v4c(0x2b3f50, 1.0f) : v4c(FRAME_BG, 1.0f));
    igPushStyleColorImVec4(ImGuiCol_ButtonHovered,
                           active ? v4c(0x34506a, 1.0f) : v4c(0x24282e, 1.0f));
    igPushStyleColorImVec4(ImGuiCol_ButtonActive,
                           active ? v4c(0x3b5c78, 1.0f) : v4c(0x2b3037, 1.0f));
    bool pressed = igButtonEx(label, (ImVec2){ w, 22.0f });
    igPopStyleColorEx(3);
    return pressed;
}

static void draw_left_panel(Editor* e) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;
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

    /* The Q lock row is always present so the panel does not jump while the
       key is held; it lights up when the drawing tool will stay active. */
    bool locked = q_lock_held(e);
    igPushStyleColorImVec4(ImGuiCol_Text, v4c(locked ? ACCENT_COL : MUTED_COL, 1.0f));
    igTextUnformatted(locked ? "Q lock: on" : "Q lock: off");
    igPopStyleColorEx(1);

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
    igCheckbox("Snap objects", &e->snap_objects);

    switch (e->tool) {
        case TOOL_LINE:
            igDummy((ImVec2){ 0.0f, 3.0f });
            small_note("Click to add a wall, then click the first point or press Enter to close the chain.");
            break;
        case TOOL_RECT:
            igDummy((ImVec2){ 0.0f, 3.0f });
            small_note("Drag a grid-aligned rectangle. Overlapping rectangles union; one that only touches or sits fully inside keeps its own outline.");
            break;
        case TOOL_DOOR:
            igDummy((ImVec2){ 0.0f, 3.0f });
            igSetNextItemWidth(84.0f);
            igSliderFloatEx("Door width", &e->door_width, DOOR_MIN_WIDTH, 200.0f,
                            "%.0f", 0);
            small_note("Hover a wall and click to place a white door at the panel width.");
            break;
        case TOOL_STAIRS: {
            igDummy((ImVec2){ 0.0f, 3.0f });
            float full = igGetContentRegionAvail().x;
            if (dir_button("Horizontal", e->stair_dir == STAIRS_H, full)) {
                e->stair_dir = STAIRS_H;
            }
            if (dir_button("Vertical", e->stair_dir == STAIRS_V, full)) {
                e->stair_dir = STAIRS_V;
            }
            igSetNextItemWidth(84.0f);
            igSliderInt("Steps", &e->stair_steps, STAIR_MIN_STEPS,
                        STAIR_MAX_STEPS);
            igCheckbox("Flip", &e->stair_flip);
            int fade = (int)e->stair_fade;
            igSetNextItemWidth(104.0f);
            if (igCombo("Falloff", &fade, "Linear\0Ease in\0Ease out\0Smooth\0")) {
                e->stair_fade = (StairFade)fade;
            }
            igSetNextItemWidth(84.0f);
            igSliderFloatEx("Min", &e->stair_min_alpha, 0.0f, 1.0f, "%.2f", 0);
            igSetNextItemWidth(84.0f);
            igSliderFloatEx("Max", &e->stair_max_alpha, 0.0f, 1.0f, "%.2f", 0);
            if (e->stair_min_alpha > e->stair_max_alpha) {
                float swap = e->stair_min_alpha;
                e->stair_min_alpha = e->stair_max_alpha;
                e->stair_max_alpha = swap;
            }
            small_note("Drag a rectangle for the run. Flip reverses which end the fade starts from.");
            break;
        }
        case TOOL_LABEL:
            igDummy((ImVec2){ 0.0f, 3.0f });
            igSetNextItemWidth(84.0f);
            igSliderFloatEx("Label size", &e->label_size, 8.0f, 48.0f, "%.1f", 0);
            small_note("Click the canvas to place a label, then type its text.");
            break;
        case TOOL_SELECT: {
            igDummy((ImVec2){ 0.0f, 3.0f });
            igCheckbox("Marquee next drag", &e->marquee_armed);
            small_note("Press M, then drag, to start a marquee from inside a shape. A drag on empty canvas also starts one. Picks wall sections; press Delete to cut them.");
            break;
        }
        case TOOL_COUNT:
        default:
            break;
    }
    if (!e->play_mode) {
        igDummy((ImVec2){ 0.0f, 8.0f });
        heading("Image onion");
        if (e->image_onion_texture) {
            igCheckbox("Show image", &e->image_onion_on);
            igSetNextItemWidth(84.0f);
            igSliderFloatEx("Opacity", &e->image_onion_alpha, 0.05f, 1.0f, "%.2f",
                            0);
            igCheckbox("Transform", &e->image_onion_edit);
            igSameLineEx(0.0f, 6.0f);
            if (igButtonEx("Reset", (ImVec2){ 56.0f, 0.0f })) {
                e->image_onion_offset = v2(0.0f, 0.0f);
                e->image_onion_scale = 1.0f;
                e->image_onion_dragging = false;
                set_status(e, false, "Image transform reset");
            }
            float offset[2] = { e->image_onion_offset.x,
                                e->image_onion_offset.y };
            igSetNextItemWidth(112.0f);
            if (igDragFloat2Ex("Move", offset, 1.0f, -20000.0f, 20000.0f,
                               "%.1f", 0)) {
                e->image_onion_offset = v2(offset[0], offset[1]);
            }
            igSetNextItemWidth(84.0f);
            igSliderFloatEx("Scale", &e->image_onion_scale, 0.05f, 20.0f,
                            "%.2f", 0);
            if (e->image_onion_edit) {
                small_note("Drag the canvas to move the image; the wheel scales it.");
            }
            char image_note[640];
            snprintf(image_note, sizeof image_note, "%s (%d x %d)",
                     path_leaf(e->image_onion_path), e->image_onion_w,
                     e->image_onion_h);
            small_note(image_note);
            if (igButtonEx("Replace image...", (ImVec2){ -FLT_MIN, 24.0f })) {
                e->request_modal = MODAL_IMAGE;
                e->modal_error[0] = '\0';
                if (e->image_onion_path[0]) {
                    snprintf(e->image_path_buf, sizeof e->image_path_buf, "%s",
                             e->image_onion_path);
                }
            }
            if (igButtonEx("Clear image", (ImVec2){ -FLT_MIN, 24.0f })) {
                release_image_onion(e);
                set_status(e, false, "Image onion cleared");
            }
        } else {
            if (igButtonEx("Load image...", (ImVec2){ -FLT_MIN, 24.0f })) {
                e->request_modal = MODAL_IMAGE;
                e->modal_error[0] = '\0';
                e->image_path_buf[0] = '\0';
            }
            small_note("Pick a PNG, JPEG, BMP, TGA, or GIF, or drop one on the window.");
        }
    }
    if (e->floor_count > 1 && !e->play_mode) {
        igDummy((ImVec2){ 0.0f, 8.0f });
        heading("Floor onion");
        if (e->onion_floor >= 0 && e->onion_floor < e->floor_count) {
            igText("Showing %s under %s", FLOOR_LABELS[e->onion_floor],
                   FLOOR_LABELS[e->floor_index]);
        } else {
            igTextDisabled("No floor checked");
        }
        igSetNextItemWidth(84.0f);
        igSliderFloatEx("Opacity", &e->onion_alpha, 0.05f, 1.0f, "%.2f", 0);
        small_note("Check a box left of a floor tab to overlay that floor.");
    }
    igDummy((ImVec2){ 0.0f, 8.0f });
    heading("Player");
    bool play = e->play_mode;
    if (igCheckbox("Play mode (P)", &play)) set_play_mode(e, play);
    if (e->level.player_present) {
        if (igButtonEx("Remove player", (ImVec2){ -FLT_MIN, 24.0f })) {
            level_remove_player(&e->level);
            e->player_moving = false;
            e->player_path_count = 0;
            clear_waypoints(e);
            e->follow_player = false;
            set_status(e, false, "Player removed");
        }
        small_note("Click to add waypoints; double-click to walk. C recenters at the current zoom.");
    } else if (e->player_place_pending) {
        if (igButtonEx("Cancel placement", (ImVec2){ -FLT_MIN, 24.0f })) {
            e->player_place_pending = false;
            set_status(e, false, "Player placement cancelled");
        }
        small_note("Click a walkable nav cell to place the player.");
    } else {
        if (igButtonEx("Place player", (ImVec2){ -FLT_MIN, 24.0f })) {
            e->nav_ready = nav_build(&e->nav, &e->level, NAV_CELL_SIZE,
                                     PLAYER_RADIUS);
            e->player_place_pending = true;
            set_status(e, e->nav_ready,
                       e->nav_ready
                           ? "Click a green nav cell to place the player"
                           : "No navigation grid for this level");
        }
        small_note("Or enter play mode and click the map.");
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
    igText("Edge %d of %d", edge, walls);
    igDummy((ImVec2){ 0.0f, 4.0f });
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
    if (dir_button("Horizontal", s->dir == STAIRS_H, dir_half)) {
        level_set_stairs(&e->level, idx, STAIRS_H, s->steps);
        commit(e, "Stairs direction changed");
    }
    igSameLineEx(0.0f, 6.0f);
    if (dir_button("Vertical", s->dir == STAIRS_V, dir_half)) {
        level_set_stairs(&e->level, idx, STAIRS_V, s->steps);
        commit(e, "Stairs direction changed");
    }
    int steps = s->steps;
    igSetNextItemWidth(128.0f);
    if (igSliderInt("Steps", &steps, STAIR_MIN_STEPS, STAIR_MAX_STEPS)) {
        level_set_stairs(&e->level, idx, s->dir, steps);
    }
    if (igIsItemDeactivatedAfterEdit()) commit(e, "Stairs step count changed");
    bool flip = s->bottom;
    if (igCheckbox("Flip", &flip)) {
        s->bottom = flip;
        commit(e, "Stairs flip changed");
    }
    int fade = (int)s->fade;
    igSetNextItemWidth(128.0f);
    if (igCombo("Falloff", &fade, "Linear\0Ease in\0Ease out\0Smooth\0")) {
        level_set_stairs_fade(&e->level, idx, (StairFade)fade, s->min_alpha,
                              s->max_alpha);
        commit(e, "Stairs falloff changed");
    }
    float min_alpha = s->min_alpha;
    igSetNextItemWidth(128.0f);
    if (igSliderFloatEx("Min opacity", &min_alpha, 0.0f, 1.0f, "%.2f", 0)) {
        level_set_stairs_fade(&e->level, idx, s->fade, min_alpha, s->max_alpha);
    }
    if (igIsItemDeactivatedAfterEdit()) commit(e, "Stairs opacity changed");
    float max_alpha = s->max_alpha;
    igSetNextItemWidth(128.0f);
    if (igSliderFloatEx("Max opacity", &max_alpha, 0.0f, 1.0f, "%.2f", 0)) {
        level_set_stairs_fade(&e->level, idx, s->fade, s->min_alpha, max_alpha);
    }
    if (igIsItemDeactivatedAfterEdit()) commit(e, "Stairs opacity changed");
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
    bool have_selection = sel_count(&e->selection) > 0;
    bool any_locked = have_selection && selection_has_locked(e);
    if (have_selection) {
        bool locked = selection_all_locked(e);
        if (igCheckbox("Lock selection", &locked)) {
            set_selection_locked(e, locked);
            set_status(e, false, locked ? "Selection locked"
                                        : "Selection unlocked");
        }
        if (any_locked) {
            small_note("Locked objects stay put while you select and edit others. Press L or the checkbox to unlock the selected objects.");
        }
    }
    igBeginDisabled(any_locked);
    SelRef single;
    if (sel_single(&e->selection, &single)) {
        switch (single.kind) {
            case SEL_WALL: inspector_edge(e, single.index); break;
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
        int visible_doors = 0;
        for (int i = 0; i < e->level.door_count; i++) {
            if (!e->level.doors[i].hidden) visible_doors++;
        }
        igText("Walls   %d", e->level.wall_count);
        igText("Shapes  %d", e->level.shape_count);
        igText("Doors   %d", visible_doors);
        igText("Labels  %d", e->level.label_count);
        igText("Stairs  %d", e->level.stairs_count);
        igDummy((ImVec2){ 0.0f, 4.0f });
        heading("Controls");
        igPushFontFloat(igGetFont(), 12.0f);
        igTextDisabled("1 Select, 2 Line, 3 Rectangle, 4 Door, 5 Stairs, 6 Label");
        igTextDisabled("F fit level, G toggle grid");
        igTextDisabled("Shift-click adds; Ctrl-click toggles");
        igTextDisabled("Drag a box to mark wall sections for cutting");
        igTextDisabled("Drag selected edges to reshape them");
        igTextDisabled("Drag a wall corner to resize the walls that meet there");
        igTextDisabled("Drag a stair edge to resize its footprint");
        igTextDisabled("Drag a group to move walls, doors, labels, or stairs");
        igTextDisabled("Drag a door to slide; drag an end to resize");
        igTextDisabled("Delete removes every selected object");
        igTextDisabled("L locks or unlocks the selection");
        igTextDisabled("Esc deselects then cancels a draft");
        igTextDisabled("Ctrl+Z undo, Ctrl+Shift+Z redo");
        igTextDisabled("Ctrl+S save, Ctrl+O open");
        igTextDisabled("P play mode   C center player   Esc stop");
        igTextDisabled("Play: click waypoints, double-click to walk");
        igTextDisabled("Middle-drag or Space+drag pans");
        igTextDisabled("Wheel zooms around the cursor");
        igTextDisabled("Ctrl+0 resets the zoom to 100 percent");
        igPopFont();
        igDummy((ImVec2){ 0.0f, 6.0f });
        igTextDisabled("%s", e->current_path);
    }
    igEndDisabled();
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
            memcpy(paths[j + 1], paths[j], 512);
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
        case MODAL_IMAGE: return "Image onion skin";
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
    igText("1 Select   2 Line   3 Rectangle   4 Door   5 Stairs   6 Label");
    igBullet();
    igText("A drawing action returns to Select unless Q is held.");
    igText("Canvas");
    igBullet();
    igText("Left click uses the active tool.");
    igBullet();
    igText("P toggles play mode. C recentres on the player at the current zoom.");
    igText("In play mode, click the map to place or walk; Esc stops the player.");
    igText("Middle-drag or Space+left-drag pans.");
    igBullet();
    igText("Wheel zooms around the cursor.");
    igText("Ctrl+0 resets the zoom to 100 percent.");
    igBullet();
    igText("F fits the level, G toggles the grid.");
    igText("Editing");
    igBullet();
    igText("Rectangle drags union overlapping rectangles. Touching rectangles keep a shared wall.");
    igBullet();
    igText("Door hovers a wall; click places a white door.");
    igBullet();
    igText("Stairs drags a footprint; Flip reverses the tread fade.");
    igBullet();
    igText("Delete removes every selected object.");
    igBullet();
    igText("Shift-click adds to the selection; Ctrl-click toggles an object.");
    igBullet();
    igText("Drag a box to select objects and mark wall sections.");
    igBullet();
    igText("Drag selected edges to reshape them.");
    igBullet();
    igText("Drag a wall corner to resize the walls that meet there.");
    igBullet();
    igText("Drag a group to move its walls, doors, labels, and stairs.");
    igBullet();
    igText("Drag a door to slide it; drag an end to resize.");
    igBullet();
    igText("Drag a stair edge to resize its footprint.");
    igBullet();
    igText("Esc deselects, then cancels a draft.");
    igBullet();
    igText("Enter closes a wall chain.");
    igBullet();
    igText("Backspace removes the last chain wall.");
    igText("Onion skins");
    igBullet();
    igText("Load an image in the left panel or drop one on the window.");
    igBullet();
    igText("Check a box beside a floor tab to overlay that floor.");
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
        } else if (e->modal == MODAL_IMAGE) {
            igText("Choose an image file");
            if (e->path_focus) igSetKeyboardFocusHere();
            ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue;
            if (e->path_focus) input_flags |= ImGuiInputTextFlags_AutoSelectAll;
            igSetNextItemWidth(360.0f);
            bool enter = igInputText("Path", e->image_path_buf,
                                     sizeof e->image_path_buf, input_flags);
            e->path_focus = false;
#if defined(__linux__)
            if (igButtonEx("Browse...", (ImVec2){ 120.0f, 28.0f })) {
                char picked[512];
                if (pick_image_path(picked, sizeof picked)) {
                    snprintf(e->image_path_buf, sizeof e->image_path_buf, "%s",
                             picked);
                    if (editor_load_onion_image(e, picked)) {
                        igCloseCurrentPopup();
                        e->modal = MODAL_NONE;
                    }
                }
            }
            igSameLineEx(0.0f, 6.0f);
#endif
            if (e->modal_error[0]) {
                igTextColored(v4c(ERROR_COL, 1.0f), "%s", e->modal_error);
            }
            igDummy((ImVec2){ 0.0f, 4.0f });
            bool do_action = enter;
            if (igButtonEx("Load", (ImVec2){ 120.0f, 28.0f })) do_action = true;
            igSameLineEx(0.0f, 6.0f);
            if (igButtonEx("Cancel", (ImVec2){ 100.0f, 28.0f })) {
                igCloseCurrentPopup();
                e->modal = MODAL_NONE;
            }
            if (do_action && e->modal != MODAL_NONE) {
                if (editor_load_onion_image(e, e->image_path_buf)) {
                    igCloseCurrentPopup();
                    e->modal = MODAL_NONE;
                }
            }
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

/* Ctrl+D copies every selected object one grid cell down-right and selects the
   copies, in one history step. */
static void duplicate_selection(Editor* e) {
    sel_prune(e);
    if (selection_has_locked(e)) {
        set_status(e, true, "Selection contains locked objects");
        return;
    }
    Selection* sel = &e->selection;
    if (sel_count(sel) == 0) {
        set_status(e, true, "Nothing selected to duplicate");
        return;
    }
    Vec2 off = v2(e->grid_size, e->grid_size);
    int made = 0;
    int wall_map[LEVEL_MAX_WALLS];
    for (int i = 0; i < LEVEL_MAX_WALLS; i++) wall_map[i] = -1;
    int old_door_count = e->level.door_count;

    for (int i = e->level.stairs_count - 1; i >= 0; i--) {
        if (!sel->stairs[i]) continue;
        const Stairs src = e->level.stairs[i];
        Rect r = level_stairs_rect(&src);
        Rect copy = { v2_add(r.min, off), v2_add(r.max, off) };
        int idx = level_add_stairs(&e->level, copy, src.dir, src.steps);
        if (idx < 0) break;
        /* Copy every creation setting, including falloff and both opacities. */
        e->level.stairs[idx] = src;
        e->level.stairs[idx].pos = copy.min;
        e->level.stairs[idx].locked = false;
        sel_add(e, SEL_STAIRS, idx);
        made++;
    }
    for (int i = e->level.label_count - 1; i >= 0; i--) {
        if (!sel->labels[i]) continue;
        const Label* src = &e->level.labels[i];
        int idx = level_add_label(&e->level, v2_add(src->pos, off), src->text,
                                  src->size);
        if (idx < 0) break;
        sel_add(e, SEL_LABEL, idx);
        made++;
    }
    for (int i = e->level.shape_count - 1; i >= 0; i--) {
        const Shape* shape = &e->level.shapes[i];
        int indices[LEVEL_MAX_WALLS];
        int count = level_shape_wall_indices(&e->level, shape->id, indices,
                                             LEVEL_MAX_WALLS);
        if (count <= 0) continue;
        bool hit = false;
        for (int k = 0; k < count; k++) {
            if (sel->walls[indices[k]]) { hit = true; break; }
        }
        if (!hit) continue;
        ShapeId copy = level_add_shape(&e->level);
        if (copy == 0) break;
        for (int k = 0; k < count; k++) {
            int old_index = indices[k];
            const Wall* w = &e->level.walls[old_index];
            int new_index = level_add_wall_to_shape(
                &e->level, copy, v2_add(w->a, off), v2_add(w->b, off));
            if (new_index >= 0) wall_map[old_index] = new_index;
        }
        if (shape->closed) level_close_shape(&e->level, copy);
        int copy_idx[LEVEL_MAX_WALLS];
        int copy_count = level_shape_wall_indices(&e->level, copy, copy_idx,
                                                  LEVEL_MAX_WALLS);
        for (int k = 0; k < copy_count; k++) sel_add(e, SEL_WALL, copy_idx[k]);
        made++;
    }

    /* A selected door whose wall wasn't part of a copied shape still gets a
       usable duplicate: copy its host wall as an open shape under the door. */
    for (int d = 0; d < old_door_count; d++) {
        if (!sel->doors[d]) continue;
        int old_wall = level_find_wall_index(&e->level, e->level.doors[d].wall);
        if (old_wall < 0 || wall_map[old_wall] >= 0) continue;
        const Wall src = e->level.walls[old_wall];
        ShapeId copy = level_add_shape(&e->level);
        if (copy == 0) break;
        int new_wall = level_add_wall_to_shape(&e->level, copy,
                                               v2_add(src.a, off),
                                               v2_add(src.b, off));
        if (new_wall < 0) {
            level_delete_shape(&e->level, copy);
            continue;
        }
        wall_map[old_wall] = new_wall;
    }

    /* Doors follow their duplicated walls, and a selected door is selected
       again on the copy. */
    for (int d = 0; d < old_door_count; d++) {
        const Door src = e->level.doors[d];
        int old_wall = level_find_wall_index(&e->level, src.wall);
        if (old_wall < 0 || wall_map[old_wall] < 0) continue;
        int idx = level_add_door(&e->level, e->level.walls[wall_map[old_wall]].id,
                                 src.t, src.width);
        if (idx < 0) break;
        if (sel->doors[d]) sel_add(e, SEL_DOOR, idx);
        made++;
    }

    if (made == 0) {
        set_status(e, true, "Nothing selected to duplicate");
        return;
    }
    if (made == 1) commit(e, "Duplicated 1 object");
    else {
        char msg[64];
        snprintf(msg, sizeof msg, "Duplicated %d objects", made);
        commit(e, msg);
    }
}

static void handle_shortcuts(Editor* e) {
    ImGuiIO* io = igGetIO();
    if (e->modal != MODAL_NONE) return;
    if (io->WantTextInput) return;

    if (igIsKeyPressedEx(ImGuiKey_P, false)) {
        set_play_mode(e, !e->play_mode);
        return;
    }
    if (e->play_mode) {
        if (io->KeyCtrl && igIsKeyPressedEx(ImGuiKey_S, false)) {
            if (io->KeyShift) open_path_modal(e, MODAL_SAVE_AS);
            else save_all_floors(e);
            return;
        }
        if (io->KeyCtrl) return;
        if (igIsKeyPressedEx(ImGuiKey_C, false)) recenter_player_camera(e);
        else if (igIsKeyPressedEx(ImGuiKey_Escape, false)) {
            e->player_moving = false;
            e->player_path_count = 0;
            clear_waypoints(e);
            set_status(e, false, "Player stopped");
        } else if (igIsKeyPressedEx(ImGuiKey_F, false)) {
            e->fit_pending = true;
        } else if (igIsKeyPressedEx(ImGuiKey_G, false)) {
            e->show_grid = !e->show_grid;
        } else if (igIsKeyPressedEx(ImGuiKey_S, false)) {
            e->snap = !e->snap;
        } else if (igIsKeyPressedEx(ImGuiKey_H, false)) {
            open_path_modal(e, MODAL_HELP);
        }
        return;
    }
    if (e->player_place_pending &&
        igIsKeyPressedEx(ImGuiKey_Escape, false)) {
        e->player_place_pending = false;
        set_status(e, false, "Player placement cancelled");
        return;
    }
    if (igIsKeyPressedEx(ImGuiKey_C, false)) {
        recenter_player_camera(e);
        return;
    }

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
        if (igIsKeyPressedEx(ImGuiKey_0, false)) {
            /* Reset to one screen pixel per world unit, keeping the world point
               under the canvas centre fixed so the view does not jump. */
            e->camera.zoom = 1.0f;
            set_status(e, false, "Zoom 100%%");
            return;
        }
        if (igIsKeyPressedEx(ImGuiKey_S, false)) {
            if (shift) open_path_modal(e, MODAL_SAVE_AS);
            else save_all_floors(e);
            return;
        }
        if (igIsKeyPressedEx(ImGuiKey_D, false)) {
            duplicate_selection(e);
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
    if (igIsKeyPressedEx(ImGuiKey_S, false)) {
        e->snap = !e->snap;
        set_status(e, false, e->snap ? "Snap on" : "Snap off");
    }
    if (igIsKeyPressedEx(ImGuiKey_G, false)) e->show_grid = !e->show_grid;
    if (igIsKeyPressedEx(ImGuiKey_L, false)) toggle_selection_lock(e);
    if (igIsKeyPressedEx(ImGuiKey_M, false) && e->tool == TOOL_SELECT) {
        e->marquee_armed = !e->marquee_armed;
        set_status(e, false, e->marquee_armed ? "Marquee armed: drag to select"
                                              : "Marquee cancelled");
    }
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
    if (igIsKeyPressedEx(ImGuiKey_Q, false)) q_lock_toggle(e);
    if (igIsKeyPressedEx(ImGuiKey_Enter, false)) {
        if (e->gesture.kind == GESTURE_CHAIN) close_wall_chain(e, true);
    }
    if (igIsKeyPressedEx(ImGuiKey_Escape, false)) {
        if (e->span_count > 0) {
            span_clear(e);
            set_status(e, false, "Sections cleared");
            return;
        }
        if (e->tool == TOOL_LINE && !gesture_is_drag(e)) {
            cancel_transient(e);
            e->tool = TOOL_SELECT;
            set_status(e, false, "Select tool");
            return;
        }
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


/* ---- floors -------------------------------------------------------------
   The reference building has three floors. Each floor is a complete Level in
   its own file, drawn at the same world scale, so a stairwell occupies the
   same rectangle on every floor. The selector switches the active document
   without fitting the camera, exactly like the in-game map screen. */

static void sync_active_floor(Editor* e) {
    if (e->floor_count <= 0) return;
    if (e->floor_index < 0 || e->floor_index >= FLOOR_MAX) return;
    store_onion_state(e);
    e->floor_levels[e->floor_index] = e->level;
}

static bool enter_floor(Editor* e, int index) {
    if (index < 0 || index >= e->floor_count || index >= FLOOR_MAX) return false;
    if (index == e->floor_index) return false;
    if (!e->floor_loaded[index]) {
        set_status(e, true, "Floor %s has no level file", FLOOR_LABELS[index]);
        return false;
    }
    cancel_gesture(e);
    sel_clear(e);
    span_clear(e);

    bool was_play = e->play_mode;
    /* The selector changes floors, not the camera scale. Keep the current
       zoom so switching floors does not resize the plan under the user. */
    float keep_zoom = e->camera.zoom;

    sync_active_floor(e);
    e->floor_index = index;
    e->level = e->floor_levels[index];
    restore_onion_state(e);
    snprintf(e->current_path, sizeof e->current_path, "%s", e->floor_paths[index]);
    snprintf(e->path_buf, sizeof e->path_buf, "%s", e->floor_paths[index]);

    e->nav_ready = false;
    e->preview_count = 0;
    e->camera_pan_active = false;

    /* Viewing a floor never moves the player. Only the explicit Place player
       action changes player_floor. The pan position also persists across the
       switch; the camera recentres only when the player starts moving or C is
       pressed. */
    if (was_play && e->player_floor == index && e->level.player_present) {
        e->nav_ready = nav_build(&e->nav, &e->level, NAV_CELL_SIZE,
                                 PLAYER_RADIUS);
    }
    e->follow_player = false;
    e->camera.zoom = keep_zoom;
    set_status(e, false, "Floor %s", FLOOR_LABELS[index]);
    return true;
}

static bool save_all_floors(Editor* e) {
    if (e->floor_count <= 1) {
        if (!e->current_path[0]) {
            open_path_modal(e, MODAL_SAVE_AS);
            return false;
        }
        return save_level_as(e, e->current_path);
    }
    sync_active_floor(e);
    LevelError err;
    for (int i = 0; i < e->floor_count; i++) {
        if (!e->floor_loaded[i]) continue;
        if (!level_save(&e->floor_levels[i], e->floor_paths[i], &err)) {
            snprintf(e->modal_error, sizeof e->modal_error, "%s", err.message);
            set_status(e, true, "Save failed: %s", err.message);
            return false;
        }
    }
    set_status(e, false, "Saved %d floors", e->floor_count);
    return true;
}

Editor* editor_create(const char* startup_path) {
    Editor* e = calloc(1, sizeof *e);
    if (!e) return NULL;

    apply_theme();
    for (int i = 0; i < FLOOR_MAX; i++) {
        level_init(&e->floor_levels[i]);
        if (!history_init(&e->floor_history[i])) {
            set_status(e, true, "Undo history unavailable");
        }
    }
    level_init(&e->level);
    e->floor_count = 1;
    e->floor_index = 0;
    e->floor_loaded[0] = true;
    e->player_floor = -1;
    e->onion_floor = -1;
    e->onion_alpha = 0.25f;
    e->image_onion_on = false;
    e->image_onion_alpha = 0.35f;
    e->image_onion_offset = v2(0.0f, 0.0f);
    e->image_onion_scale = 1.0f;
    e->image_onion_edit = false;
    history_reset(&e->floor_history[e->floor_index], &e->level);

    e->camera.center = v2(0.0f, 0.0f);
    e->camera.zoom = 1.0f;
    e->tool = TOOL_SELECT;
    e->selection.primary.kind = SEL_NONE;
    e->selection.primary.index = -1;
    e->grid_size = 8.0f;
    e->snap = true;
    e->snap_objects = true;
    e->show_grid = true;
    e->marquee_armed = false;
    e->door_width = 10.0f;
    e->label_size = 10.0f;
    e->stair_dir = STAIRS_H;
    e->stair_steps = 10;
    e->stair_flip = false;
    e->stair_fade = STAIR_FADE_LINEAR;
    e->stair_min_alpha = STAIR_ALPHA_MIN;
    e->stair_max_alpha = STAIR_ALPHA_MAX;
    e->fit_pending = true;
    e->modal = MODAL_NONE;
    e->request_modal = MODAL_NONE;
    e->menu_height = 23.0f;
    sg_image_desc image_desc = { 0 };
    image_desc.width = PLAYER_SPRITE_W;
    image_desc.height = PLAYER_SPRITE_H;
    image_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    image_desc.data.ptr = PLAYER_SPRITE_RGBA;
    image_desc.data.size = sizeof PLAYER_SPRITE_RGBA;
    e->player_image = sg_make_image(&image_desc);
    sg_view_desc view_desc = { 0 };
    view_desc.texture.image = e->player_image;
    e->player_view = sg_make_view(&view_desc);
    sg_sampler_desc sampler_desc = { 0 };
    sampler_desc.min_filter = SG_FILTER_LINEAR;
    sampler_desc.mag_filter = SG_FILTER_LINEAR;
    sampler_desc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    sampler_desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    e->player_sampler = sg_make_sampler(&sampler_desc);
    e->player_texture = (ImTextureID)simgui_imtextureid_with_sampler(
        e->player_view, e->player_sampler);
    e->image_onion_sampler = sg_make_sampler(&sampler_desc);
    snprintf(e->current_path, sizeof e->current_path, "%s", "levels/re2-library.lvl");
    snprintf(e->path_buf, sizeof e->path_buf, "%s", e->current_path);

    const char* path = (startup_path && startup_path[0]) ? startup_path
                                                          : "levels/re2-library.lvl";
    bool loaded = false;
    /* A floor set is present when both companion files sit beside the 3F
       document. Any other startup path keeps the original single-level
       behavior. */
    const char* companions[FLOOR_MAX] = {
        "levels/re2-library-1f.lvl",
        "levels/re2-library-2f.lvl",
        path,
    };
    bool have_floors = !startup_path || startup_path[0] == '\0' ||
                       strcmp(path, "levels/re2-library.lvl") == 0;
    for (int i = 0; i < FLOOR_MAX && have_floors; i++) {
        have_floors = file_exists(companions[i]);
    }
    if (have_floors) {
        e->floor_count = FLOOR_MAX;
        for (int i = 0; i < FLOOR_MAX; i++) {
            LevelError err;
            snprintf(e->floor_paths[i], sizeof e->floor_paths[i], "%s",
                     companions[i]);
            if (level_load(&e->floor_levels[i], companions[i], &err)) {
                e->floor_loaded[i] = true;
                history_reset(&e->floor_history[i], &e->floor_levels[i]);
            } else {
                e->floor_loaded[i] = false;
                set_status(e, true, "Floor load failed: %s", err.message);
            }
        }
        e->player_floor = -1;
        for (int i = 0; i < FLOOR_MAX; i++) {
            if (e->floor_loaded[i] && e->floor_levels[i].player_present) {
                e->player_floor = i;
                break;
            }
        }
        /* A player record belongs to exactly one floor. */
        for (int i = 0; i < FLOOR_MAX; i++) {
            if (i != e->player_floor) e->floor_levels[i].player_present = false;
        }
        e->floor_index = FLOOR_DEFAULT;
        e->level = e->floor_levels[FLOOR_DEFAULT];
        snprintf(e->current_path, sizeof e->current_path, "%s",
                 e->floor_paths[FLOOR_DEFAULT]);
        snprintf(e->path_buf, sizeof e->path_buf, "%s", e->current_path);
        loaded = e->floor_loaded[FLOOR_DEFAULT];
    } else if (file_exists(path)) {
        LevelError err;
        if (level_load(&e->level, path, &err)) {
            loaded = true;
            e->player_floor = e->level.player_present ? 0 : -1;
            snprintf(e->current_path, sizeof e->current_path, "%s", path);
            snprintf(e->path_buf, sizeof e->path_buf, "%s", path);
            snprintf(e->floor_paths[0], sizeof e->floor_paths[0], "%s", path);
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

    history_reset(&e->floor_history[e->floor_index], &e->level);
    if (loaded) {
        set_status(e, false, "Loaded %s", e->current_path);
    } else if (!e->status[0]) {
        set_status(e, true, "No level loaded");
    }
    restore_onion_state(e);
    return e;
}


void editor_destroy(Editor* e) {
    if (!e) return;
    release_image_onion(e);
    if (e->image_onion_sampler.id) {
        sg_destroy_sampler(e->image_onion_sampler);
    }
    if (e->player_sampler.id) sg_destroy_sampler(e->player_sampler);
    if (e->player_view.id) sg_destroy_view(e->player_view);
    if (e->player_image.id) sg_destroy_image(e->player_image);
    for (int i = 0; i < FLOOR_MAX; i++) history_shutdown(&e->floor_history[i]);
    free(e);
}

void editor_frame(Editor* e) {
    if (!e) return;
    flush_retired_image_onion(e);
    ImGuiIO* io = igGetIO();
    e->display_w = io->DisplaySize.x;
    e->display_h = io->DisplaySize.y;

    draw_menu(e);

    float bottom = e->display_h;
    float canvas_x0 = PANEL_LEFT_W;
    float canvas_x1 = e->display_w - PANEL_RIGHT_W;
    if (canvas_x1 < canvas_x0 + 80.0f) canvas_x1 = canvas_x0 + 80.0f;
    e->canvas = rect_from_points(v2(canvas_x0, e->menu_height), v2(canvas_x1, bottom));

    update_player(e, io->DeltaTime);
    update_camera(e, io->DeltaTime);
    update_player_preview(e);
    draw_left_panel(e);
    draw_inspector(e);
    draw_canvas(e, io->MousePos, mouse_available(io->MousePos));
    handle_shortcuts(e);
    draw_modals(e);
}
