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
    printf("walls %d shapes %d doors %d labels %d stairs %d\n",
           level.wall_count, level.shape_count, level.door_count,
           level.label_count, level.stairs_count);
    printf("bounds %.1f %.1f %.1f %.1f\n", b.min.x, b.min.y, b.max.x, b.max.y);
    for (int i = 0; i < level.shape_count; i++) {
        const Shape* shape = &level.shapes[i];
        printf("shape %u closed %d\n", shape->id, shape->closed ? 1 : 0);
    }
    for (int i = 0; i < level.door_count; i++) {
        printf("door %d wall %u width %.1f\n", i, level.doors[i].wall,
               level.doors[i].width);
    }
    return 0;
}
