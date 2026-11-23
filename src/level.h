#ifndef LEVEL_H
#define LEVEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "geom.h"

#define LEVEL_MAX_WALLS 512
#define LEVEL_MAX_SHAPES 512
#define LEVEL_MAX_DOORS 256
#define LEVEL_MAX_LABELS 128
#define LEVEL_MAX_STAIRS 64
#define LEVEL_LABEL_LEN 64
#define LEVEL_ONION_PATH_LEN 512

#define DOOR_THICKNESS 3.0f
#define DOOR_MIN_WIDTH 8.0f
#define STAIR_STEP_DEPTH 10.0f
#define STAIR_WIDTH 44.0f
#define STAIR_MIN_SIZE 8.0f
#define STAIR_MIN_STEPS 2
#define STAIR_MAX_STEPS 48
#define STAIR_ALPHA_MIN 0.18f
#define STAIR_ALPHA_MAX 0.68f

typedef uint32_t WallId;
typedef uint32_t ShapeId;

/* A shape is one drawn wall outline: a chain of orthogonal walls that share
   endpoints. A rectangle is a shape of four walls. The outline is the only
   geometry the level stores, so a rectangle union, a reshape, and a save/load
   round trip all land on the same four walls. */
typedef struct {
    ShapeId id;
    bool closed;
} Shape;

typedef struct {
    WallId id;
    ShapeId shape;
    Vec2 a, b;
    bool locked;
    /* A drawn reference line that is not a collision wall: balcony rails,
       counters, shelving outlines. It renders like a wall but nav ignores it. */
    bool detail;
} Wall;

/* Doors are white and live on a wall: t is the centre parameter along the
   host and width is the world length of the bar. */
typedef struct {
    WallId wall;
    float t;
    float width;
    bool locked;
    /* Navigation-only opening. The reference has archways and open passages
       that are not white door bars; the editor still needs a gap in the wall
       for the nav grid, so those openings are stored but not drawn. */
    bool hidden;
} Door;

typedef struct {
    Vec2 pos;
    float size;
    char text[LEVEL_LABEL_LEN];
    bool locked;
} Label;

typedef enum {
    STAIRS_H = 0,
    STAIRS_V = 1,
} StairsDir;

/* How the tread opacity travels from the faint end to the bright end. */
typedef enum {
    STAIR_FADE_LINEAR = 0,
    STAIR_FADE_EASE_IN = 1,
    STAIR_FADE_EASE_OUT = 2,
    STAIR_FADE_SMOOTH = 3,
    STAIR_FADE_COUNT
} StairFade;

/* A stair footprint is an explicit box, so its edges can sit on the grid and
   be dragged. pos is the min corner. width and length are the world extents.
   dir picks which axis carries the treads; steps divides that run. bottom says
   the bottom of the run sits at the min end of that axis; the treads fade out
   toward it, and the panel's Flip setting toggles that end. */
typedef struct {
    Vec2 pos;
    float width;
    float length;
    int steps;
    StairsDir dir;
    bool bottom;
    StairFade fade;
    /* Opacity at the faint end and at the bright end, in 0..1. Flip chooses
       which end of the run each one lands on. */
    float min_alpha;
    float max_alpha;
    bool locked;
} Stairs;

/* Reference overlays belong to the floor document so saving and reopening a
   level restores the same skins. The image fit and transform are stored in
   world units; the path is the last field so it may contain spaces. */
typedef struct {
    bool image_present;
    char image_path[LEVEL_ONION_PATH_LEN];
    bool image_show;
    float image_alpha;
    Vec2 image_offset;
    float image_scale;
    Rect image_fit;
    int floor_index;
    float floor_alpha;
} OnionSkin;

typedef struct {
    Wall walls[LEVEL_MAX_WALLS];
    int wall_count;
    WallId next_wall_id;

    Shape shapes[LEVEL_MAX_SHAPES];
    int shape_count;
    ShapeId next_shape_id;

    Door doors[LEVEL_MAX_DOORS];
    int door_count;

    Label labels[LEVEL_MAX_LABELS];
    int label_count;

    Stairs stairs[LEVEL_MAX_STAIRS];
    int stairs_count;
    /* At most one player marker. heading is radians, 0 = up (-y). */
    bool player_present;
    Vec2 player_pos;
    float player_heading;
    OnionSkin onion;
} Level;

