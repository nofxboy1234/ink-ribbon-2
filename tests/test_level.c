#include "geom.h"
#include "level.h"
#include "nav.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void check(bool cond, const char* name) {
    if (cond) {
        printf("PASS %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failures++;
    }
}

static bool nearf(float a, float b) {
    return fabsf(a - b) <= 1e-4f;
}

static bool nearv(Vec2 a, Vec2 b) {
    return nearf(a.x, b.x) && nearf(a.y, b.y);
}

static bool write_text_file(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static ShapeId add_rectangle(Level* level, Vec2 origin, float w, float h,
                             int walls[4]) {
    ShapeId id = level_add_shape(level);
    walls[0] = level_add_wall_to_shape(level, id, origin,
                                       v2(origin.x + w, origin.y));
    walls[1] = level_add_wall_to_shape(level, id, v2(origin.x + w, origin.y),
                                       v2(origin.x + w, origin.y + h));
    walls[2] = level_add_wall_to_shape(level, id, v2(origin.x + w, origin.y + h),
                                       v2(origin.x, origin.y + h));
    walls[3] = level_add_wall_to_shape(level, id, v2(origin.x, origin.y + h),
                                       origin);
    return id;
}

static void test_geometry(void) {
    check(nearv(snap_ortho(v2(10, 10), v2(23, 14), 10), v2(20, 10)),
          "snap_ortho chooses horizontal and snaps free x");
    check(nearv(snap_ortho(v2(10, 10), v2(13, 24), 10), v2(10, 20)),
          "snap_ortho chooses vertical and snaps free y");
    check(nearv(snap_ortho(v2(10, 10), v2(20, 20), 10), v2(10, 20)),
          "snap_ortho tie chooses vertical");
    check(nearv(snap_grid(v2(-13, -16), 10), v2(-10, -20)),
          "snap_grid handles negative coordinates");
    check(nearv(snap_grid(v2(4, 6), 10), v2(0, 10)),
          "snap_grid rounds to nearest grid line");

    check(nearf(point_segment_dist(v2(5, 5), v2(0, 0), v2(10, 0)), 5),
          "point_segment_dist perpendicular");
    check(nearf(point_segment_dist(v2(15, 0), v2(0, 0), v2(10, 0)), 5),
          "point_segment_dist past endpoint");
    check(nearf(point_segment_t(v2(5, 5), v2(0, 0), v2(10, 0)), 0.5f),
          "point_segment_t projects midpoint");
    check(nearf(point_segment_t(v2(-5, 3), v2(0, 0), v2(10, 0)), 0),
          "point_segment_t clamps below zero");
    check(nearf(point_segment_t(v2(15, 3), v2(0, 0), v2(10, 0)), 1),
          "point_segment_t clamps above one");
    check(nearv(segment_point(v2(0, 0), v2(10, 0), 0.25f), v2(2.5f, 0)),
          "segment_point interpolates along the segment");

    Rect rr = rect_from_points(v2(10, 20), v2(0, 5));
    check(nearv(rr.min, v2(0, 5)) && nearv(rr.max, v2(10, 20)),
          "rect_from_points normalizes corners");
    check(rect_contains(rr, v2(5, 10)) && !rect_contains(rr, v2(11, 10)),
          "rect_contains checks the inclusive box");
    check(rect_overlaps(rr, rect_from_points(v2(9, 9), v2(11, 11))) &&
              !rect_overlaps(rr, rect_from_points(v2(20, 20), v2(30, 30))),
          "rect_overlaps detects overlap and separation");
    Rect marquee = rect_from_points(v2(2, 2), v2(8, 8));
    check(segment_intersects_rect(v2(0, 5), v2(10, 5), marquee),
          "segment_intersects_rect catches a crossing segment");
    check(!segment_intersects_rect(v2(0, 20), v2(10, 20), marquee),
          "segment_intersects_rect rejects a separate segment");
    check(segment_intersects_rect(v2(3, 3), v2(4, 4), marquee),
          "segment_intersects_rect catches a segment inside the box");

    check(nearf(clampf(150, 0, 100), 100) && nearf(lerpf(0, 10, 0.25f), 2.5f),
          "clampf and lerpf");
    check(nearv(v2_add(v2(1, 2), v2(3, 4)), v2(4, 6)) &&
              nearf(v2_dot(v2(1, 2), v2(3, 4)), 11),
          "v2_add and v2_dot");
}

static void test_polygon(void) {
    Vec2 square[4] = { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } };
    check(point_in_polygon(square, 4, v2(5, 5)), "point_in_polygon inside");
    check(!point_in_polygon(square, 4, v2(15, 5)), "point_in_polygon outside");
    check(point_in_polygon(square, 4, v2(0, 5)), "point_in_polygon on edge");
    check(point_in_polygon(square, 4, v2(10, 10)), "point_in_polygon on corner");

    Vec2 poly[7] = {
        { 0, 0 }, { 5, 0 }, { 10, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 }, { 0, 0 },
    };
    int n = polygon_simplify(poly, 7);
    check(n == 4, "polygon_simplify removes duplicate and collinear points");
    check(nearf(polygon_area(poly, n), 100), "polygon_area of simplified square");
    Rect b = polygon_bounds(poly, n);
    check(nearv(b.min, v2(0, 0)) && nearv(b.max, v2(10, 10)),
          "polygon_bounds encloses all points");

    Vec2 reverse[4] = { { 0, 0 }, { 0, 10 }, { 10, 10 }, { 10, 0 } };
    check(nearf(polygon_area(reverse, 4), 100),
          "polygon_area is orientation independent");
    check(polygon_simplify(square, 4) == 4,
          "polygon_simplify leaves a simple polygon unchanged");

    check(polygon_intersects_rect(square, 4, rect_from_points(v2(2, 2), v2(3, 3))),
          "polygon_intersects_rect sees a box inside the polygon");
    check(polygon_intersects_rect(square, 4, rect_from_points(v2(-2, 4), v2(2, 6))),
          "polygon_intersects_rect sees a box crossing an edge");
    check(!polygon_intersects_rect(square, 4, rect_from_points(v2(20, 20), v2(30, 30))),
          "polygon_intersects_rect rejects a separate box");
    Vec2 ell[6] = { { 0, 0 }, { 10, 0 }, { 10, 4 }, { 4, 4 }, { 4, 10 }, { 0, 10 } };
    check(!polygon_intersects_rect(ell, 6, rect_from_points(v2(6, 6), v2(9, 9))),
          "polygon_intersects_rect respects a concave notch");
}

static void test_walls(void) {
    Level level;
    level_init(&level);
    check(level.next_wall_id == 1, "level_init sets next_wall_id to 1");
    check(level_is_empty(&level), "level_init produces an empty document");

    check(level_add_wall(&level, v2(0, 0), v2(100, 0)) == 0,
          "add horizontal wall");
    check(level_add_wall(&level, v2(100, 0), v2(100, 100)) == 1,
          "add vertical wall");
    check(level.walls[0].id == 1 && level.walls[1].id == 2,
          "wall ids are monotonic");
    check(level_add_wall(&level, v2(0, 0), v2(10, 10)) == -1,
          "reject non-orthogonal wall");
    check(level_add_wall(&level, v2(5, 5), v2(5, 5)) == -1,
          "reject zero-length wall");
    check(wall_is_vertical(&level.walls[1]), "wall_is_vertical identifies vertical");
    check(!wall_is_vertical(&level.walls[0]), "wall_is_vertical rejects horizontal");
    check(level_find_wall_index(&level, 0) == -1 &&
              level_wall_by_id(&level, 0) == NULL &&
              level_wall_by_id_const(&level, 99) == NULL,
          "wall lookup rejects id 0 and unknown ids");

    level_add_wall(&level, v2(0, 100), v2(200, 100));
    float t = -1;
    int idx = level_nearest_wall(&level, v2(50, 5), 10, &t);
    check(idx == 0 && nearf(t, 0.5f), "level_nearest_wall picks wall and t");
    idx = level_nearest_wall(&level, v2(-1, 0.5f), 5, &t);
    check(idx == 0 && nearf(t, 0), "level_nearest_wall clamps t at the endpoint");
    idx = level_nearest_wall(&level, v2(98, 50), 5, &t);
    check(idx == 1 && nearf(t, 0.5f), "level_nearest_wall picks the vertical wall");
    idx = level_nearest_wall(&level, v2(50, 50), 1, &t);
    check(idx == -1, "level_nearest_wall respects max_dist");

    Level empty;
    level_init(&empty);
    check(level_nearest_wall(&empty, v2(0, 0), 100, &t) == -1,
          "level_nearest_wall returns -1 on an empty level");
}

static void test_doors(void) {
    Level level;
    level_init(&level);
    level_add_wall(&level, v2(0, 0), v2(100, 0));
    WallId host = level.walls[0].id;

    check(level_add_door(&level, 999, 0.5f, 20) == -1,
          "reject door on an unknown wall");
    check(level_add_door(&level, host, 0.5f, 20) == 0,
          "add door to a wall");
    Vec2 a, b;
    level_door_span(&level, &level.doors[0], &a, &b);
    check(nearv(a, v2(40, 0)) && nearv(b, v2(60, 0)),
          "door span writes the bar endpoints");
    check(nearv(level_door_center(&level, &level.doors[0]), v2(50, 0)),
          "door center is the span midpoint");

    check(level_set_door_span(&level, 0, 0.95f, 20),
          "set_door_span accepts a valid change");
    level_door_span(&level, &level.doors[0], &a, &b);
    check(nearv(a, v2(80, 0)) && nearv(b, v2(100, 0)),
          "set_door_span slides the door and clamps t");
    check(!level_set_door_span(&level, -1, 0.5f, 20) &&
              !level_set_door_span(&level, 9, 0.5f, 20),
          "set_door_span rejects a bad index");

    level_set_door_span(&level, 0, 0.5f, 500);
    check(nearf(level.doors[0].width, 100),
          "set_door_span clamps width to wall length");
    level_set_door_span(&level, 0, 0.5f, 1);
    check(nearf(level.doors[0].width, DOOR_MIN_WIDTH),
          "set_door_span enforces minimum width");

    check(level_add_door(&level, host, 0, 20) == 1,
          "add_door clamps t at the wall start");
    level_door_span(&level, &level.doors[1], &a, &b);
    check(nearv(a, v2(0, 0)) && nearv(b, v2(20, 0)),
          "add_door keeps the whole span on the wall");

    check(level_delete_door(&level, 0) && level.door_count == 1,
          "delete door");
    check(!level_delete_door(&level, 5), "delete door rejects a bad index");

    int other = level_add_wall(&level, v2(0, 100), v2(100, 100));
    check(level_add_door(&level, level.walls[other].id, 0.5f, 20) == 1,
          "add a door hosted by another wall");
    check(level_delete_wall(&level, 0) && level.wall_count == 1 &&
              level.door_count == 1 && level.doors[0].wall == 2,
          "delete wall cascades only its hosted doors");
    check(level_delete_wall(&level, 0) && level.wall_count == 0 &&
              level.door_count == 0,
          "delete the last wall leaves no orphan doors");
}

