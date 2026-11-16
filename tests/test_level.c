#include "geom.h"
#include "level.h"

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

static void test_rooms(void) {
    Level level;
    level_init(&level);
    level_add_wall(&level, v2(0, 0), v2(100, 0));
    level_add_wall(&level, v2(100, 0), v2(100, 100));
    level_add_wall(&level, v2(100, 100), v2(0, 100));
    level_add_wall(&level, v2(0, 100), v2(0, 0));
    check(level.wall_count == 4, "four walls close the loop");

    Vec2 square[4] = { { 0, 0 }, { 100, 0 }, { 100, 100 }, { 0, 100 } };
    Vec2 too_few[2] = { { 0, 0 }, { 10, 0 } };
    check(level_add_room(&level, too_few, 2, ROOM_BLUE) == -1,
          "reject room with fewer than 3 points");
    check(level_add_room(&level, square, 4, (RoomColor)7) == -1,
          "reject room with unknown color");
    check(level_add_room(&level, square, 4, ROOM_BLUE) == 0,
          "add room from closed wall loop");
    check(level_room_at(&level, v2(50, 50)) == 0, "room_at finds the room");
    check(level_room_at(&level, v2(150, 50)) == -1, "room_at misses outside");

    check(level_set_room_color(&level, 0, ROOM_RED) &&
              level.rooms[0].color == ROOM_RED,
          "recolor room");
    check(!level_set_room_color(&level, 9, ROOM_RED),
          "recolor rejects a bad room index");
    check(level_move_room(&level, 0, v2(10, 10)), "move room");
    check(level_room_at(&level, v2(5, 5)) == -1,
          "moved room leaves its old area");
    check(level_room_at(&level, v2(105, 105)) == 0,
          "moved room covers its new area");

    Vec2 raw[5] = { { 0, 0 }, { 50, 0 }, { 100, 0 }, { 100, 100 }, { 0, 100 } };
    check(level_add_room(&level, raw, 5, ROOM_RED) == 1 && level.rooms[1].count == 4,
          "add_room simplifies a collinear polygon");
    check(level_room_at(&level, v2(50, 50)) == 1,
          "room_at returns the last-added containing room");
    check(level_delete_room(&level, 1) && level.room_count == 1 &&
              level_room_at(&level, v2(50, 50)) == 0 &&
              level_room_at(&level, v2(5, 5)) == -1,
          "delete room removes it from hit testing");
    check(!level_delete_room(&level, 1), "delete room rejects a bad index");
}

