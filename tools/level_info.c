#include <stdio.h>

#include "level.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: level_info LEVEL.lvl\n");
        return 2;
    }
    Level level;
    LevelError err;
    if (!level_load(&level, argv[1], &err)) {
        fprintf(stderr, "%s: %s\n", argv[1], err.message);
        return 1;
    }
    Rect b = level_bounds(&level);
    printf("walls %d shapes %d rooms %d doors %d labels %d stairs %d\n",
           level.wall_count, level.shape_count, level.room_count, level.door_count,
           level.label_count, level.stairs_count);
    printf("bounds %.1f %.1f %.1f %.1f\n", b.min.x, b.min.y, b.max.x, b.max.y);
    for (int i = 0; i < level.shape_count; i++) {
        const Shape* shape = &level.shapes[i];
        const char* fill = shape->fill == SHAPE_FILL_BLUE ? "blue" :
                           shape->fill == SHAPE_FILL_RED ? "red" : "none";
        printf("shape %u closed %d fill %s\n", shape->id, shape->closed ? 1 : 0, fill);
    }
    for (int i = 0; i < level.room_count; i++) {
        printf("room %d color %s points %d\n", i,
               level.rooms[i].color == ROOM_BLUE ? "blue" : "red",
               level.rooms[i].count);
    }
    for (int i = 0; i < level.door_count; i++) {
        const char* color = level.doors[i].color == DOOR_WHITE ? "white" :
                            level.doors[i].color == DOOR_RED ? "red" : "blue";
        printf("door %d color %s wall %u width %.1f\n", i, color,
               level.doors[i].wall, level.doors[i].width);
    }
    return 0;
}