static void test_delete_edge_reopens_shape(void) {
    Level level;
    level_init(&level);
    ShapeId shape = level_add_shape(&level);
    level_add_wall_to_shape(&level, shape, v2(0, 0), v2(100, 0));
    level_add_wall_to_shape(&level, shape, v2(100, 0), v2(100, 100));
    level_add_wall_to_shape(&level, shape, v2(100, 100), v2(0, 100));
    level_add_wall_to_shape(&level, shape, v2(0, 100), v2(0, 0));
    check(level_close_shape(&level, shape), "close a four wall shape");
    const Shape* record = level_shape_by_id_const(&level, shape);
    check(record && record->closed, "closed shape reports closed");

    check(level_delete_wall(&level, 3), "delete one edge of a closed shape");
    record = level_shape_by_id_const(&level, shape);
    check(record && !record->closed, "deleting an edge reopens the shape");
    check(level_shape_polygon(&level, shape, NULL, 0) < 0 ||
              level_shape_polygon(&level, shape, NULL, 0) == -1,
          "an open shape has no polygon");
    check(level_shape_at(&level, v2(50, 50)) < 0,
          "an open shape is not a click target");

    level_delete_wall(&level, 2);
    level_delete_wall(&level, 1);
    level_delete_wall(&level, 0);
    check(level.shape_count == 0 && level.wall_count == 0,
          "deleting every edge removes the shape");
}

static void test_labels(void) {
    Level level;
    level_init(&level);

    char long_text[100];
    memset(long_text, 'x', sizeof long_text);
    long_text[sizeof long_text - 1] = '\0';
    check(level_add_label(&level, v2(10, 20), long_text, 12) == 0,
          "add label");
    check(strlen(level.labels[0].text) == LEVEL_LABEL_LEN - 1,
          "label text truncated to LEVEL_LABEL_LEN-1");
    check(nearv(level.labels[0].pos, v2(10, 20)) && nearf(level.labels[0].size, 12),
          "label keeps position and size");

    check(level_add_label(&level, v2(0, 0), "attic stairs", 4) == 1,
          "add label with spaces in the text");
    check(nearf(level.labels[1].size, 6), "label size clamped to at least 6");
    check(strcmp(level.labels[1].text, "attic stairs") == 0,
          "label text keeps spaces");
    check(level_set_label_text(&level, 1, "new name") &&
              strcmp(level.labels[1].text, "new name") == 0,
          "set label text");
    check(level_set_label_size(&level, 1, 9) &&
              nearf(level.labels[1].size, 9),
          "set label size");
    check(level_move_label(&level, 1, v2(5, 6)) &&
              nearv(level.labels[1].pos, v2(5, 6)),
          "move label");
    check(level_delete_label(&level, 1) && level.label_count == 1,
          "delete label");
    check(!level_delete_label(&level, 4), "delete label rejects a bad index");
}

static void test_stairs(void) {
    Level level;
    level_init(&level);

    Rect box = { { 100, 100 }, { 160, 144 } };
    check(level_add_stairs(&level, box, STAIRS_H, 1) == 0 &&
              level.stairs[0].steps == STAIR_MIN_STEPS,
          "stairs step count clamped low");
    Rect h = level_stairs_rect(&level.stairs[0]);
    check(nearv(h.min, v2(100, 100)) && nearv(h.max, v2(160, 144)) &&
              nearf(level.stairs[0].width, 60) &&
              nearf(level.stairs[0].length, 44),
          "add stairs keeps the rect as the explicit footprint");

    check(level_add_stairs(&level, (Rect){ { 100, 100 }, { 144, 160 } },
                           STAIRS_V, 100) == 1 &&
              level.stairs[1].steps == STAIR_MAX_STEPS,
          "stairs step count clamped high");
    Rect v = level_stairs_rect(&level.stairs[1]);
    check(nearv(v.min, v2(100, 100)) && nearv(v.max, v2(144, 160)),
          "stairs V footprint is the stored box, not derived from steps");
    check(level.stairs[0].fade == STAIR_FADE_LINEAR &&
              nearf(level.stairs[0].min_alpha, STAIR_ALPHA_MIN) &&
              nearf(level.stairs[0].max_alpha, STAIR_ALPHA_MAX),
          "a new stair uses the linear default fade");
    check(level_set_stairs_fade(&level, 0, STAIR_FADE_SMOOTH, 0.9f, 0.1f) &&
              level.stairs[0].fade == STAIR_FADE_SMOOTH &&
              nearf(level.stairs[0].min_alpha, 0.1f) &&
              nearf(level.stairs[0].max_alpha, 0.9f),
          "set stairs fade keeps min below max");
    check(!level_set_stairs_fade(&level, 0, (StairFade)99, 0.1f, 0.9f) &&
              !level_set_stairs_fade(&level, 0, STAIR_FADE_LINEAR, NAN, 0.9f) &&
              !level_set_stairs_fade(&level, 9, STAIR_FADE_LINEAR, 0.1f, 0.9f),
          "set stairs fade rejects a bad enum, a non-finite opacity, or a bad index");

    check(level_set_stairs(&level, 0, STAIRS_V, 10), "set stairs direction");
    check(nearv(level.stairs[0].pos, v2(100, 100)) &&
              nearf(level.stairs[0].width, 60) &&
              nearf(level.stairs[0].length, 44) &&
              level.stairs[0].steps == 10,
          "set direction and steps keeps the footprint");

    check(level_set_stairs_rect(&level, 0, (Rect){ { 92, 100 }, { 160, 144 } }),
          "set stairs rect moves one edge");
    Rect dragged = level_stairs_rect(&level.stairs[0]);
    check(nearv(dragged.min, v2(92, 100)) && nearv(dragged.max, v2(160, 144)),
          "resized box keeps the other three edges");

    Level snapshot = level;
    check(level_add_stairs(&level, (Rect){ { 0, 0 }, { 7, 50 } },
                           STAIRS_H, 4) == -1,
          "reject stairs whose footprint is too small");
    check(memcmp(&level, &snapshot, sizeof level) == 0,
          "rejected add leaves the level byte-identical");
    check(level_add_stairs(&level, (Rect){ { 0, 0 }, { INFINITY, 50 } },
                           STAIRS_H, 4) == -1,
          "reject stairs with a non-finite corner");
    check(memcmp(&level, &snapshot, sizeof level) == 0,
          "rejected non-finite add leaves the level byte-identical");

    snapshot = level;
    check(!level_set_stairs_rect(&level, 0,
                                 (Rect){ { 100, 100 }, { 107, 144 } }),
          "set stairs rect rejects a too-small box");
    check(memcmp(&level, &snapshot, sizeof level) == 0,
          "rejected resize leaves the stair byte-identical");

    check(level_move_stairs(&level, 0, v2(200, 300)) &&
              nearv(level.stairs[0].pos, v2(200, 300)) &&
              nearf(level.stairs[0].width, 68) &&
              nearf(level.stairs[0].length, 44),
          "move stairs shifts the min corner and keeps the size");
    check(level_delete_stairs(&level, 0) && level.stairs_count == 1,
          "delete stairs");
    check(!level_delete_stairs(&level, 4), "delete stairs rejects a bad index");
    check(level_add_stairs(&level, (Rect){ { 0, 0 }, { 10, 10 } },
                           (StairsDir)5, 4) == -1,
          "reject stairs with unknown direction");
}

static bool wall_ok_for_test(const Wall* wall) {
    float dx = fabsf(wall->b.x - wall->a.x);
    float dy = fabsf(wall->b.y - wall->a.y);
    return (dx <= 0.001f) != (dy <= 0.001f);
}

