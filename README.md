# Ink Ribbon Level Editor

A native 2D level editor for floor-plan style maps. It is written in C with sokol and Dear ImGui and it targets the look of the RE2 library reference map: near-black canvas, thin orthogonal walls, dark blue and red painted areas, white, red, and blue doors fixed to walls, room labels, and hatched stairs.

![The editor with the demo level](docs/screenshots/editor.png)

The demo level in `levels/re2-library.lvl` is loaded on startup. It has 42 walls, 6 painted areas, 10 doors, 6 labels, and 4 stair runs. `tools/gen_demo_map.py` regenerates it from readable coordinates.

## Build and run

The project is tested on Linux with X11 and OpenGL. It needs a C and C++ compiler, make, pkg-config, and the X11, Xi, Xcursor, and GL development packages.

Fedora:

```sh
sudo dnf install gcc gcc-c++ make pkgconf-pkg-config libX11-devel libXi-devel libXcursor-devel mesa-libGL-devel
```

Debian and Ubuntu:

```sh
sudo apt install gcc g++ make pkg-config libx11-dev libxi-dev libxcursor-dev libgl1-mesa-dev
```

`make verify` also needs `xdotool` and ImageMagick, which provide `import`, `convert`, and `compare`. The editor itself does not use them.

Build and run from the repository root:

```sh
git submodule update --init --recursive
make
./build/editor
```

Load another level with `./build/editor --level path/to/level.lvl`. The `levels/` path is relative to the working directory.

`sokol` is pinned at `93aa187b5d387cc24b7290b3ca3f13f61e139e7c` and `dcimgui` (Dear ImGui 1.93.0 C bindings) at `2c0e382fc10771bbd5dac15fc34f7fab84f87917`. Both live in `vendor/` as git submodules.

## What you can do

- Draw walls click by click. Every segment is horizontal or vertical and continues from the last click.
- Close a drawn loop by clicking its first point or pressing Enter. Close with an L-shaped pair when the last point is not aligned with the first. Press Esc to keep the chain open.
- Create a painted area automatically when a loop closes, or draw one explicitly with the Room tool. Recolor any area blue or red.
- Add white, red, or blue doors to walls. A door keeps a fixed thickness, slides along its wall, and changes length by dragging either end or with the inspector.
- Add text labels anywhere, edit the text in the inspector, resize and move them.
- Add standard stairs with 2 to 48 steps. The step count sets the run length, and each run is horizontal or vertical.
- Select, move, edit, and delete the placed objects. Deleting a wall also deletes the doors hosted by it.
- Click to select, shift-click to add or remove, or drag from empty canvas to marquee select. Delete removes the whole selection in one step, and dragging selected labels, stairs, and rooms moves them together.
- Snap to the grid on placement and movement. The grid sizes are 4, 8, 16, and 32 world units, and the Snap checkbox turns snapping off.
- Pan, zoom, fit the level, undo and redo, save, and load.

## Controls

| Input | Action |
| --- | --- |
| 1 to 6 | Select, Wall, Room, Door, Label, Stairs |
| Left click | Use the active tool |
| Shift plus left click | Add an object to the selection |
| Ctrl plus left click | Toggle an object in the selection |
| Drag on empty canvas | Marquee select; Shift adds to the selection |
| Middle drag, or Space plus left drag | Pan |
| Mouse wheel | Zoom around the cursor |
| Enter | Close a wall chain or room polygon |
| Esc | Clear the selection, cancel a draft, or cancel a drag |
| Backspace | Remove the last wall in the current chain |
| Delete | Delete every selected object |
| F | Fit the level |
| G | Toggle the grid |
| Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z | Undo, redo |
| Ctrl+S, Ctrl+Shift+S | Save, save as |
| Ctrl+O | Open |
| H | Controls dialog |

## Editing model

Walls are independent axis-aligned segments. Corners meet because the wall tool starts each new segment at the previous click. A wall has a stable id. Doors store that id, a normalized center position, and a world-space length, so a door follows its wall and never points at a deleted one.

Painted areas are explicit polygons. That keeps their color and identity when nearby walls change, and it allows fills that are not exact wall loops, as in the reference. Painted areas do not follow wall edits.

Labels store a position, a size, and a string. Stairs store a center, a direction, and a step count. `STAIR_STEP_DEPTH` and `STAIR_WIDTH` fix the tread size.

Selection is UI state and is never saved into a level. It is a bounded set per object kind plus a primary object. Shift-click adds an object, Ctrl-click toggles it, a marquee replaces or adds to the set, and Delete removes every selected object with one undo step. Dragging a selected label, stair, or room moves all selected labels, stairs, and rooms together. Doors keep their own slide and end-resize gestures. Walls stay selected but do not move.