static void test_doors(void) {
    Level level;
    level_init(&level);
    level_add_wall(&level, v2(0, 0), v2(100, 0));
    WallId host = level.walls[0].id;

    check(level_add_door(&level, 999, 0.5f, 20, DOOR_RED) == -1,
          "reject door on an unknown wall");
    check(level_add_door(&level, host, 0.5f, 20, DOOR_RED) == 0,
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

    check(level_add_door(&level, host, 0, 20, DOOR_WHITE) == 1,
          "add_door clamps t at the wall start");
    level_door_span(&level, &level.doors[1], &a, &b);
    check(nearv(a, v2(0, 0)) && nearv(b, v2(20, 0)),
          "add_door keeps the whole span on the wall");

    check(level_set_door_color(&level, 1, DOOR_BLUE) &&
              level.doors[1].color == DOOR_BLUE,
          "set door color");
    check(level_delete_door(&level, 0) && level.door_count == 1,
          "delete door");
    check(!level_delete_door(&level, 5), "delete door rejects a bad index");

    int other = level_add_wall(&level, v2(0, 100), v2(100, 100));
    check(level_add_door(&level, level.walls[other].id, 0.5f, 20, DOOR_BLUE) == 1,
          "add a door hosted by another wall");
    check(level_delete_wall(&level, 0) && level.wall_count == 1 &&
              level.door_count == 1 && level.doors[0].wall == 2,
          "delete wall cascades only its hosted doors");
    check(level_delete_wall(&level, 0) && level.wall_count == 0 &&
              level.door_count == 0,
          "delete the last wall leaves no orphan doors");
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

    check(level_add_stairs(&level, v2(100, 100), STAIRS_H, 1) == 0 &&
              level.stairs[0].steps == STAIR_MIN_STEPS,
          "stairs step count clamped low");
    Rect h = level_stairs_rect(&level.stairs[0]);
    check(nearv(h.min, v2(90, 78)) && nearv(h.max, v2(110, 122)),
          "stairs H rect is wide on x and tall on y");

    check(level_add_stairs(&level, v2(100, 100), STAIRS_V, 100) == 1 &&
              level.stairs[1].steps == STAIR_MAX_STEPS,
          "stairs step count clamped high");
    Rect v = level_stairs_rect(&level.stairs[1]);
    check(nearv(v.min, v2(78, -140)) && nearv(v.max, v2(122, 340)),
          "stairs V rect transposes the footprint");

    check(level_set_stairs(&level, 0, STAIRS_V, 10), "set stairs direction");
    Rect changed = level_stairs_rect(&level.stairs[0]);
    check(nearv(changed.min, v2(78, 50)) && nearv(changed.max, v2(122, 150)),
          "stairs rect follows set_stairs");
    check(level_move_stairs(&level, 0, v2(0, 0)) &&
              nearv(level.stairs[0].pos, v2(0, 0)),
          "move stairs");
    check(level_delete_stairs(&level, 0) && level.stairs_count == 1,
          "delete stairs");
    check(!level_delete_stairs(&level, 4), "delete stairs rejects a bad index");
    check(level_add_stairs(&level, v2(0, 0), (StairsDir)5, 4) == -1,
          "reject stairs with unknown direction");
}

static void test_save_load(void) {
    Level src;
    level_init(&src);
    level_add_wall(&src, v2(0, 0), v2(100, 0));
    level_add_wall(&src, v2(100, 0), v2(100, 100));
    level_add_wall(&src, v2(100, 100), v2(0, 100));
    level_delete_wall(&src, 0);

    Vec2 square[4] = { { 0, 0 }, { 100, 0 }, { 100, 100 }, { 0, 100 } };
    level_add_room(&src, square, 4, ROOM_BLUE);
    level_add_door(&src, src.walls[0].id, 0.5f, 20, DOOR_RED);
    level_add_label(&src, v2(10, 20), "attic stairs", 12);
    level_add_stairs(&src, v2(50, 50), STAIRS_H, 6);

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
            check(strcmp(first, "ink-level 1\n") == 0,
                  "saved file starts with ink-level 1");
        } else {
            check(false, "saved file starts with ink-level 1");
        }
        rewind(f);
        size_t got = fread(file, 1, sizeof file - 1, f);
        file[got] = '\0';
        fclose(f);
    }
    check(strstr(file, "wall 2 100.000 0.000 100.000 100.000\n") != NULL,
          "save writes wall records with %.3f floats and preserved ids");
    check(strstr(file, "door 1 2 0.500 20.000\n") != NULL,
          "save writes the door host and span");
    check(strstr(file, "label 10.000 20.000 12.000 attic stairs\n") != NULL,
          "save writes label text as the rest of the line");
    check(strstr(file, "label 0.000 0.000 8.000 bad name\n") != NULL,
          "save replaces control characters in label text with spaces");
    check(strstr(file, "stairs 50.000 50.000 0 6\n") != NULL,
          "save writes the stairs record");

    Level dst;
    level_init(&dst);
    check(level_load(&dst, path, &err), "level_load reads the saved level");
    check(err.message[0] == '\0', "level_load clears the error on success");
    check(dst.wall_count == 2 && dst.walls[0].id == 2 && dst.walls[1].id == 3,
          "roundtrip preserves wall ids");
    check(dst.next_wall_id == 4, "load sets next_wall_id to max wall id + 1");
    check(dst.door_count == 1 && dst.doors[0].wall == 2 &&
              nearf(dst.doors[0].t, 0.5f) && nearf(dst.doors[0].width, 20),
          "roundtrip preserves the door host and span");
    check(nearv(level_door_center(&dst, &dst.doors[0]), v2(100, 50)),
          "loaded door keeps its world position");
    check(dst.room_count == 1 && dst.rooms[0].color == ROOM_BLUE &&
              dst.rooms[0].count == 4,
          "roundtrip preserves the room");
    check(nearv(dst.rooms[0].pts[2], v2(100, 100)),
          "roundtrip preserves room points");
    check(dst.label_count == 2 &&
              strcmp(dst.labels[0].text, "attic stairs") == 0 &&
              strcmp(dst.labels[1].text, "bad name") == 0,
          "roundtrip preserves label text and spacing");
    check(dst.stairs_count == 1 && dst.stairs[0].dir == STAIRS_H &&
              dst.stairs[0].steps == 6,
          "roundtrip preserves stairs");
    Rect src_bounds = level_bounds(&src);
    Rect dst_bounds = level_bounds(&dst);
    check(nearv(src_bounds.min, dst_bounds.min) &&
              nearv(src_bounds.max, dst_bounds.max),
          "roundtrip preserves document bounds");

    int nw = level_add_wall(&dst, v2(0, 0), v2(0, 50));
    check(nw == 2 && dst.walls[2].id == 4,
          "loaded level continues the wall id sequence");

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

    Vec2 square[4] = { { 100, 100 }, { 200, 100 }, { 200, 200 }, { 100, 200 } };
    level_add_room(&level, square, 4, ROOM_RED);
    level_add_label(&level, v2(-5, -5), "hi", 10);
    level_add_stairs(&level, v2(300, 300), STAIRS_H, 2);
    Rect all = level_bounds(&level);
    check(nearv(all.min, v2(-5, -5)), "level_bounds includes label text box");
    check(nearf(all.max.x, 310) && nearf(all.max.y, 322),
          "level_bounds includes stairs and room extents");
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

int main(void) {
    test_geometry();
    test_polygon();
    test_walls();
    test_rooms();
    test_doors();
    test_labels();
    test_stairs();
    test_save_load();
    test_bounds();
    test_history();

    if (failures == 0) {
        printf("ALL CORE TESTS PASSED\n");
        return 0;
    }
    printf("%d CORE CHECK(S) FAILED\n", failures);
    return 1;
}
