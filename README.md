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

`make verify` also needs `xdotool`, ImageMagick (for `import`, `convert`, and `compare`), and `Xvfb`. The editor itself does not use them. Verification runs on a private Xvfb display, so it never moves your mouse or touches your desktop. The harness tags every process with a per-run id, such as `ink-ribbon-editor-4192-7731`, claims its own X display with a lock, and records itself in `/tmp/ink-ribbon-runs`. Two runs can therefore go at once on separate displays. Each run kills only its own processes on exit or SIGTERM, and the next run reaps the processes and display lock of any run that was killed outright. `make verify` also fails if its own run leaves anything behind.

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
- Click to select, shift-click to add, Ctrl-click to remove, or drag from empty canvas to marquee select. Delete removes the whole selection in one step, and dragging anything selected moves the group together.
- Snap to the grid on placement and movement. The drawn grid follows the selected Grid size, so every line is a position objects can land on. The grid sizes are 4, 8, 16, and 32 world units, and the Snap checkbox turns snapping off.
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

Selection is UI state and is never saved into a level. It is a bounded set per object kind plus a primary object. Shift-click adds an object, Ctrl-click toggles it, a marquee replaces or adds to the set, and Delete removes every selected object with one undo step. Dragging a selected wall moves the wall, or the whole group when it is part of a multi-selection. Selected rooms, labels, and stairs move with it. A door stores its host wall id and a normalized position, so a door travels with its wall and cannot leave it. A door dragged alone still slides along its wall, and a door end still resizes it.

Snap-to-grid applies to wall and room points, labels, stairs, door centers, door resize spans, and object drags. A group drag snaps from the primary object, so the relative spacing of the group stays intact. Snap is on by default. The grid sizes are 4, 8, 16, and 32 world units.

Undo stores whole level snapshots. Each completed change writes one snapshot. A wall click, a drag, and a property edit each undo as their own step. Closing a loop writes its closing walls and the fill together. The history keeps 64 levels.

## Selection and snapping

![Marquee selection in progress](docs/screenshots/marquee.png)

A plain click on an unselected object selects only it. Clicking an object already in a multi-selection keeps the group and makes it primary, so the group can be dragged. Shift-click adds an object and never removes one. Ctrl-click toggles it. Dragging from empty canvas draws a marquee, and Shift makes the marquee additive. The inspector shows per-kind counts for a multi-selection, and Delete removes the whole set in one undo step.

![Two selected labels](docs/screenshots/selection.png)

![The whole level dragged as one group](docs/screenshots/group-drag.png)

Marquee select everything, then drag any selected wall. The walls, their doors, the fills, the labels, and the stairs move by one grid-snapped delta and commit as one undo step.

Dragging moves the whole group, walls and their doors included. A door has no independent position, so it follows its host wall; selecting a door without its wall leaves it in place and the status line says so. A door dragged alone slides along its wall, and dragging an end handle resizes it. Snap uses the grid size set in the left panel, and the group snaps from the primary object's anchor so the rest keeps its offset.

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
| `tests/test_level.c` | 171 headless checks for the core |
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
PASS core tests (171 checks)
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
PASS group drag moved walls and their doors with the rest of the selection
PASS shift selection deleted both labels in one step
PASS dragged wall moved onto grid coordinates
PASS l-shaped close created a fill (+6884 blue px)
PASS l-shaped close saved 4 new walls and a 4 point room
ALL EDITOR CHECKS PASSED
```

The script assumes a 1280x800 window and the default panel widths, and it uses its own Xvfb display. Screenshots land in `/tmp/ink-ribbon-verify-*.png`.

## Focus and the pointer

ImGui reports the pointer position as a sentinel when the window has none, which is what happens when you alt+tab away. The editor treats that as "no pointer": it draws no preview, ends any drag at its last applied position, and leaves an open wall chain alone. When focus returns and you move the mouse, the chain preview comes back and drawing continues.

Before this was handled, leaving the window mid-chain made the preview build a dashed line of astronomically large length, which pinned a core and grew memory until the machine froze. `make verify` now covers that case.

## Limits

- Walls are horizontal or vertical only. Walls can be selected and deleted, but their endpoints do not drag. Deleting a wall cascades its doors.
- Painted areas are independent polygons and do not follow wall edits.
- Fixed capacities are 512 walls, 64 areas, 256 doors, 128 labels, and 64 stair runs. A capacity failure is reported in the status strip.
- One level is open at a time. New and Open do not ask before discarding unsaved changes. There is no copy and paste and no native file dialog.
- Group drag moves walls, rooms, labels, and stairs. Doors follow their host walls, so a selected door whose wall is not selected stays put. A single selected door slides along its wall instead.
- The editor was tested on Linux and X11 only.

## Third party

- [sokol](https://github.com/floooh/sokol), zlib/libpng license.
- [dcimgui](https://github.com/floooh/dcimgui), Dear ImGui under the MIT license.