typedef struct {
    char message[256];
} LevelError;

void level_init(Level* level);
bool level_is_empty(const Level* level);
Rect level_bounds(const Level* level);

/* Walls are orthogonal by construction. level_add_wall rejects non-orthogonal
   or zero-length segments and returns the new index, or -1. It creates a new
   open shape that holds just this wall. */
int level_add_wall(Level* level, Vec2 a, Vec2 b);

ShapeId level_add_shape(Level* level);
Shape* level_shape_by_id(Level* level, ShapeId id);
const Shape* level_shape_by_id_const(const Level* level, ShapeId id);
/* Adds a wall to an existing shape. The wall must be orthogonal and non-zero.
   Returns the wall index, or -1. */
int level_add_wall_to_shape(Level* level, ShapeId shape, Vec2 a, Vec2 b);
/* Marks the shape closed when it has at least three walls whose endpoints
   form one connected loop. */
bool level_close_shape(Level* level, ShapeId id);
/* Deletes the shape, its walls, and every door hosted by those walls. */
bool level_delete_shape(Level* level, ShapeId id);
/* Wall indices of a shape, ordered by the order they were added. Returns the
   count, or -1. */
int level_shape_wall_indices(const Level* level, ShapeId id, int* out, int cap);
/* Polygon of a closed shape's vertices in wall order. Returns the point count,
   or -1 when the shape is missing, open, or its walls do not connect. */
int level_shape_polygon(const Level* level, ShapeId id, Vec2* out, int cap);
/* Polygon of a shape's wall loop whether or not its closed flag is set.
   Returns -1 when the walls do not form one connected loop. */
int level_shape_loop_polygon(const Level* level, ShapeId id, Vec2* out,
                             int cap);
/* A new closed shape whose four walls are rect, in loop order. Returns 0 when
   the rectangle is degenerate or the level is full. */
ShapeId level_add_rect_shape(Level* level, Rect rect);
/* The result of adding a rectangle to a shape. A disjoint rectangle is the
   normal way to start a second shape, so it is not an error. */
typedef enum {
    LEVEL_UNION_OK = 0,
    LEVEL_UNION_DISJOINT,
    LEVEL_UNION_INVALID,
} LevelUnion;

/* Rebuilds the shape's outline as the orthogonal union of its own area and
   rect, as one closed loop with no interior walls. The union splits the
   geometry on its own x and y coordinates, so it is exact for any
   axis-aligned shape. LEVEL_UNION_DISJOINT means the rect shares no edge and
   no area with the shape. Doors on walls the union keeps are re-hosted at the
   same world centre; doors on walls that become interior are dropped and
   counted in doors_dropped. */
LevelUnion level_union_rect(Level* level, ShapeId shape, Rect rect,
                            int* doors_dropped);
/* Reshape: slide one wall along its normal by delta, carrying the endpoints of
   every wall that shares them. Clamps delta so no neighbor would collapse or
   flip. Returns true and writes the applied delta when it moved. */
bool level_slide_wall_edge(Level* level, int wall_index, float delta,
                           float min_len);
/* Reshape several edges at once. selected[i] marks walls[i]. The pointer delta
   is projected onto each selected wall's normal, so a horizontal edge takes
   only dy and a vertical edge only dx. A corner shared by two selected edges
   takes both axes, and a neighbour that is not selected is dragged along.
   Returns true when any wall moved. */
bool level_slide_edges(Level* level, const bool* selected, int count, Vec2 delta,
                       float min_len);
/* Rigid translate: moves every selected wall by delta, so their doors follow by
   wall id. Returns true when any wall moved. */