Snap-to-grid applies to wall and room points, labels, stairs, door centers, door resize spans, and object drags. A group drag snaps from the primary object, so the relative spacing of the group stays intact. Snap is on by default. The grid sizes are 4, 8, 16, and 32 world units.

Undo stores whole level snapshots. Each completed change writes one snapshot. A wall click, a drag, and a property edit each undo as their own step. Closing a loop writes its closing walls and the fill together. The history keeps 64 levels.

## Selection and snapping

![Marquee selection in progress](docs/screenshots/marquee.png)

A plain click on an unselected object selects only it. Clicking an object already in a multi-selection keeps the group and makes it primary, so the group can be dragged. Shift-click adds an object and never removes one. Ctrl-click toggles it. Dragging from empty canvas draws a marquee, and Shift makes the marquee additive. The inspector shows per-kind counts for a multi-selection, and Delete removes the whole set in one undo step.

![Two selected labels](docs/screenshots/selection.png)

Dragging a selected label, stair, or room moves every selected label, stair, and room. A door in a multi-selection still slides along its wall or resizes from an end. Snap uses the grid size set in the left panel. The group snaps from the primary object's anchor, so the rest of the group keeps its offset.

The recommended interaction settings are now the only behavior. Door placement is click only, closing a wall loop always creates a fill, and a door always opens the wall behind it. The Prototype panel and its unused variants were removed.

## Level format

Levels are UTF-8 text and diff cleanly in git. A short example:

```text
ink-level 1
wall 1 40.000 40.000 520.000 40.000
wall 2 520.000 40.000 520.000 640.000
room 1 4 100.000 120.000 220.000 120.000 220.000 260.000 100.000 260.000
door 0 1 0.5000 44.000
label 120.000 300.000 19.000 West Storage Room
stairs 420.000 110.000 0 10
```

Records are `wall`, `room`, `door`, `label`, and `stairs`. Walls carry their ids so door hosts survive a save and load. Load parses into a temporary level and leaves the open level untouched on any error.

## Project layout

| Path | Owns |
| --- | --- |
| `src/main.c` | sokol app lifecycle, font, frame order |
| `src/editor.c` | ImGui theme, panels, canvas, gestures, camera, modals |
| `src/level.c` | level document, edits, save, load, undo ring |
| `src/geom.c` | vectors, snapping, segment and polygon math |
| `tests/test_level.c` | 163 headless checks for the core |
| `tools/verify.sh` | end-to-end X11 check with pixel evidence |
| `tools/shot.sh` | build, launch, drive, screenshot |
| `tools/gen_demo_map.py` | regenerate the demo level |
| `tools/level_info.c` | print counts and bounds for a level file |

Only `main.c` and `editor.c` include sokol or ImGui headers. `make test` links the core with gcc alone, so a core file that pulls in a UI header fails the build.

## Verification

```sh
make test
make verify
```

`make verify` runs `tools/verify.sh`, which rebuilds, runs the core tests, drives the real X11 window with xdotool, samples canvas pixels, and checks the saved file. A recorded run at 1280x800:

```text
PASS build without warnings
PASS prototype settings removed
PASS core tests (163 checks)
PASS demo fills blue=28197 red=25854 white=440
PASS closed loop added a fill (+9296 blue px)
PASS delete removed the fill
PASS undo restored the fill
PASS click placement added a white door (+52 white px)
PASS label typed and deactivated
PASS save wrote 46 walls, 11 doors, a grid label, and a grid door
PASS reload restored the level (blue=37899)
PASS marquee selected all and shift-click preserved the selected walls, rooms, and doors
PASS undo restored the level after marquee delete
PASS group drag moved both labels onto the grid
PASS shift selection deleted both labels in one step
PASS l-shaped close created a fill (+6884 blue px)
PASS l-shaped close saved 4 new walls and a 4 point room
ALL EDITOR CHECKS PASSED
```

The script assumes a 1280x800 window and the default panel widths. Screenshots land in `/tmp/ink-ribbon-verify-*.png`.

## Limits

- Walls are horizontal or vertical only. Walls can be selected and deleted, but their endpoints do not drag. Deleting a wall cascades its doors.
- Painted areas are independent polygons and do not follow wall edits.
- Fixed capacities are 512 walls, 64 areas, 256 doors, 128 labels, and 64 stair runs. A capacity failure is reported in the status strip.
- One level is open at a time. New and Open do not ask before discarding unsaved changes. There is no copy and paste and no native file dialog.
- Group drag moves labels, stairs, and rooms. Walls and doors stay fixed, and a door slides or resizes on its own.
- The editor was tested on Linux and X11 only.

## Third party

- [sokol](https://github.com/floooh/sokol), zlib/libpng license.
- [dcimgui](https://github.com/floooh/dcimgui), Dear ImGui under the MIT license.