static void test_rect_union(void) {
    Level level;
    level_init(&level);

    check(level_add_rect_shape(&level, (Rect){ { 0, 0 }, { 0, 10 } }) == 0,
          "rect shape rejects a zero-width rectangle");
    check(level_add_rect_shape(&level, (Rect){ { 0, 0 }, { INFINITY, 10 } }) == 0,
          "rect shape rejects a non-finite rectangle");

    Rect first = { { 0, 0 }, { 100, 100 } };
    ShapeId shape = level_add_rect_shape(&level, first);
    check(shape == 1 && level.wall_count == 4 && level.shape_count == 1 &&
              level.shapes[0].closed,
          "rect shape is one closed four-wall shape");
    Vec2 polygon[LEVEL_MAX_WALLS];
    check(level_shape_polygon(&level, shape, polygon, LEVEL_MAX_WALLS) == 4,
          "rect shape has a four point polygon");

    int dropped = -1;
    check(level_union_rect(&level, shape, first, &dropped) == LEVEL_UNION_OK,
          "union with its own rectangle keeps the shape");
    check(level.wall_count == 4 && dropped == 0,
          "a redundant union leaves the outline alone");

    Level disjoint_before = level;
    check(level_union_rect(&level, shape, (Rect){ { 200, 200 }, { 300, 300 } },
                           &dropped) == LEVEL_UNION_DISJOINT,
          "a separate rectangle is disjoint");
    check(memcmp(&level, &disjoint_before, sizeof level) == 0,
          "a disjoint union leaves the level byte-identical");

    check(level_union_rect(&level, shape, (Rect){ { 100, 100 }, { 200, 200 } },
                           &dropped) == LEVEL_UNION_DISJOINT,
          "a corner-touching rectangle is disjoint");

    Rect overlap = { { 60, 60 }, { 160, 160 } };
    check(level_union_rect(&level, shape, overlap, &dropped) == LEVEL_UNION_OK,
          "overlapping rectangles union");
    check(level.shape_count == 1 && level.wall_count == 8,
          "the union is one shape with a merged eight-wall outline");
    int count = level_shape_polygon(&level, shape, polygon, LEVEL_MAX_WALLS);
    check(count == 8 &&
              nearf(polygon_area(polygon, count), 100.0f * 100.0f + 100.0f * 100.0f -
                                                      40.0f * 40.0f),
          "the union outline covers both rectangles once");

    Level edge;
    level_init(&edge);
    ShapeId edge_shape = level_add_rect_shape(&edge, (Rect){ { 0, 0 }, { 100, 100 } });
    check(level_union_rect(&edge, edge_shape, (Rect){ { 100, 0 }, { 200, 50 } },
                           NULL) == LEVEL_UNION_OK,
          "a rectangle sharing an edge unions");
    check(edge.wall_count == 6 &&
              level_shape_polygon(&edge, edge_shape, polygon, LEVEL_MAX_WALLS) == 6 &&
              nearf(polygon_area(polygon, 6), 100.0f * 100.0f + 100.0f * 50.0f),
          "the shared edge disappears and the union is one six-wall outline");

    Level plus;
    level_init(&plus);
    ShapeId bar = level_add_rect_shape(&plus, (Rect){ { 40, 0 }, { 60, 100 } });
    check(level_union_rect(&plus, bar, (Rect){ { 0, 40 }, { 100, 60 } }, NULL) ==
              LEVEL_UNION_OK,
          "the vertical bar takes the horizontal bar");
    check(level_union_rect(&plus, bar, (Rect){ { 40, 40 }, { 60, 60 } }, NULL) ==
              LEVEL_UNION_OK,
          "a third overlapping rectangle joins the plus");
    check(plus.wall_count == 12 &&
              level_shape_polygon(&plus, bar, polygon, LEVEL_MAX_WALLS) == 12 &&
              nearf(polygon_area(polygon, 12), 3600.0f),
          "a plus built from three rectangles costs only its twelve-wall outline");
    bool orthogonal = true;
    for (int i = 0; i < plus.wall_count; i++) {
        if (!wall_ok_for_test(&plus.walls[i])) orthogonal = false;
    }
    check(orthogonal, "every union wall stays orthogonal");

    /* Doors keep their world centre when their wall survives, and drop when
       the wall becomes interior. */
    Level doors;
    level_init(&doors);
    ShapeId door_shape = level_add_rect_shape(&doors, (Rect){ { 0, 0 }, { 100, 100 } });
    int top = -1;
    for (int i = 0; i < doors.wall_count; i++) {
        if (fabsf(doors.walls[i].a.y) <= 0.001f &&
            fabsf(doors.walls[i].b.y) <= 0.001f) {
            top = i;
        }
    }
    check(top >= 0, "find the top wall for the door test");
    check(level_add_door(&doors, doors.walls[top].id, 0.3f, 20) == 0,
          "add a door to the top wall");
    check(level_union_rect(&doors, door_shape, (Rect){ { 0, 100 }, { 50, 200 } },
                           &dropped) == LEVEL_UNION_OK &&
              dropped == 0 && doors.door_count == 1,
          "a union that keeps the host wall keeps its door");
    check(nearv(level_door_center(&doors, &doors.doors[0]), v2(30, 0)),
          "the re-hosted door keeps its world centre");

    Level gone;
    level_init(&gone);
    ShapeId gone_shape = level_add_rect_shape(&gone, (Rect){ { 0, 0 }, { 100, 100 } });
    int right = -1;
    for (int i = 0; i < gone.wall_count; i++) {
        if (wall_is_vertical(&gone.walls[i]) &&
            fabsf(gone.walls[i].a.x - 100.0f) <= 0.001f) {
            right = i;
        }
    }
    check(right >= 0, "find the right wall for the drop test");
    check(level_add_door(&gone, gone.walls[right].id, 0.5f, 20) == 0,
          "add a door to the right wall");
    check(level_union_rect(&gone, gone_shape, (Rect){ { 100, 0 }, { 200, 100 } },
                           &dropped) == LEVEL_UNION_OK &&
              dropped == 1 && gone.door_count == 0,
          "a door whose wall becomes interior is dropped and counted");

    Level free;
    level_init(&free);
    ShapeId free_shape = level_add_shape(&free);
    level_add_wall_to_shape(&free, free_shape, v2(0, 0), v2(10, 0));
    level_add_wall_to_shape(&free, free_shape, v2(10, 0), v2(10, 10));
    level_add_wall_to_shape(&free, free_shape, v2(10, 10), v2(0, 10));
    level_add_wall_to_shape(&free, free_shape, v2(0, 10), v2(0, 0));
    check(level_close_shape(&free, free_shape), "close an off-grid shape");
    check(level_union_rect(&free, free_shape, (Rect){ { 5, 5 }, { 15, 15 } },
                           NULL) == LEVEL_UNION_OK &&
              free.wall_count == 8 &&
              level_shape_polygon(&free, free_shape, polygon, LEVEL_MAX_WALLS) == 8 &&
              nearf(polygon_area(polygon, 8), 175.0f),
          "the union works on coordinates that are not on the editor grid");
}

static void test_rect_union_save_load(void) {
    Level level;
    level_init(&level);
    ShapeId shape = level_add_rect_shape(&level, (Rect){ { 0, 0 }, { 100, 100 } });
    check(level_union_rect(&level, shape, (Rect){ { 60, 60 }, { 160, 160 } },
                           NULL) == LEVEL_UNION_OK,
          "prepare a union for the roundtrip");

    char path[256];
    snprintf(path, sizeof path, "/tmp/ink_ribbon_union_%ld.lvl", (long)getpid());
    LevelError err;
    check(level_save(&level, path, &err), "save a unioned level");

    Level loaded;
    level_init(&loaded);
    check(level_load(&loaded, path, &err), "load a unioned level");
    check(loaded.shape_count == 1 && loaded.wall_count == 8 &&
              loaded.shapes[0].closed,
          "the union survives save and load as one closed shape");
    Vec2 polygon[LEVEL_MAX_WALLS];
    check(level_shape_polygon(&loaded, loaded.shapes[0].id, polygon,
                              LEVEL_MAX_WALLS) == 8 &&
              nearf(polygon_area(polygon, 8), 18400.0f),
          "the loaded union keeps its outline and area");
    check(level_union_rect(&loaded, loaded.shapes[0].id,
                           (Rect){ { -40, -40 }, { 20, 20 } }, NULL) ==
              LEVEL_UNION_OK &&
              loaded.wall_count == 12,
          "a loaded union accepts the next rectangle");
    remove(path);
}

static void test_stairs_flip(void) {
    Level level;
    level_init(&level);
    check(level_add_stairs(&level, (Rect){ { 0, 0 }, { 100, 50 } }, STAIRS_H, 6) ==
              0,
          "add a stair for the flip roundtrip");
    check(!level.stairs[0].bottom, "a new stair starts with the fade at the min end");
    level.stairs[0].bottom = true;
    check(level_set_stairs_fade(&level, 0, STAIR_FADE_EASE_OUT, 0.25f, 0.9f),
          "set a custom stair fade before saving");

    char path[256];
    snprintf(path, sizeof path, "/tmp/ink_ribbon_flip_%ld.lvl", (long)getpid());
    LevelError err;
    check(level_save(&level, path, &err), "save a flipped stair");

    char file[2048] = { 0 };
    FILE* f = fopen(path, "rb");
    if (f) {
        size_t got = fread(file, 1, sizeof file - 1, f);
        file[got] = '\0';
        fclose(f);
    }
    check(strstr(file, "stairs 0.000 0.000 0 6 100.000 50.000 1 2 0.250 0.900\n") != NULL,
          "the saved stair carries the flip flag, falloff, and opacities");

    Level loaded;
    level_init(&loaded);
    check(level_load(&loaded, path, &err) && loaded.stairs_count == 1,
          "load the flipped stair");
    check(loaded.stairs[0].bottom && loaded.stairs[0].steps == 6 &&
              nearf(loaded.stairs[0].width, 100) &&
              nearf(loaded.stairs[0].length, 50) &&
              loaded.stairs[0].fade == STAIR_FADE_EASE_OUT &&
              nearf(loaded.stairs[0].min_alpha, 0.25f) &&
              nearf(loaded.stairs[0].max_alpha, 0.9f),
          "the flip flag, footprint, falloff, and opacities survive save and load");
    level.stairs[0].bottom = false;
    check(level_save(&level, path, &err), "save an unflipped stair");
    level_init(&loaded);
    check(level_load(&loaded, path, &err) && !loaded.stairs[0].bottom &&
              loaded.stairs[0].fade == STAIR_FADE_EASE_OUT,
          "the unflipped flag and falloff survive save and load");
    remove(path);
}

