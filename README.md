# Ink Ribbon Level Editor

A native 2D level editor for floor-plan style maps, written in C with sokol and
Dear ImGui. It has one look: white lines on a near-black canvas. Walls and stair
treads draw faint, doors draw white, and nothing is filled. The only tools are
Select, Line, Rectangle, Door, Stairs, and Label.

The editor loads `levels/re2-library.lvl` on startup. That file is the rough
mockup of overlapping rectangles that sets the shape language for the rebuilt
map. The full-scale plan is kept in `levels/re2-library-source.lvl`, and
`tools/gen_demo_map.py` regenerates it from readable coordinates.

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

`make verify` also needs `xdotool`, ImageMagick (for `import`, `convert`, and `compare`), and `Xvfb`. The editor itself does not use them. Verification runs on a private Xvfb display, so it never moves your mouse or touches your desktop. The harness tags every process with a per-run id, claims its own X display with a lock, and records itself in `/tmp/ink-ribbon-runs`. Two runs can go at once on separate displays. Each run kills only its own processes, and the next run reaps the processes and display lock of any run that was killed outright. `make verify` also fails if its own run leaves anything behind.

Build and run from the repository root:

```sh
git submodule update --init --recursive
make
./build/editor
```

Load another level with `./build/editor --level path/to/level.lvl`. The `levels/` path is relative to the working directory.

## What you can do

- Press 1 to 6 for Select, Line, Rectangle, Door, Stairs, and Label. After a drawing action the editor returns to Select. Hold Q while the action completes to keep the tool.
- Drag a rectangle with the Rectangle tool. A rectangle that overlaps or shares an edge with a shape joins it. The outline is rebuilt as one closed loop, so a plus or L shaped room built from several rectangles has no interior lines and no doubled walls. A rectangle that only touches a corner starts a new shape.
- Hover a wall with the Door tool. A white ghost shows the door at the panel width, centred on the point projected onto the wall and snapped along it. Click to place the door. Drag either end with Select to change its length.
- Drag a rectangle with the Stairs tool. The run takes the panel's direction and step count. Flip reverses which end the tread fade starts from, and the flag is saved with the level. Drag any stair edge with Select to resize the footprint.
- Draw walls click by click with the Line tool. Every segment is horizontal or vertical and continues from the last click. Click the first point or press Enter to close the loop. Esc keeps the chain open.
- Add text labels with the Label tool. The new label is selected with its inspector text field focused, so typing renames it. The panel sets the size for the next label and the inspector resizes and moves any label.
- Select, edit, and delete the placed objects. Deleting a shape removes its walls and the doors hosted by them.
- Click to select, shift-click to add, Ctrl-click to remove. Drag a box to select whole labels, stairs, and doors and to mark the crossed part of every wall it passes over. Delete cuts all the marked sections in one step and still removes the selected objects. Dragging a wall edge reshapes its shape, dragging a wall corner resizes the lines that meet there, and dragging labels or stairs moves them together.
- Snap to the grid on placement, dragging, and reshaping. The drawn grid follows the selected Grid size, so every line is a position objects can land on. The grid sizes are 4, 8, 16, and 32 world units, and the Snap checkbox turns snapping off. The Rectangle tool always snaps to the grid because a rectangle is a grid cell.
- Pan, zoom, fit the level, undo and redo, save, and load.

## Controls

| Input | Action |
| --- | --- |
| 1, 2, 3, 4, 5, 6 | Select, Line, Rectangle, Door, Stairs, Label. Select is the default |
| Q (held) | Keep the active drawing tool after an action |
| Left click | Use the active tool |
| Shift plus left click | Add an object to the selection |
| Ctrl plus left click | Toggle an object in the selection |
| Drag a wall edge | Reshape the shape by sliding that edge and stretching its neighbours |
| Drag a wall corner | Resize the walls that meet at that corner |
| Drag a stair edge | Resize the stair footprint on the grid |
| Drag on empty canvas | Marquee select and mark crossed wall sections; Shift adds to the selection |
| Middle drag, or Space plus left drag | Pan |
| Mouse wheel | Zoom around the cursor |
| Enter | Close a line chain |
| Esc | Clear the selection, cancel a draft, or cancel a drag |
| Backspace | Remove the last wall in the current chain |
| Delete | Cut every marked wall section and delete the selected objects |
| F | Fit the level |
| G | Toggle the grid |
| Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z | Undo, redo |
| Ctrl+S, Ctrl+Shift+S | Save, save as |
| Ctrl+O | Open |
| H | Controls dialog |

## Editing model

Walls are independent axis-aligned segments. Corners meet because the Line tool starts each new segment at the previous click, and the Rectangle tool creates its four walls in loop order. A wall has a stable id. Doors store that id, a normalized centre position, and a world-space length, so a door follows its wall and never points at a deleted one.

A shape stores only its wall loop and a closed flag. The Rectangle tool adds a rectangle by splitting the shape and the rectangle on their own x and y coordinates, marking the occupied cells, and rebuilding the boundary from the cell edges. Consecutive edges merge into one wall, so the union is one closed loop. The operation replaces the shape's walls and keeps the shape id. A door whose wall survives moves to the matching new wall at the same world centre. A door whose wall becomes interior has no line left and is dropped. The union is stored as walls, so it survives save and load without new records.

