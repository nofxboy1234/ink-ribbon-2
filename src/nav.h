#ifndef NAV_H
#define NAV_H

#include "level.h"

/* 8 units per cell fits the 10-unit doors and keeps the whole map under the
   fixed cell budget. */
#define NAV_CELL_SIZE 8.0f
#define NAV_MAX_CELLS 20000
#define NAV_MAX_PATH 4096

typedef struct {
    Vec2 center;
    bool vertical;
    float half;
} NavDoor;

typedef struct {
    float cell;
    float radius;
    Vec2 origin;
    int cols;
    int rows;
    bool built;
    const Level* level;
    NavDoor doors[LEVEL_MAX_DOORS];
    int door_count;
    bool open[NAV_MAX_CELLS];
    /* Bit per direction: E, W, N, S, NE, NW, SE, SW. */
    unsigned char links[NAV_MAX_CELLS];
} NavGrid;

/* Builds walkable cells and edges. Doors are treated as openings; walls are
   solid everywhere else. radius is the player's collision radius. */
bool nav_build(NavGrid* nav, const Level* level, float cell, float radius);

int nav_cell_at(const NavGrid* nav, Vec2 p);
Vec2 nav_cell_center(const NavGrid* nav, int index);
bool nav_cell_valid(const NavGrid* nav, int index);
bool nav_cell_is_open(const NavGrid* nav, int index);

/* A* over the 8-neighbour grid. Writes cell indices from start to goal and
   returns the count, or 0 when there is no path. */
int nav_find_path(const NavGrid* nav, int start, int goal, int* out, int max);

/* True when every sampled point on the segment is on an open nav cell. */
bool nav_line_clear(const NavGrid* nav, Vec2 a, Vec2 b);

/* Turns raw A* cells into centred, line-of-sight-filtered and Chaikin-rounded
   waypoints. Falls back to the filtered polyline if a smoothed segment fails
   the clearance sample. Returns the waypoint count. */
int nav_smooth_path(const NavGrid* nav, const int* cells, int count, Vec2* out,
                    int max);

/* Cell under a world point, or -1 when outside the grid. */
int nav_cell_at_world(const NavGrid* nav, Vec2 p);

#endif