static void test_save_load(void) {
    Level src;
    level_init(&src);
    level_add_wall(&src, v2(0, 0), v2(100, 0));
    level_add_wall(&src, v2(100, 0), v2(100, 100));
    level_add_wall(&src, v2(100, 100), v2(0, 100));
    level_delete_wall(&src, 0);

    level_add_door(&src, src.walls[0].id, 0.5f, 20);
    level_add_label(&src, v2(10, 20), "attic stairs", 12);
    level_add_stairs(&src, (Rect){ { 50, 50 }, { 110, 94 } }, STAIRS_H, 6);
    check(level_set_stairs_rect(&src, 0, (Rect){ { 42, 50 }, { 114, 94 } }),
          "prepare a resized stair for the roundtrip");

    check(level_add_label(&src, v2(0, 0), "bad\nname", 8) == 1,
          "add label with a control character");
    char path[256];
    snprintf(path, sizeof path, "/tmp/ink_ribbon_core_%ld.lvl", (long)getpid());

    LevelError err;
    check(level_save(&src, path, &err), "level_save writes the document");
    check(err.message[0] == '\0', "level_save clears the error on success");

    FILE* f = fopen(path, "rb");
    check(f != NULL, "saved level file is readable");
    char first[64] = { 0 };
    char file[8192] = { 0 };
    if (f) {
        if (fgets(first, sizeof first, f)) {
            check(strcmp(first, "ink-level 3\n") == 0,
                  "saved file starts with ink-level 3");
        } else {
            check(false, "saved file starts with ink-level 3");
        }
        rewind(f);
        size_t got = fread(file, 1, sizeof file - 1, f);
        file[got] = '\0';
        fclose(f);
    }
    check(strstr(file, "shape 2 0 0\n") != NULL &&
              strstr(file, "shape 3 0 0\n") != NULL,
          "save writes one open shape per wall");
    check(strstr(file, "wall 2 2 100.000 0.000 100.000 100.000\n") != NULL,
          "save writes wall records with the shape id and preserved ids");
    check(strstr(file, "door 0 2 0.500 20.000\n") != NULL,
          "save writes the white door host and span");
    check(strstr(file, "label 10.000 20.000 12.000 attic stairs\n") != NULL,
          "save writes label text as the rest of the line");
    check(strstr(file, "label 0.000 0.000 8.000 bad name\n") != NULL,
          "save replaces control characters in label text with spaces");
    check(strstr(file, "stairs 42.000 50.000 0 6 72.000 44.000 0 0 0.180 0.680\n") != NULL,
          "save writes the explicit resized stair footprint");

    Level dst;
    level_init(&dst);
    check(level_load(&dst, path, &err), "level_load reads the saved level");
    check(err.message[0] == '\0', "level_load clears the error on success");
    check(dst.wall_count == 2 && dst.walls[0].id == 2 && dst.walls[1].id == 3,
          "roundtrip preserves wall ids");
    check(dst.stairs_count == 1 && dst.stairs[0].bottom == 0,
          "roundtrip preserves the stair fade end");
    check(dst.next_wall_id == 4, "load sets next_wall_id to max wall id + 1");
    check(dst.shape_count == 2 && dst.shapes[0].id == 2 && dst.shapes[1].id == 3 &&
              !dst.shapes[0].closed && !dst.shapes[1].closed,
          "roundtrip preserves open shapes");
    check(dst.walls[0].shape == 2 && dst.walls[1].shape == 3,
          "roundtrip preserves each wall's shape id");
    check(dst.next_shape_id == 4,
          "load sets next_shape_id to max shape id + 1");
    check(dst.door_count == 1 && dst.doors[0].wall == 2 &&
              nearf(dst.doors[0].t, 0.5f) && nearf(dst.doors[0].width, 20),
          "roundtrip preserves the door host and span");
    check(nearv(level_door_center(&dst, &dst.doors[0]), v2(100, 50)),
          "loaded door keeps its world position");
    check(dst.label_count == 2 &&
              strcmp(dst.labels[0].text, "attic stairs") == 0 &&
              strcmp(dst.labels[1].text, "bad name") == 0,
          "roundtrip preserves label text and spacing");
    check(dst.stairs_count == 1 && dst.stairs[0].dir == STAIRS_H &&
              dst.stairs[0].steps == 6 &&
              nearv(dst.stairs[0].pos, v2(42, 50)) &&
              nearf(dst.stairs[0].width, 72) &&
              nearf(dst.stairs[0].length, 44),
          "roundtrip preserves the resized stair box and steps");
    Rect src_bounds = level_bounds(&src);
    Rect dst_bounds = level_bounds(&dst);
    check(nearv(src_bounds.min, dst_bounds.min) &&
              nearv(src_bounds.max, dst_bounds.max),
          "roundtrip preserves document bounds");

    int nw = level_add_wall(&dst, v2(0, 0), v2(0, 50));
    check(nw == 2 && dst.walls[2].id == 4,
          "loaded level continues the wall id sequence");
    check(dst.shape_count == 3 && dst.shapes[2].id == 4 &&
              dst.walls[2].shape == 4 && dst.next_shape_id == 5,
          "a wall added after load gets its own new shape");

    char comments[256];
    snprintf(comments, sizeof comments, "/tmp/ink_ribbon_comments_%ld.lvl",
             (long)getpid());
    FILE* cf = fopen(comments, "wb");
    check(cf != NULL, "write comment fixture");
    if (cf) {
        fputs("# leading comment\n\nink-level 1\n\n# wall below\n"
              "wall 7 0.000 0.000 5.000 0.000\n",
              cf);
        fclose(cf);
    }
    Level cl;
    level_init(&cl);
    check(level_load(&cl, comments, &err),
          "level_load ignores blank lines and comments");
    check(cl.wall_count == 1 && cl.walls[0].id == 7 && cl.next_wall_id == 8,
          "commented load preserves the wall id");
    check(cl.shape_count == 1 && cl.shapes[0].id == 1 && !cl.shapes[0].closed &&
              cl.walls[0].shape == 1 && cl.next_shape_id == 2,
          "version 1 load gives each wall its own open shape");

    char bad_path[256];
    snprintf(bad_path, sizeof bad_path, "/tmp/ink_ribbon_bad_%ld.lvl",
             (long)getpid());
    FILE* bf = fopen(bad_path, "wb");
    check(bf != NULL, "write malformed fixture");
    if (bf) {
        fputs("ink-level 1\nwall 1 0.000 0.000 10.000 10.000\n", bf);
        fclose(bf);
    }
    Level bad;
    level_init(&bad);
    level_add_wall(&bad, v2(0, 0), v2(5, 0));
    bool bad_ok = level_load(&bad, bad_path, &err);
    check(!bad_ok, "level_load rejects a non-orthogonal wall");
    check(strncmp(err.message, "line 2:", 7) == 0,
          "load error names the failing line");
    check(bad.wall_count == 1 && bad.walls[0].id == 1,
          "failed load leaves the target level untouched");

    FILE* df = fopen(bad_path, "wb");
    if (df) {
        fputs("ink-level 1\nwall 1 0.000 0.000 10.000 0.000\n"
              "wall 1 0.000 10.000 10.000 10.000\n",
              df);
        fclose(df);
    }
    check(!level_load(&bad, bad_path, &err),
          "level_load rejects duplicate wall ids");
    check(strncmp(err.message, "line 3:", 7) == 0,
          "duplicate wall error names the second line");

    FILE* uf = fopen(bad_path, "wb");
    if (uf) {
        fputs("ink-level 1\ndoor 0 9 0.500 10.000\n", uf);
        fclose(uf);
    }
    check(!level_load(&bad, bad_path, &err),
          "level_load rejects a door on an unknown wall");

    FILE* pf = fopen(bad_path, "wb");
    if (pf) {
        fputs("ink-level 1\ndoor 2 5 0.500 10.000\n"
              "wall 5 0.000 0.000 50.000 0.000\n",
              pf);
        fclose(pf);
    }
    Level reordered;
    level_init(&reordered);
    check(level_load(&reordered, bad_path, &err),
          "level_load accepts a door before its host wall");
    check(reordered.door_count == 1 && reordered.doors[0].wall == 5 &&
              nearv(level_door_center(&reordered, &reordered.doors[0]),
                    v2(25, 0)),
          "door resolves its host after the whole file is read");

    char missing_path[256];
    snprintf(missing_path, sizeof missing_path, "/tmp/ink_ribbon_missing_%ld.lvl",
             (long)getpid());
    remove(missing_path);
    check(!level_load(&bad, missing_path, &err),
          "level_load reports a missing file");
    check(strncmp(err.message, "line 1:", 7) == 0,
          "missing-file load error carries the line prefix");

    const char* missing = "/tmp/ink-ribbon-no-such-dir/level.lvl";
    check(!level_save(&src, missing, &err), "level_save reports an I/O failure");
    check(err.message[0] != '\0', "failed save fills the error message");
    Level intact;
    level_init(&intact);
    check(level_load(&intact, path, &err) && intact.wall_count == 2,
          "failed save leaves the existing level intact");

    remove(path);
    remove(comments);
    remove(bad_path);
}

static void test_stair_migration(void) {
    char path[256];
    snprintf(path, sizeof path, "/tmp/ink_ribbon_stair_v2_%ld.lvl",
             (long)getpid());

    LevelError err;
    Level level;
    level_init(&level);
    check(write_text_file(path,
                          "ink-level 2\n"
                          "stairs 100.000 100.000 0 6\n"),
          "write a version 2 fixture with a stair");
    check(level_load(&level, path, &err),
          "a version 2 file with a stair still loads");
    check(level.stairs_count == 1 && level.stairs[0].dir == STAIRS_H &&
              level.stairs[0].steps == 6 &&
              nearf(level.stairs[0].width, 60) &&
              nearf(level.stairs[0].length, 44),
          "version 2 stair derives 44 across and steps*10 along");
    check(nearv(level.stairs[0].pos, v2(70, 78)),
          "version 2 centre converts to the min corner");

    check(write_text_file(path,
                          "ink-level 1\n"
                          "stairs 100.000 100.000 1 9\n"),
          "write a version 1 fixture with a stair");
    level_init(&level);
    check(level_load(&level, path, &err) && level.stairs_count == 1,
          "a version 1 file with a stair still loads");
    check(level.stairs[0].dir == STAIRS_V &&
              nearf(level.stairs[0].width, 44) &&
              nearf(level.stairs[0].length, 90),
          "version 1 vertical stair derives the transposed box");
    check(nearv(level.stairs[0].pos, v2(78, 55)),
          "version 1 centre converts to the min corner");

    Level intact;
    level_init(&intact);
    level_add_stairs(&intact, (Rect){ { 0, 0 }, { 20, 20 } }, STAIRS_H, 4);
    Level snapshot = intact;
    check(write_text_file(path,
                          "ink-level 3\n"
                          "stairs 0.000 0.000 0 4 4.000 20.000\n"),
          "write a version 3 stair below the minimum size");
    check(!level_load(&intact, path, &err),
          "load rejects a version 3 stair footprint that is too small");
    check(memcmp(&intact, &snapshot, sizeof intact) == 0,
          "a rejected stair load leaves the target level untouched");
    remove(path);
}

static void test_bounds(void) {
    Level level;
    level_init(&level);
    Rect empty = level_bounds(&level);
    check(nearv(empty.min, v2(0, 0)) && nearv(empty.max, v2(0, 0)),
          "level_bounds on an empty level is a zero rect");

    level_add_wall(&level, v2(10, 20), v2(10, 60));
    Rect walls = level_bounds(&level);
    check(nearv(walls.min, v2(10, 20)) && nearv(walls.max, v2(10, 60)),
          "level_bounds covers wall endpoints");

    level_add_label(&level, v2(-5, -5), "hi", 10);
    level_add_stairs(&level, (Rect){ { 290, 278 }, { 310, 322 } }, STAIRS_H, 2);
    Rect all = level_bounds(&level);
    check(nearv(all.min, v2(-5, -5)), "level_bounds includes label text box");
    check(nearf(all.max.x, 310) && nearf(all.max.y, 322),
          "level_bounds includes stair extents");
}

static void test_history(void) {
    History h;
    check(history_init(&h), "history_init allocates snapshots");

    Level level;
    level_init(&level);
    level_add_wall(&level, v2(0, 0), v2(10, 0));
    history_reset(&h, &level);
    check(h.count == 1 && h.cursor == 0, "history_reset stores the first snapshot");
    check(!history_can_undo(&h) && !history_can_redo(&h),
          "fresh history has no undo or redo");

    level_add_wall(&level, v2(0, 0), v2(0, 10));
    history_commit(&h, &level);
    level_add_wall(&level, v2(10, 0), v2(10, 10));
    history_commit(&h, &level);
    check(h.count == 3 && h.cursor == 2, "history_commit appends snapshots");
    check(history_can_undo(&h) && !history_can_redo(&h),
          "history flags after commit");

    Level cur;
    level_init(&cur);
    check(history_undo(&h, &cur) && cur.wall_count == 2,
          "history_undo restores the previous state");
    check(history_can_redo(&h), "history_can_redo after undo");
    check(history_undo(&h, &cur) && cur.wall_count == 1,
          "history_undo reaches the first snapshot");
    check(!history_undo(&h, &cur), "history_undo stops at the oldest snapshot");
    check(history_redo(&h, &cur) && cur.wall_count == 2,
          "history_redo restores the next state");

    level_add_wall(&level, v2(20, 20), v2(30, 20));
    history_commit(&h, &level);
    check(h.count == 3 && h.cursor == 2 && !history_can_redo(&h),
          "history_commit drops the redo tail");
    check(history_undo(&h, &cur) && cur.wall_count == 2,
          "undo after a branching commit skips the dropped redo state");
    history_shutdown(&h);

    History restore;
    check(history_init(&restore), "history_init for the restore test");
    Level live;
    level_init(&live);
    level_add_wall(&live, v2(0, 0), v2(4, 0));
    history_reset(&restore, &live);
    level_add_wall(&live, v2(0, 0), v2(0, 4));
    history_commit(&restore, &live);
    level_add_wall(&live, v2(8, 0), v2(8, 4));
    check(live.wall_count == 3, "uncommitted edit is live in the document");
    check(history_restore_current(&restore, &live) && live.wall_count == 2,
          "history_restore_current discards an uncommitted edit");
    check(!history_restore_current(NULL, &live),
          "history_restore_current rejects a null history");
    history_shutdown(&restore);

    History ring;
    check(history_init(&ring), "history_init for the ring test");
    Level r;
    level_init(&r);
    history_reset(&ring, &r);
    for (int i = 0; i < HISTORY_MAX; i++) {
        level_add_wall(&r, v2((float)i, 0), v2((float)i + 1, 0));
        history_commit(&ring, &r);
    }
    check(ring.count == HISTORY_MAX, "history ring stays at HISTORY_MAX");
    Level out;
    level_init(&out);
    int undos = 0;
    while (history_undo(&ring, &out)) undos++;
    check(undos == HISTORY_MAX - 1, "history ring drops the oldest snapshot");
    check(out.wall_count == 1, "oldest surviving snapshot is one commit old");
    history_shutdown(&ring);
}

