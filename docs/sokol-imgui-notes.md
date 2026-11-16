# How sokol and Dear ImGui fit together here

## Overview

The editor is one sokol_app window. sokol_app owns the OS window and input. sokol_gfx owns the swapchain pass. sokol_imgui (`vendor/sokol/util/sokol_imgui.h`) adapts Dear ImGui 1.93 to both. All level content is drawn as 2D primitives into ImGui draw lists, so the project needs no custom sokol_gfx pipeline, vertex format, or shader.

The application stays in C. Dear ImGui is C++, so `vendor/dcimgui` supplies `cimgui.h` and a C ABI with the `ig` prefix. `imgui.cpp` and friends compile as C++, the app compiles as gnu11, and g++ links the result.

## Key concepts

`SOKOL_IMPL` must be defined in exactly one translation unit before including the sokol headers. That unit is `src/main.c`. `SOKOL_IMGUI_IMPL` is a separate define for `sokol_imgui.h`.

sokol_app calls `init`, `frame`, `event`, and `cleanup` on the main thread. All ImGui and sokol_gfx calls happen on that thread.

The frame order is fixed. `simgui_new_frame` starts ImGui's frame with display size, delta time, and DPI scale. UI code runs. `simgui_flush` builds ImGui draw lists and updates its font texture. Only then may `sg_begin_pass`, `simgui_draw`, `sg_end_pass`, and `sg_commit` run.

Input arrives through `sapp_event` and is forwarded to `simgui_handle_event`. The app then reads ImGui's IO and item state, not the raw events, for editor interaction. This keeps the canvas and the panels on one input model.

## How the canvas works

The canvas is an ImGui window with no title bar, no scrolling, and a fixed position derived from the viewport. An invisible button covers its content area so ImGui tracks hover, click, and drag ownership. The level is drawn with `ImDrawList` calls clipped to that window.

Two transforms connect the level document to the screen. `world_to_screen` maps a world point through camera center, zoom, and canvas rect. `screen_to_world` inverts it. Grid snapping rounds world coordinates. Hit testing runs in world space with a pixel-sized tolerance divided by zoom.

Mouse wheel zooms around the cursor. Middle-drag or space-drag pans. Keyboard shortcuts route through `igIsKeyPressed` after checking that no ImGui text field wants the keyboard.

## Where things live

`src/main.c` is the sokol lifecycle and window setup. `src/level.h` is the level document and pure operations. `src/geom.*` is vector, segment, and polygon math. `src/editor.h` is UI state and the interaction state machine. `src/ui_*.c` draws panels and the canvas and mutates the document only through `level.*` operations.

## Gotchas

Calling `simgui_flush` after a render pass starts trips an assertion. Calling ImGui outside a `new_frame` and `flush` pair crashes. `ImDrawList` vertices are capped by `simgui_desc_t.max_vertices`, so the grid is culled to the visible rect rather than drawn at every level coordinate. Font texture creation is lazy in ImGui 1.93, and sokol_imgui installs the texture update callback, so the app must not create or bind its own font texture.

The default font is a 13 pixel bitmap. The editor removes it with `.no_default_font` and adds the embedded scalable default font at 15 pixels for legible room labels at multiple zoom levels.