Labels store a position, a size, and a string. Stairs store a min-corner box, a direction, a step count, and which end of the run the fade starts from. `STAIR_STEP_DEPTH` and `STAIR_WIDTH` fix the tread size. The stair flag is the optional last field of the stair record.

Selection is UI state and is never saved into a level. It is a bounded set per object kind plus a primary object. Shift-click adds an object, Ctrl-click toggles it, a box replaces or adds to the set, and Delete removes every selected object with one undo step. A box also marks the crossed part of each wall it passes over as a section, and Delete cuts every marked section in the same step. Dragging a wall edge reshapes its shape, and dragging a wall corner moves that corner and every shared vertex that must follow. Selected labels and stairs move as a group when dragged. A door stores its host wall id and a normalized position, so a door travels with its wall and cannot leave it. A door dragged alone still slides along its wall, and a door end still resizes it.

Snap applies to line points, labels, stair boxes and edges, door centres, door resize spans, reshapes, and object drags. A group drag snaps from the primary object, so the relative spacing of the group stays intact. Snap is on by default. The grid sizes are 4, 8, 16, and 32 world units.

Undo stores whole level snapshots. Each completed change writes one snapshot. A wall click, a drag, and a property edit each undo as their own step. The history keeps 64 levels.

## One look

The canvas is near-black. Every wall, stair tread, and selection helper is a line. Walls draw at a fixed faint alpha. Doors are always white. Stairs are neutral treads with no footprint fill. Labels are light grey. The editor has no fill palette, no room colour, and no shape fill mode. Loading a level that still carries fill, room, or door colour columns succeeds: the fill and colour values are ignored and the room polygons are skipped. Saving writes the fill and door colour columns as 0 for older readers and writes no room records.

## Verification

```sh
make test
make verify
```

`make verify` rebuilds, runs the core tests, loads a legacy level with a fill, a room polygon, and a coloured door, then drives the real X11 window with xdotool. It checks that the fill is ignored, the wall is faint, the door is white, and the six tool buttons activate in order. It draws two overlapping rectangles and checks from the saved file that one shape holds exactly the eight boundary walls with no interior lines, then reloads that file and compares every wall record. It hovers a wall and checks the ghost and the placed white door, then proves that the tool returned to Select and that Q held keeps it. It places a stair, checks the saved flip flag, and toggles the flag from the inspector. A recorded run:

```text
PASS build without warnings
PASS core tests (338 checks)
PASS empty room (040404), faint wall (max 0.340974), white door (max 0.94902)
PASS save writes fill 0, door colour 0, and no room record
PASS six tools activate in order on keys 1 to 6
PASS key 7 is not a tool
PASS the tool returned to Select after the rectangle action
PASS two overlapping rectangles are one eight-wall shape with no interior lines
PASS level structure, doors, stairs, and labels
PASS the union round-trips through save and load
PASS hover ghost, click placed a white 32-unit door at the projected point
PASS the tool returned to Select after placing the door
PASS Q lock lights, holds the Door tool, and the next unheld action switches
PASS a stair drag uses the panel direction, steps, and flip
PASS the tool returned to Select after the stair action
PASS the stair flip flag persists in the level
PASS focus loss survived (rss=104296 KB)
PASS no leftover processes
ALL SIX-TOOL CHECKS PASSED
```

The script assumes a 1280x800 window and the default panel widths, and it uses its own Xvfb display. Screenshots land in `/tmp/ink-ribbon-six-*.png`.

## Project layout

| Path | Owns |
| --- | --- |
| `src/main.c` | sokol app lifecycle, font, frame order |
| `src/editor.c` | ImGui theme, panels, canvas, gestures, camera, modals |
| `src/level.c` | level document, shapes, rectangle union, edits, save, load, undo ring |
| `src/geom.c` | vectors, snapping, segment and polygon math |
| `tests/test_level.c` | headless checks for the core |
| `tools/verify.sh` | end-to-end X11 check with pixel and coordinate evidence |
| `tools/shot.sh` | build, launch, drive, screenshot |
| `tools/xvfb.sh` | private X display, per-run process tags, cleanup |
| `tools/focus_drop.py` | drop X focus the way switching windows does |
| `tools/gen_demo_map.py` | regenerate the demo level |
| `tools/level_info.c` | print counts and bounds for a level file |

Only `main.c` and `editor.c` include sokol or ImGui headers. `make test` links the core with gcc alone, so a core file that pulls in a UI header fails the build.

## Limits

- Walls are horizontal or vertical only, and an edge slides only along its normal, so a wall cannot be dragged diagonally or turned into a diagonal. Deleting an edge cascades its doors and reopens the shape if the loop breaks.
- The union rebuilds a shape from its current outline, not from the rectangles that formed it.
- A union drops a door whose wall becomes interior. The status strip reports how many doors were dropped.
- Fixed capacities are 512 shapes, 512 walls, 256 doors, 128 labels, and 64 stair runs. A capacity failure is reported in the status strip.
- One level is open at a time. New and Open do not ask before discarding unsaved changes. There is no copy and paste and no native file dialog.
- Group drag moves walls, labels, and stairs. Doors follow their host walls, so a selected door whose wall is not selected stays put. A single selected door slides along its wall instead.
- The editor was tested on Linux and X11 only.

## Third party

- [sokol](https://github.com/floooh/sokol), zlib/libpng license.
- [dcimgui](https://github.com/floooh/dcimgui), Dear ImGui under the MIT license.