static void test_shape_lifecycle(void) {
    Level level;
    level_init(&level);
    check(level.next_shape_id == 1 && level.shape_count == 0,
          "level_init starts shape ids at 1");

    ShapeId shape = level_add_shape(&level);
    check(shape == 1 && level.shape_count == 1,
          "level_add_shape returns a new id");
    check(level.shapes[0].id == 1 && !level.shapes[0].closed,
          "a new shape is open");
    check(level_shape_by_id(&level, shape) == &level.shapes[0] &&
              level_shape_by_id_const(&level, shape) == &level.shapes[0],
          "shape lookup finds the shape");
    check(level_shape_by_id(&level, 99) == NULL &&
              level_shape_by_id_const(&level, 0) == NULL,
          "shape lookup rejects unknown ids");

    check(level_add_wall_to_shape(&level, 99, v2(0, 0), v2(10, 0)) == -1,
          "add wall rejects an unknown shape");
    check(level_add_wall_to_shape(&level, shape, v2(0, 0), v2(10, 10)) == -1,
          "add wall rejects a non-orthogonal segment");
    check(level_add_wall_to_shape(&level, shape, v2(5, 5), v2(5, 5)) == -1,
          "add wall rejects a zero-length segment");

    int w0 = level_add_wall_to_shape(&level, shape, v2(0, 0), v2(100, 0));
    int w1 = level_add_wall_to_shape(&level, shape, v2(100, 0), v2(100, 100));
    int w2 = level_add_wall_to_shape(&level, shape, v2(100, 100), v2(0, 100));
    int w3 = level_add_wall_to_shape(&level, shape, v2(0, 100), v2(0, 0));
    check(w0 == 0 && w1 == 1 && w2 == 2 && w3 == 3,
          "shape walls are appended in wall order");
    check(level.walls[0].shape == shape && level.walls[1].shape == shape &&
              level.walls[2].shape == shape && level.walls[3].shape == shape,
          "each added wall records its shape id");

    int indices[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    check(level_shape_wall_indices(&level, shape, indices, 8) == 4 &&
              indices[0] == 0 && indices[1] == 1 &&
              indices[2] == 2 && indices[3] == 3,
          "shape wall indices follow insertion order");
    check(level_shape_wall_indices(&level, shape, indices, 2) == 2 &&
              indices[0] == 0 && indices[1] == 1,
          "shape wall indices are capped by the output count");
    check(level_shape_wall_indices(&level, 99, indices, 8) == -1,
          "shape wall indices rejects an unknown shape");

    Vec2 polygon[8];
    check(level_shape_polygon(&level, shape, polygon, 8) == -1,
          "an open shape has no polygon");
    check(level_close_shape(&level, shape), "close a four-wall loop");
    check(level.shapes[0].closed, "closing keeps the shape closed");
    int count = level_shape_polygon(&level, shape, polygon, 8);
    check(count == 4 &&
              nearv(polygon[0], v2(0, 0)) &&
              nearv(polygon[1], v2(100, 0)) &&
              nearv(polygon[2], v2(100, 100)) &&
              nearv(polygon[3], v2(0, 100)),
          "polygon returns the vertices in wall order without a closing point");
    check(level_shape_polygon(&level, shape, polygon, 3) == -1,
          "polygon rejects a too-small output buffer");
    check(!level_close_shape(&level, 99), "close rejects an unknown shape");

    Level chain;
    level_init(&chain);
    ShapeId open = level_add_shape(&chain);
    level_add_wall_to_shape(&chain, open, v2(0, 0), v2(10, 0));
    level_add_wall_to_shape(&chain, open, v2(10, 0), v2(10, 10));
    level_add_wall_to_shape(&chain, open, v2(10, 10), v2(0, 10));
    check(!level_close_shape(&chain, open),
          "three walls that do not reconnect cannot close");

    Level mixed;
    level_init(&mixed);
    ShapeId mixed_shape = level_add_shape(&mixed);
    level_add_wall_to_shape(&mixed, mixed_shape, v2(0, 0), v2(100, 0));
    level_add_wall_to_shape(&mixed, mixed_shape, v2(100, 100), v2(100, 0));
    level_add_wall_to_shape(&mixed, mixed_shape, v2(100, 100), v2(0, 100));
    level_add_wall_to_shape(&mixed, mixed_shape, v2(0, 0), v2(0, 100));
    check(level_close_shape(&mixed, mixed_shape),
          "close handles walls whose stored direction alternates");
    check(level_shape_polygon(&mixed, mixed_shape, polygon, 8) == 4 &&
              nearv(polygon[0], v2(0, 0)) &&
              nearv(polygon[1], v2(100, 0)) &&
              nearv(polygon[2], v2(100, 100)) &&
              nearv(polygon[3], v2(0, 100)),
          "polygon follows the connected traversal order");
}

static void test_delete_shape(void) {
    Level level;
    level_init(&level);
    int keep_wall = level_add_wall(&level, v2(0, 0), v2(10, 0));
    ShapeId keep_shape = level.walls[keep_wall].shape;
    WallId keep_id = level.walls[keep_wall].id;

    int walls[4];
    ShapeId gone = add_rectangle(&level, v2(100, 0), 100, 100, walls);
    check(level_close_shape(&level, gone), "close the shape slated for deletion");
    level_add_door(&level, keep_id, 0.5f, 4);
    level_add_door(&level, level.walls[walls[0]].id, 0.5f, 20);
    level_add_door(&level, level.walls[walls[1]].id, 0.5f, 20);

    check(level_delete_shape(&level, gone),
          "delete shape removes a closed shape");
    check(level.wall_count == 1 && level.walls[0].id == keep_id &&
              level.walls[0].shape == keep_shape,
          "delete shape removes only that shape's walls");
    check(level.shape_count == 1 &&
              level_shape_by_id(&level, gone) == NULL &&
              level_shape_by_id(&level, keep_shape) != NULL,
          "delete shape removes the shape record");
    check(level.door_count == 1 && level.doors[0].wall == keep_id,
          "delete shape cascades doors hosted by its walls");
    check(!level_delete_shape(&level, gone),
          "delete shape rejects an id that no longer exists");

    Level chain;
    level_init(&chain);
    ShapeId id = level_add_shape(&chain);
    level_add_wall_to_shape(&chain, id, v2(0, 0), v2(10, 0));
    level_add_wall_to_shape(&chain, id, v2(10, 0), v2(20, 0));
    check(level_delete_wall(&chain, 0) && chain.wall_count == 1 &&
              chain.shape_count == 1 && chain.walls[0].shape == id,
          "deleting one wall keeps its shape while walls remain");
    check(level_delete_wall(&chain, 0) && chain.wall_count == 0 &&
              chain.shape_count == 0,
          "deleting the last wall removes its shape");
}

static void test_shape_reshape(void) {
    Level level;
    level_init(&level);
    int walls[4];
    ShapeId shape = add_rectangle(&level, v2(0, 0), 100, 100, walls);
    check(level_close_shape(&level, shape), "close the rectangle before sliding");
    level_add_door(&level, level.walls[walls[0]].id, 0.5f, 20);
    WallId bottom_id = level.walls[walls[0]].id;

    check(level_slide_wall_edge(&level, walls[0], 40, 8),
          "slide a horizontal edge by +40");
    check(nearv(level.walls[walls[0]].a, v2(0, 40)) &&
              nearv(level.walls[walls[0]].b, v2(100, 40)),
          "the slid edge moves along its normal");
    check(nearv(level.walls[walls[1]].a, v2(100, 40)) &&
              nearv(level.walls[walls[1]].b, v2(100, 100)),
          "the right neighbor stretches from the moved endpoint");
    check(nearv(level.walls[walls[3]].a, v2(0, 100)) &&
              nearv(level.walls[walls[3]].b, v2(0, 40)),
          "the left neighbor stretches from the moved endpoint");
    check(nearv(level.walls[walls[2]].a, v2(100, 100)) &&
              nearv(level.walls[walls[2]].b, v2(0, 100)),
          "the opposite edge stays put");
    check(level.shapes[0].closed, "the shape stays closed after the slide");

    Vec2 polygon[8];
    check(level_shape_polygon(&level, shape, polygon, 8) == 4 &&
              nearv(polygon[0], v2(0, 40)) &&
              nearv(polygon[1], v2(100, 40)) &&
              nearv(polygon[2], v2(100, 100)) &&
              nearv(polygon[3], v2(0, 100)),
          "the reshaped polygon follows the moved edge");
    check(level.door_count == 1 && level.doors[0].wall == bottom_id &&
              nearf(level.doors[0].t, 0.5f) &&
              nearv(level_door_center(&level, &level.doors[0]), v2(50, 40)),
          "a door keeps its wall id and t and rides the moved edge");

    Level clamp;
    level_init(&clamp);
    int clamp_walls[4];
    ShapeId clamp_shape = add_rectangle(&clamp, v2(0, 0), 100, 100, clamp_walls);
    level_close_shape(&clamp, clamp_shape);
    check(level_slide_wall_edge(&clamp, clamp_walls[0], 140, 8),
          "a long slide clamps to the neighbor minimum length");
    check(nearv(clamp.walls[clamp_walls[0]].a, v2(0, 92)) &&
              nearv(clamp.walls[clamp_walls[0]].b, v2(100, 92)),
          "the clamped edge stops before the neighbor collapses");
    check(nearf(wall_length(&clamp.walls[clamp_walls[1]]), 8) &&
              nearf(wall_length(&clamp.walls[clamp_walls[3]]), 8),
          "the vertical neighbors keep exactly the minimum length");
    check(clamp.shapes[0].closed, "the clamped shape stays closed");

    Level before = clamp;
    check(!level_slide_wall_edge(&clamp, clamp_walls[0], 50, 8) &&
              memcmp(&clamp, &before, sizeof clamp) == 0,
          "a slide that would collapse a neighbor is rejected unchanged");
    check(!level_slide_wall_edge(&clamp, -1, 10, 8) &&
              memcmp(&clamp, &before, sizeof clamp) == 0,
          "a slide rejects a bad wall index unchanged");

    Level sideways;
    level_init(&sideways);
    int side_walls[4];
    ShapeId side_shape = add_rectangle(&sideways, v2(0, 0), 100, 100,
                                       side_walls);
    level_close_shape(&sideways, side_shape);
    check(level_slide_wall_edge(&sideways, side_walls[1], -40, 8),
          "slide a vertical edge by -40");
    check(nearv(sideways.walls[side_walls[1]].a, v2(60, 0)) &&
              nearv(sideways.walls[side_walls[1]].b, v2(60, 100)),
          "the vertical edge moves on x");
    check(nearv(sideways.walls[side_walls[0]].b, v2(60, 0)) &&
              nearv(sideways.walls[side_walls[2]].a, v2(60, 100)) &&
              nearv(sideways.walls[side_walls[3]].a, v2(0, 100)),
          "the horizontal neighbors stretch and the opposite edge stays");
    check(sideways.shapes[0].closed,
          "the vertically slid shape stays closed");
}

static void test_slide_edges_multi(void) {
    Level level;
    level_init(&level);
    int walls[4];
    ShapeId shape = add_rectangle(&level, v2(0, 0), 100, 100, walls);
    check(level_close_shape(&level, shape),
          "close the rectangle before multi-edge sliding");

    bool bottom_only[4] = { true, false, false, false };
    check(level_slide_edges(&level, bottom_only, 4, v2(30, 40), 8),
          "slide the bottom edge and ignore the pointer x delta");
    check(nearv(level.walls[walls[0]].a, v2(0, 40)) &&
              nearv(level.walls[walls[0]].b, v2(100, 40)),
          "the selected bottom edge moves only on y");
    check(nearv(level.walls[walls[1]].a, v2(100, 40)) &&
              nearv(level.walls[walls[1]].b, v2(100, 100)) &&
              nearv(level.walls[walls[3]].a, v2(0, 100)) &&
              nearv(level.walls[walls[3]].b, v2(0, 40)),
          "the two vertical neighbours stretch to the moved edge");
    check(nearv(level.walls[walls[2]].a, v2(100, 100)) &&
              nearv(level.walls[walls[2]].b, v2(0, 100)),
          "the top edge stays where it was");
    check(level.shapes[0].closed &&
              nearv(level.walls[walls[1]].a, level.walls[walls[0]].b) &&
              nearv(level.walls[walls[3]].b, level.walls[walls[0]].a) &&
              nearv(level.walls[walls[2]].a, level.walls[walls[1]].b),
          "the rectangle stays closed after sliding one edge");

    Level both;
    level_init(&both);
    int both_walls[4];
    ShapeId both_shape = add_rectangle(&both, v2(0, 0), 100, 100, both_walls);
    level_close_shape(&both, both_shape);
    bool bottom_and_top[4] = { true, false, true, false };
    check(level_slide_edges(&both, bottom_and_top, 4, v2(0, 40), 8),
          "slide the bottom and top edges together");
    check(nearv(both.walls[both_walls[0]].a, v2(0, 40)) &&
              nearv(both.walls[both_walls[0]].b, v2(100, 40)) &&
              nearv(both.walls[both_walls[2]].a, v2(100, 140)) &&
              nearv(both.walls[both_walls[2]].b, v2(0, 140)),
          "both horizontal edges move by the full delta");
    check(nearf(wall_length(&both.walls[both_walls[1]]), 100) &&
              nearf(wall_length(&both.walls[both_walls[3]]), 100),
          "the vertical neighbours keep their length when both edges move");
    check(both.shapes[0].closed &&
              nearv(both.walls[both_walls[1]].a, both.walls[both_walls[0]].b) &&
              nearv(both.walls[both_walls[1]].b, both.walls[both_walls[2]].a) &&
              nearv(both.walls[both_walls[3]].b, both.walls[both_walls[0]].a),
          "the shape stays closed when both edges move");

    Level corner;
    level_init(&corner);
    int corner_walls[4];
    ShapeId corner_shape = add_rectangle(&corner, v2(0, 0), 100, 100,
                                         corner_walls);
    level_close_shape(&corner, corner_shape);
    bool left_and_bottom[4] = { true, false, false, true };
    check(level_slide_edges(&corner, left_and_bottom, 4, v2(40, 30), 8),
          "drag the left and bottom edges diagonally");
    check(nearv(corner.walls[corner_walls[0]].a, v2(40, 30)) &&
              nearv(corner.walls[corner_walls[0]].b, v2(100, 30)),
          "the bottom edge follows the shared corner in both axes");
    check(nearv(corner.walls[corner_walls[3]].a, v2(40, 100)) &&
              nearv(corner.walls[corner_walls[3]].b, v2(40, 30)),
          "the left edge follows the shared corner in both axes");
    check(nearv(corner.walls[corner_walls[1]].a, v2(100, 30)) &&
              nearv(corner.walls[corner_walls[1]].b, v2(100, 100)) &&
              nearv(corner.walls[corner_walls[2]].a, v2(100, 100)) &&
              nearv(corner.walls[corner_walls[2]].b, v2(40, 100)),
          "the unselected top and right edges follow their moved endpoints");
    check(corner.shapes[0].closed &&
              nearv(corner.walls[corner_walls[0]].a,
                    corner.walls[corner_walls[3]].b) &&
              nearv(corner.walls[corner_walls[1]].a,
                    corner.walls[corner_walls[0]].b) &&
              nearv(corner.walls[corner_walls[2]].a,
                    corner.walls[corner_walls[1]].b) &&
              nearv(corner.walls[corner_walls[3]].a,
                    corner.walls[corner_walls[2]].b),
          "the diagonally dragged shape stays closed");

    Level clamp;
    level_init(&clamp);
    int clamp_walls[4];
    ShapeId clamp_shape = add_rectangle(&clamp, v2(0, 0), 100, 100,
                                        clamp_walls);
    level_close_shape(&clamp, clamp_shape);
    bool clamp_edges[4] = { true, false, false, true };
    check(level_slide_edges(&clamp, clamp_edges, 4, v2(140, 140), 60),
          "an oversized diagonal drag clamps instead of collapsing walls");
    bool all_keep = true;
    float shortest = INFINITY;
    for (int i = 0; i < clamp.wall_count; i++) {
        float len = wall_length(&clamp.walls[i]);
        shortest = fminf(shortest, len);
        if (len < 60.0f - 1e-4f) all_keep = false;
    }
    check(all_keep && nearf(shortest, 60.0f),
          "the clamp keeps every wall at or above min_len");
    check(nearv(clamp.walls[clamp_walls[0]].a, v2(40, 40)) &&
              nearv(clamp.walls[clamp_walls[0]].b, v2(100, 40)) &&
              nearv(clamp.walls[clamp_walls[3]].b, v2(40, 40)),
          "the clamp stops the moving corner before either wall flips");
    check(clamp.shapes[0].closed, "the clamped shape stays closed");

    Level still = clamp;
    check(!level_slide_edges(&clamp, clamp_edges, 4, v2(0, 0), 60) &&
              memcmp(&clamp, &still, sizeof clamp) == 0,
          "a zero multi-edge delta changes nothing");
}

static void test_move_walls(void) {
    Level level;
    level_init(&level);
    int walls[4];
    ShapeId shape = add_rectangle(&level, v2(0, 0), 100, 100, walls);
    level_close_shape(&level, shape);
    WallId ids[4];
    for (int i = 0; i < 4; i++) ids[i] = level.walls[walls[i]].id;
    level_add_door(&level, ids[0], 0.5f, 20);

    bool selected[4] = { true, true, true, true };
    Vec2 delta = v2(25, -15);
    check(level_move_walls(&level, selected, 4, delta),
          "move every wall of a shape by the drag delta");
    check(nearv(level.walls[walls[0]].a, v2(25, -15)) &&
              nearv(level.walls[walls[0]].b, v2(125, -15)) &&
              nearv(level.walls[walls[2]].a, v2(125, 85)) &&
              nearv(level.walls[walls[2]].b, v2(25, 85)),
          "each wall translates by exactly the delta");
    check(level.walls[walls[0]].id == ids[0] &&
              level.walls[walls[1]].id == ids[1] &&
              level.walls[walls[2]].id == ids[2] &&
              level.walls[walls[3]].id == ids[3] &&
              level.walls[walls[0]].shape == shape,
          "wall ids and shape membership survive the move");
    check(level.door_count == 1 && level.doors[0].wall == ids[0] &&
              nearf(level.doors[0].t, 0.5f) &&
              nearv(level_door_center(&level, &level.doors[0]), v2(75, -15)),
          "a door stays on its wall and rides the translation");

    Level before = level;
    check(!level_move_walls(&level, selected, 4, v2(0, 0)) &&
              memcmp(&level, &before, sizeof level) == 0,
          "move walls rejects a zero delta unchanged");
    check(!level_move_walls(&level, selected, 4, v2(NAN, 0)) &&
              memcmp(&level, &before, sizeof level) == 0,
          "move walls rejects a NaN delta unchanged");
    check(!level_move_walls(&level, selected, 4, v2(INFINITY, 0)) &&
              memcmp(&level, &before, sizeof level) == 0,
          "move walls rejects an infinite delta unchanged");
    check(!level_move_walls(&level, NULL, 4, delta) &&
              memcmp(&level, &before, sizeof level) == 0,
          "move walls rejects a null selection unchanged");
    check(!level_move_walls(&level, selected, -1, delta) &&
              memcmp(&level, &before, sizeof level) == 0,
          "move walls rejects a negative count unchanged");
    check(!level_move_walls(&level, selected, level.wall_count + 1, delta) &&
              memcmp(&level, &before, sizeof level) == 0,
          "move walls rejects a count past the wall array unchanged");
}

static void test_shape_at(void) {
    Level level;
    level_init(&level);
    int outer_walls[4];
    int inner_walls[4];
    ShapeId outer = add_rectangle(&level, v2(0, 0), 200, 200, outer_walls);
    check(level_close_shape(&level, outer),
          "close the outer rectangle for shape_at");
    ShapeId inner = add_rectangle(&level, v2(50, 50), 100, 100, inner_walls);
    check(level_close_shape(&level, inner),
          "close the nested rectangle for shape_at");

    check(level_shape_at(&level, v2(100, 100)) == 1,
          "shape_at returns the innermost closed shape under the point");
    check(level_shape_at(&level, v2(20, 20)) == 0,
          "shape_at returns the outer shape outside the inner one");
    check(level_shape_at(&level, v2(300, 20)) == -1,
          "shape_at misses a point outside every closed shape");

    int twin_walls[4];
    ShapeId twin = add_rectangle(&level, v2(50, 50), 100, 100, twin_walls);
    check(level_close_shape(&level, twin),
          "close a second identical outline for shape_at");
    check(level_shape_at(&level, v2(100, 100)) == 2,
          "shape_at prefers the newest of two identical outlines");

    Level open;
    level_init(&open);
    ShapeId open_shape = level_add_shape(&open);
    level_add_wall_to_shape(&open, open_shape, v2(0, 0), v2(100, 0));
    level_add_wall_to_shape(&open, open_shape, v2(100, 0), v2(100, 100));
    level_add_wall_to_shape(&open, open_shape, v2(0, 0), v2(0, 100));
    check(!open.shapes[0].closed &&
              level_shape_at(&open, v2(50, 50)) == -1,
          "shape_at ignores an open shape");
}

static void test_save_load_shapes(void) {
    Level src;
    level_init(&src);
    int walls[4];
    ShapeId shape = add_rectangle(&src, v2(0, 0), 100, 100, walls);
    check(level_close_shape(&src, shape), "prepare a closed shape");
    level_add_door(&src, src.walls[walls[0]].id, 0.5f, 20);

    char path[256];
    snprintf(path, sizeof path, "/tmp/ink_ribbon_shapes_%ld.lvl",
             (long)getpid());
    LevelError err;
    check(level_save(&src, path, &err), "save a closed filled shape");

    char file[8192] = { 0 };
    FILE* f = fopen(path, "rb");
    check(f != NULL, "saved shape file is readable");
    if (f) {
        size_t got = fread(file, 1, sizeof file - 1, f);
        file[got] = '\0';
        fclose(f);
    }
    check(strncmp(file, "ink-level 3\n", 12) == 0,
          "shape save starts with ink-level 3");
    check(strstr(file, "shape 1 1 0\n") != NULL,
          "shape save writes the closed flag and the legacy fill column as 0");
    check(strstr(file, "wall 1 1 0.000 0.000 100.000 0.000\n") != NULL,
          "shape save writes the wall's shape id");

    Level dst;
    level_init(&dst);
    check(level_load(&dst, path, &err), "load a version 2 shape file");
    check(dst.shape_count == 1 && dst.shapes[0].id == 1 &&
              dst.shapes[0].closed,
          "roundtrip preserves the closed flag");
    check(dst.wall_count == 4 && dst.walls[0].shape == 1 &&
              dst.walls[1].shape == 1 && dst.walls[2].shape == 1 &&
              dst.walls[3].shape == 1,
          "roundtrip preserves every wall's shape id");
    check(dst.next_shape_id == 2,
          "shape load sets next_shape_id to max shape id + 1");
    Vec2 polygon[8];
    check(level_shape_polygon(&dst, 1, polygon, 8) == 4 &&
              nearv(polygon[2], v2(100, 100)),
          "the loaded shape keeps its polygon");
    int added = level_add_wall(&dst, v2(0, 0), v2(0, 50));
    check(added == 4 && dst.walls[4].id == 5 && dst.walls[4].shape == 2 &&
              dst.next_shape_id == 3,
          "a wall added after loading gets the next wall and shape ids");

    char v1_path[256];
    snprintf(v1_path, sizeof v1_path, "/tmp/ink_ribbon_v1_%ld.lvl",
             (long)getpid());
    check(write_text_file(v1_path,
                          "ink-level 1\n"
                          "wall 10 0.000 0.000 50.000 0.000\n"
                          "wall 11 50.000 0.000 50.000 50.000\n"),
          "write a version 1 fixture");
    Level old;
    level_init(&old);
    check(level_load(&old, v1_path, &err), "version 1 files still load");
    check(old.shape_count == 2 && old.shapes[0].id == 1 &&
              old.shapes[1].id == 2 && !old.shapes[0].closed &&
              !old.shapes[1].closed &&
              old.walls[0].shape == 1 && old.walls[1].shape == 2,
          "version 1 gives every wall one fresh open shape");
    check(old.next_wall_id == 12 && old.next_shape_id == 3,
          "version 1 load continues both id sequences");

    char bad_path[256];
    snprintf(bad_path, sizeof bad_path, "/tmp/ink_ribbon_bad_shape_%ld.lvl",
             (long)getpid());
    Level intact;
    level_init(&intact);
    level_add_wall(&intact, v2(0, 0), v2(5, 0));
    Level snapshot = intact;

    check(write_text_file(bad_path,
                          "ink-level 2\n"
                          "wall 1 9 0.000 0.000 10.000 0.000\n"),
          "write an unknown-shape fixture");
    check(!level_load(&intact, bad_path, &err),
          "load rejects a wall on an unknown shape");
    check(strncmp(err.message, "line 2:", 7) == 0,
          "unknown shape error names the wall line");
    check(memcmp(&intact, &snapshot, sizeof intact) == 0,
          "a rejected shape load leaves the target level untouched");

    check(write_text_file(bad_path,
                          "ink-level 2\n"
                          "shape 1 0 0\n"
                          "shape 1 0 0\n"),
          "write a duplicate-shape fixture");
    check(!level_load(&intact, bad_path, &err),
          "load rejects duplicate shape ids");

    check(write_text_file(bad_path,
                          "ink-level 2\n"
                          "shape 1 1 0\n"
                          "wall 1 1 0.000 0.000 10.000 0.000\n"
                          "wall 2 1 10.000 0.000 10.000 10.000\n"),
          "write a short closed-shape fixture");
    check(!level_load(&intact, bad_path, &err),
          "load rejects a closed shape with fewer than three walls");

    check(write_text_file(bad_path,
                          "ink-level 2\n"
                          "shape 1 0 7\n"
                          "wall 1 1 0.000 0.000 10.000 0.000\n"),
          "write a legacy-fill fixture");
    check(level_load(&intact, bad_path, &err) && intact.shape_count == 1,
          "load ignores the legacy fill column");
    check(write_text_file(bad_path,
                          "ink-level 3\n"
                          "shape 1 0 0\n"
                          "room 1 4 0.000 0.000 10.000 0.000 10.000 10.000 "
                          "0.000 10.000\n"
                          "wall 1 1 0.000 0.000 10.000 0.000\n"),
          "write a legacy room fixture");
    check(level_load(&intact, bad_path, &err) && intact.wall_count == 1,
          "load skips a legacy room record");

    remove(path);
    remove(v1_path);
    remove(bad_path);
}

static void test_cut_wall_span(void) {
    Level level;
    int dropped = -1;

    /* A middle cut of one wall leaves the near piece on the old wall and the
       far piece on a new wall of the same shape. */
    level_init(&level);
    int index = level_add_wall(&level, v2(0, 0), v2(100, 0));
    ShapeId shape = level.walls[0].shape;
    WallId id = level.walls[0].id;
    check(level_cut_wall_span(&level, index, 0.2f, 0.6f, &dropped),
          "cut the middle out of a wall");
    check(level.wall_count == 2 && level.shape_count == 1 &&
              level.walls[0].id == id && level.walls[1].id != id &&
              level.walls[0].shape == shape && level.walls[1].shape == shape,
          "a middle cut leaves two walls on the one shape with a fresh wall id");
    check(nearv(level.walls[0].a, v2(0, 0)) &&
              nearv(level.walls[0].b, v2(20, 0)) &&
              nearv(level.walls[1].a, v2(60, 0)) &&
              nearv(level.walls[1].b, v2(100, 0)),
          "the cut leaves the walls on either side of the removed section");
    check(dropped == 0, "a cut with no doors drops no doors");

    /* The two parameters are an interval, not a direction. */
    level_init(&level);
    index = level_add_wall(&level, v2(0, 0), v2(100, 0));
    check(level_cut_wall_span(&level, index, 0.6f, 0.2f, &dropped) &&
              level.wall_count == 2 && nearv(level.walls[0].b, v2(20, 0)) &&
              nearv(level.walls[1].a, v2(60, 0)),
          "a reversed pair of parameters cuts the same section");

    /* Cutting a middle section of a closed outline keeps the shape record
       closed and adds one wall to it. */
    level_init(&level);
    int rect_walls[4];
    ShapeId rect = add_rectangle(&level, v2(0, 0), 100, 100, rect_walls);
    check(level_close_shape(&level, rect), "close the rectangle before a cut");
    check(level_cut_wall_span(&level, rect_walls[0], 0.2f, 0.6f, &dropped),
          "cut the middle out of a closed rectangle's top wall");
    const Shape* record = level_shape_by_id_const(&level, rect);
    int top_pieces = 0;
    for (int i = 0; i < level.wall_count; i++) {
        const Wall* w = &level.walls[i];
        if (w->shape == rect && nearf(w->a.y, 0) && nearf(w->b.y, 0)) top_pieces++;
    }
    check(level.wall_count == 5 && level.shape_count == 1 && record &&
              record->closed && top_pieces == 2,
          "a middle cut splits one wall in two and leaves the shape closed");

    /* A cut from the start removes the wall and leaves only the far piece. */
    level_init(&level);
    index = level_add_wall(&level, v2(0, 0), v2(100, 0));
    id = level.walls[0].id;
    check(level_cut_wall_span(&level, index, 0.0f, 0.4f, &dropped),
          "cut a wall from its start");
    check(level.wall_count == 1 && level.walls[0].id != id &&
              nearv(level.walls[0].a, v2(40, 0)) &&
              nearv(level.walls[0].b, v2(100, 0)),
          "a cut from the start leaves only the remaining wall");

    /* A cut to the end keeps the old wall for the near piece. */
    level_init(&level);
    index = level_add_wall(&level, v2(0, 0), v2(100, 0));
    id = level.walls[0].id;
    check(level_cut_wall_span(&level, index, 0.6f, 1.0f, &dropped),
          "cut a wall to its end");
    check(level.wall_count == 1 && level.walls[0].id == id &&
              nearv(level.walls[0].a, v2(0, 0)) &&
              nearv(level.walls[0].b, v2(60, 0)),
          "a cut to the end leaves one wall that keeps the old id");

    /* Removing every wall removes the shape with it. */
    level_init(&level);
    index = level_add_wall(&level, v2(0, 0), v2(100, 0));
    check(level_cut_wall_span(&level, index, 0.0f, 1.0f, &dropped) &&
              level.wall_count == 0 && level.shape_count == 0,
          "a cut of the whole wall removes the empty shape");

    /* Parameters outside 0..1 are clamped to the wall. */
    level_init(&level);
    index = level_add_wall(&level, v2(0, 0), v2(100, 0));
    check(level_cut_wall_span(&level, index, -0.5f, 1.5f, &dropped) &&
              level.wall_count == 0 && level.shape_count == 0,
          "parameters outside 0..1 clamp to the whole wall");

    /* Doors sort by centre: inside goes, before stays on the shortened wall,
       after moves to the new wall, and both keep their world centre. */
    level_init(&level);
    index = level_add_wall(&level, v2(0, 0), v2(100, 0));
    WallId host = level.walls[0].id;
    check(level_add_door(&level, host, 0.1f, 10) == 0 &&
              level_add_door(&level, host, 0.35f, 10) == 1 &&
              level_add_door(&level, host, 0.8f, 10) == 2,
          "place a door before, inside, and after the span");
    dropped = -1;
    check(level_cut_wall_span(&level, index, 0.2f, 0.6f, &dropped) &&
              level.door_count == 2 && dropped == 1,
          "a door inside the span goes and is counted");
    check(level.doors[0].wall == host &&
              nearv(level_door_center(&level, &level.doors[0]), v2(10, 0)),
          "a door before the span keeps its wall and world centre");
    check(level.doors[1].wall != host &&
              level.doors[1].wall == level.walls[1].id &&
              nearv(level_door_center(&level, &level.doors[1]), v2(80, 0)),
          "a door beyond the span moves to the new wall and keeps its centre");

    /* Rejections leave the level byte for byte untouched. */
    level_init(&level);
    level_add_wall(&level, v2(0, 0), v2(100, 0));
    level_add_door(&level, level.walls[0].id, 0.5f, 10);
    Level snapshot = level;
    dropped = 7;
    check(!level_cut_wall_span(&level, 0, 0.5f, 0.5f, &dropped) &&
              memcmp(&level, &snapshot, sizeof level) == 0 && dropped == 0,
          "a zero length span is rejected and changes nothing");
    check(!level_cut_wall_span(&level, 4, 0.1f, 0.2f, &dropped) &&
              memcmp(&level, &snapshot, sizeof level) == 0,
          "a wall index out of range is rejected and changes nothing");
    check(!level_cut_wall_span(&level, 0, 0.1f, 0.1004f, &dropped) &&
              memcmp(&level, &snapshot, sizeof level) == 0,
          "a span shorter than 1e-3 is rejected and changes nothing");
    check(!level_cut_wall_span(&level, 0, NAN, 0.5f, &dropped) &&
              memcmp(&level, &snapshot, sizeof level) == 0,
          "a non-finite parameter is rejected and changes nothing");
}

static bool has_wall(const Level* level, Vec2 a, Vec2 b) {
    for (int i = 0; i < level->wall_count; i++) {
        const Wall* w = &level->walls[i];
        if ((nearv(w->a, a) && nearv(w->b, b)) ||
            (nearv(w->a, b) && nearv(w->b, a))) {
            return true;
        }
    }
    return false;
}

static void test_move_vertex(void) {
    Level level;
    level_init(&level);
    int walls[4];
    ShapeId rect = add_rectangle(&level, v2(0, 0), 100, 100, walls);
    check(level_close_shape(&level, rect), "close the rectangle before a corner move");
    check(level_move_vertex(&level, v2(0, 0), v2(20, 30)),
          "move a rectangle corner in both axes");
    check(nearv(level.walls[walls[0]].a, v2(20, 30)) &&
              nearv(level.walls[walls[0]].b, v2(100, 30)),
          "the top wall follows the corner on its free axis");
    check(nearv(level.walls[walls[3]].a, v2(20, 100)) &&
              nearv(level.walls[walls[3]].b, v2(20, 30)),
          "the left wall follows the corner on its free axis");
    check(nearv(level.walls[walls[1]].a, v2(100, 30)) &&
              nearv(level.walls[walls[2]].b, v2(20, 100)),
          "the walls across the rectangle stay joined to the moved sides");
    bool orthogonal = true;
    for (int i = 0; i < level.wall_count; i++) {
        float dx = fabsf(level.walls[i].b.x - level.walls[i].a.x);
        float dy = fabsf(level.walls[i].b.y - level.walls[i].a.y);
        if (dx > 1e-3f && dy > 1e-3f) orthogonal = false;
    }
    const Shape* record = level_shape_by_id_const(&level, rect);
    check(orthogonal && record && record->closed,
          "a corner move keeps every wall orthogonal and the shape closed");
    int at_corner = 0;
    for (int i = 0; i < level.wall_count; i++) {
        if (nearv(level.walls[i].a, v2(20, 30)) ||
            nearv(level.walls[i].b, v2(20, 30))) {
            at_corner++;
        }
    }
    check(at_corner == 2,
          "the moved corner is shared by the two walls that meet there");

    /* A move that would collapse the left wall is rejected whole. */
    Level snapshot = level;
    check(!level_move_vertex(&level, v2(20, 30), v2(20, 100)) &&
              memcmp(&level, &snapshot, sizeof level) == 0,
          "a corner move that would collapse a wall is rejected and changes nothing");

    /* A T junction moves every wall at the shared point. */
    level_init(&level);
    level_add_wall(&level, v2(-50, 0), v2(0, 0));
    level_add_wall(&level, v2(0, 0), v2(50, 0));
    level_add_wall(&level, v2(0, 0), v2(0, 50));
    check(level_move_vertex(&level, v2(0, 0), v2(10, 10)),
          "move the shared corner of a T junction");
    int joined = 0;
    for (int i = 0; i < level.wall_count; i++) {
        if (nearv(level.walls[i].a, v2(10, 10)) ||
            nearv(level.walls[i].b, v2(10, 10))) {
            joined++;
        }
    }
    check(joined == 3 && nearv(level.walls[0].a, v2(-50, 10)) &&
              nearv(level.walls[1].b, v2(50, 10)) &&
              nearv(level.walls[2].b, v2(10, 50)),
          "all three walls at the corner stay joined and orthogonal");
    check(!level_move_vertex(&level, v2(200, 200), v2(210, 210)),
          "a corner move with no wall at the point is rejected");
}

static void test_cut_two_walls(void) {
    Level level;
    level_init(&level);
    int horizontal = level_add_wall(&level, v2(0, 0), v2(100, 0));
    WallId horizontal_id = level.walls[horizontal].id;
    int vertical = level_add_wall(&level, v2(50, -50), v2(50, 50));
    WallId vertical_id = level.walls[vertical].id;

    /* The editor's Delete finds each wall by id because a cut appends a tail
       and can renumber what follows. */
    WallId ids[2] = { horizontal_id, vertical_id };
    bool cut = true;
    for (int i = 0; i < 2; i++) {
        int index = level_find_wall_index(&level, ids[i]);
        int dropped = 0;
        if (index < 0 ||
            !level_cut_wall_span(&level, index, 0.2f, 0.6f, &dropped)) {
            cut = false;
        }
    }
    check(cut && level.wall_count == 4,
          "two spans on two walls cut in one call pattern leave four pieces");
    check(has_wall(&level, v2(0, 0), v2(20, 0)) &&
              has_wall(&level, v2(60, 0), v2(100, 0)) &&
              has_wall(&level, v2(50, -50), v2(50, -30)) &&
              has_wall(&level, v2(50, 10), v2(50, 50)),
          "both walls keep the pieces outside their spans");
    check(level_wall_by_id_const(&level, horizontal_id) &&
              level_wall_by_id_const(&level, vertical_id),
          "each cut wall keeps its id on the near piece");
}

static void test_nav_grid(void) {
    Level level;
    level_init(&level);
    int walls[4];
    ShapeId room = add_rectangle(&level, v2(-100, -100), 200, 200, walls);
    level_close_shape(&level, room);
    int inner = level_add_wall(&level, v2(0, -100), v2(0, 100));
    WallId inner_id = level.walls[inner].id;
    NavGrid nav;
    check(nav_build(&nav, &level, NAV_CELL_SIZE, 3.2f),
          "nav grid builds inside the room");
    int start = nav_cell_at_world(&nav, v2(-40, 0));
    int goal = nav_cell_at_world(&nav, v2(40, 0));
    int path[NAV_MAX_PATH];
    check(start >= 0 && goal >= 0, "nav finds the start and goal cells");
    check(nav_find_path(&nav, start, goal, path, NAV_MAX_PATH) == 0,
          "a solid wall blocks the path");

    check(level_add_door(&level, inner_id, 0.5f, 20.0f) == 0,
          "add a door through the blocking wall");
    check(nav_build(&nav, &level, NAV_CELL_SIZE, 3.2f),
          "nav grid rebuilds with the door");
    start = nav_cell_at_world(&nav, v2(-40, 0));
    goal = nav_cell_at_world(&nav, v2(40, 0));
    int count = nav_find_path(&nav, start, goal, path, NAV_MAX_PATH);
    check(count > 1, "the path crosses the door");
    if (count > 1) {
        bool through_door = false;
        for (int i = 1; i < count; i++) {
            Vec2 a = nav_cell_center(&nav, path[i - 1]);
            Vec2 b = nav_cell_center(&nav, path[i]);
            if (a.x <= 0.0f && b.x >= 0.0f) through_door = true;
        }
        check(through_door, "the path crosses from one side of the door to the other");
        Vec2 smooth[NAV_MAX_PATH];
        int smooth_count = nav_smooth_path(&nav, path, count, smooth,
                                           NAV_MAX_PATH);
        check(smooth_count > 1, "the path smooths to at least two waypoints");
        bool clear = true;
        for (int i = 1; i < smooth_count; i++) {
            if (!nav_line_clear(&nav, smooth[i - 1], smooth[i])) clear = false;
        }
        check(clear, "every smoothed segment stays on open cells");
        check(v2_dist(smooth[0], nav_cell_center(&nav, path[0])) < 0.01f &&
                  v2_dist(smooth[smooth_count - 1],
                          nav_cell_center(&nav, path[count - 1])) < 0.01f,
              "smoothing keeps the route endpoints");
    }
}

static void test_player(void) {
    Level level;
    level_init(&level);
    check(level_is_empty(&level), "a level with no player is empty");
    check(level_place_player(&level, v2(12, 34), 1.5f),
          "place the one player marker");
    check(level.player_present && nearv(level.player_pos, v2(12, 34)) &&
              nearf(level.player_heading, 1.5f),
          "the player keeps its position and heading");
    Rect b = level_bounds(&level);
    check(nearv(b.min, v2(12, 34)) && nearv(b.max, v2(12, 34)),
          "the player contributes to the level bounds");
    char path[256];
    snprintf(path, sizeof path, "/tmp/ink_ribbon_player_%ld.lvl", (long)getpid());
    LevelError err;
    check(level_save(&level, path, &err), "save a player level");
    Level loaded;
    level_init(&loaded);
    check(level_load(&loaded, path, &err) && loaded.player_present &&
              nearv(loaded.player_pos, v2(12, 34)) &&
              nearf(loaded.player_heading, 1.5f),
          "the player survives save and load");
    check(level_remove_player(&loaded) && !loaded.player_present,
          "remove the player");
    remove(path);
}

int main(void) {
    test_geometry();
    test_polygon();
    test_walls();
    test_doors();
    test_labels();
    test_stairs();
    test_stairs_flip();
    test_rect_union();
    test_rect_union_save_load();
    test_save_load();
    test_stair_migration();
    test_bounds();
    test_history();
    test_shape_lifecycle();
    test_delete_shape();
    test_shape_reshape();
    test_slide_edges_multi();
    test_move_walls();
    test_shape_at();
    test_cut_wall_span();
    test_cut_two_walls();
    test_move_vertex();
    test_delete_edge_reopens_shape();
    test_save_load_shapes();
    test_nav_grid();
    test_player();

    if (failures == 0) {
        printf("ALL CORE TESTS PASSED\n");
        return 0;
    }
    printf("%d CORE CHECK(S) FAILED\n", failures);
    return 1;
}
