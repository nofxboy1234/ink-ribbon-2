#ifndef LEVEL_H
#define LEVEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "geom.h"

#define LEVEL_MAX_WALLS 512
#define LEVEL_MAX_SHAPES 512
#define LEVEL_MAX_ROOMS 64
#define LEVEL_MAX_ROOM_PTS 64
#define LEVEL_MAX_DOORS 256
#define LEVEL_MAX_LABELS 128
#define LEVEL_MAX_STAIRS 64
#define LEVEL_LABEL_LEN 64

#define DOOR_THICKNESS 6.0f
#define DOOR_MIN_WIDTH 8.0f
#define STAIR_STEP_DEPTH 10.0f
#define STAIR_WIDTH 44.0f
#define STAIR_MIN_STEPS 2
#define STAIR_MAX_STEPS 48

typedef uint32_t WallId;
typedef uint32_t ShapeId;

/* A shape is one drawn wall outline: a chain of orthogonal walls that share
   endpoints. A single wall is a shape of one wall. Closing a shape makes it a
   loop whose inside can be filled. */
typedef enum {
    SHAPE_FILL_NONE = 0,
    SHAPE_FILL_BLUE = 1,
    SHAPE_FILL_RED = 2,
} ShapeFill;

typedef struct {
    ShapeId id;
    bool closed;
    ShapeFill fill;
} Shape;

typedef struct {
    WallId id;
    ShapeId shape;
    Vec2 a, b;
} Wall;

typedef enum {
    ROOM_BLUE = 0,
    ROOM_RED = 1,
} RoomColor;

typedef struct {
    Vec2 pts[LEVEL_MAX_ROOM_PTS];
    int count;
    RoomColor color;
} Room;

typedef enum {
    DOOR_WHITE = 0,
    DOOR_RED = 1,
    DOOR_BLUE = 2,
} DoorColor;

typedef struct {
    WallId wall;
    float t;
    float width;
    DoorColor color;
} Door;

typedef struct {
    Vec2 pos;
    float size;
    char text[LEVEL_LABEL_LEN];
} Label;

typedef enum {
    STAIRS_H = 0,
    STAIRS_V = 1,
} StairsDir;

typedef struct {
    Vec2 pos;
    int steps;
    StairsDir dir;
} Stairs;

typedef struct {
    Wall walls[LEVEL_MAX_WALLS];
    int wall_count;
    WallId next_wall_id;

    Shape shapes[LEVEL_MAX_SHAPES];
    int shape_count;
    ShapeId next_shape_id;

    Room rooms[LEVEL_MAX_ROOMS];
    int room_count;

    Door doors[LEVEL_MAX_DOORS];
    int door_count;

    Label labels[LEVEL_MAX_LABELS];
    int label_count;

    Stairs stairs[LEVEL_MAX_STAIRS];
    int stairs_count;
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
bool level_set_shape_fill(Level* level, ShapeId id, ShapeFill fill);
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
/* Index of the closed shape whose outline contains p, or -1. */
int level_shape_at(const Level* level, Vec2 p);
bool level_delete_wall(Level* level, int wall_index);
Wall* level_wall_by_id(Level* level, WallId id);
const Wall* level_wall_by_id_const(const Level* level, WallId id);
int level_find_wall_index(const Level* level, WallId id);
bool wall_is_vertical(const Wall* wall);

/* Returns the wall index nearest to p within max_dist, writing the projected
   parameter to out_t. -1 when nothing is close enough. */
int level_nearest_wall(const Level* level, Vec2 p, float max_dist, float* out_t);

int level_add_room(Level* level, const Vec2* pts, int count, RoomColor color);
bool level_delete_room(Level* level, int room_index);
int level_room_at(const Level* level, Vec2 p);
bool level_set_room_color(Level* level, int room_index, RoomColor color);
bool level_move_room(Level* level, int room_index, Vec2 delta);

int level_add_door(Level* level, WallId wall, float t, float width, DoorColor color);
bool level_delete_door(Level* level, int door_index);
bool level_set_door_span(Level* level, int door_index, float t, float width);
bool level_set_door_color(Level* level, int door_index, DoorColor color);
Vec2 level_door_center(const Level* level, const Door* door);
void level_door_span(const Level* level, const Door* door, Vec2* out_a, Vec2* out_b);

int level_add_label(Level* level, Vec2 pos, const char* text, float size);
bool level_delete_label(Level* level, int label_index);
bool level_set_label_text(Level* level, int label_index, const char* text);
bool level_set_label_size(Level* level, int label_index, float size);
bool level_move_label(Level* level, int label_index, Vec2 pos);

int level_add_stairs(Level* level, Vec2 pos, StairsDir dir, int steps);
bool level_delete_stairs(Level* level, int stairs_index);
bool level_set_stairs(Level* level, int stairs_index, StairsDir dir, int steps);
bool level_move_stairs(Level* level, int stairs_index, Vec2 pos);
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
bool history_undo(History* history, Level* level);
bool history_redo(History* history, Level* level);
bool history_restore_current(History* history, Level* level);
bool history_can_undo(const History* history);
bool history_can_redo(const History* history);

#endif