bool level_move_walls(Level* level, const bool* selected, int count, Vec2 delta);
/* Moves one corner. Every wall whose endpoint is `from` keeps that endpoint,
   and each vertex that must follow moves with it so every wall stays
   orthogonal and the outline stays connected. Rejects a move that would
   collapse a wall or leave one diagonal, and changes nothing on rejection. */
bool level_move_vertex(Level* level, Vec2 from, Vec2 to);
/* Index of the closed shape whose outline contains p, or -1. */
int level_shape_at(const Level* level, Vec2 p);
bool level_delete_wall(Level* level, int wall_index);
/* Cuts the section between t0 and t1 out of one wall. The parameters are
   ordered and clamped first. When the span starts after 0 the wall keeps its
   id and shape and ends at the span start; when it ends before 1 a new wall
   joins the same shape for the remainder, with a fresh WallId. Doors whose
   centre lies inside the span are dropped and counted in dropped_doors. A door
   before the span keeps its wall, and a door after it moves to the new wall,
   both with t rescaled so the world centre holds. A shape loses its record
   when the cut removes its last wall. Rejects a span shorter than 1e-3, a wall
   index out of range, or a cut that would leave a wall too short to exist, and
   changes nothing on rejection. */
bool level_cut_wall_span(Level* level, int wall_index, float t0, float t1,
                         int* dropped_doors);
Wall* level_wall_by_id(Level* level, WallId id);
const Wall* level_wall_by_id_const(const Level* level, WallId id);
int level_find_wall_index(const Level* level, WallId id);
bool wall_is_vertical(const Wall* wall);

/* Returns the wall index nearest to p within max_dist, writing the projected
   parameter to out_t. -1 when nothing is close enough. */
int level_nearest_wall(const Level* level, Vec2 p, float max_dist, float* out_t);

int level_add_door(Level* level, WallId wall, float t, float width);
bool level_delete_door(Level* level, int door_index);
bool level_set_door_span(Level* level, int door_index, float t, float width);
Vec2 level_door_center(const Level* level, const Door* door);
void level_door_span(const Level* level, const Door* door, Vec2* out_a, Vec2* out_b);

int level_add_label(Level* level, Vec2 pos, const char* text, float size);
bool level_delete_label(Level* level, int label_index);
bool level_set_label_text(Level* level, int label_index, const char* text);
bool level_set_label_size(Level* level, int label_index, float size);
bool level_move_label(Level* level, int label_index, Vec2 pos);

int level_add_stairs(Level* level, Rect rect, StairsDir dir, int steps);
bool level_delete_stairs(Level* level, int stairs_index);
bool level_set_stairs(Level* level, int stairs_index, StairsDir dir, int steps);
bool level_set_stairs_fade(Level* level, int stairs_index, StairFade fade,
                           float min_alpha, float max_alpha);
/* Replaces the footprint. Rejects a non-finite or smaller-than-STAIR_MIN_SIZE
   box and leaves the stair unchanged. */
bool level_set_stairs_rect(Level* level, int stairs_index, Rect rect);
bool level_move_stairs(Level* level, int stairs_index, Vec2 pos);
bool level_place_player(Level* level, Vec2 pos, float heading);
bool level_remove_player(Level* level);
Rect level_stairs_rect(const Stairs* stairs);
float wall_length(const Wall* wall);

bool level_save(const Level* level, const char* path, LevelError* err);
bool level_load(Level* level, const char* path, LevelError* err);

#define HISTORY_MAX 64

typedef struct {
    Level* snapshots;
    int count;
    int cursor;
    int capacity;
} History;

bool history_init(History* history);
void history_shutdown(History* history);
void history_reset(History* history, const Level* level);
void history_commit(History* history, const Level* level);
/* Copies the committed snapshot at the history cursor into out. A drag reads
   this before it commits, so it can tell a newly created overlap from one that
   was already there when the gesture began. */
bool history_current_copy(const History* history, Level* out);
bool history_undo(History* history, Level* level);
bool history_redo(History* history, Level* level);
bool history_restore_current(History* history, Level* level);
bool history_can_undo(const History* history);
bool history_can_redo(const History* history);

#endif
