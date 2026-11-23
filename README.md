# Ink Ribbon Level Editor

A native 2D level editor for floor-plan style maps, written in C with sokol and
Dear ImGui. It has one look: white lines on a near-black canvas. Walls and stair
treads draw faint, doors draw white, and nothing is filled. The only tools are
Select, Line, Rectangle, Door, Stairs, and Label.

The editor loads `levels/re2-library.lvl` (3F) on startup. That file is rebuilt
from the reference floor plan and matches it one reference pixel per world
unit. The plan is kept in `levels/re2-library-source.lvl`; `tools/gen_ref_map.py`
regenerates it from readable coordinates, and `tools/build_map.py` draws it in
the running editor through the real tools. The 1F and 2F plans live beside it in
`levels/re2-library-1f.lvl` and `levels/re2-library-2f.lvl`, generated from the
same reference-pixel coordinates by `tools/gen_floor_maps.py`. All three share
one world coordinate system, so a stairwell occupies the same rectangle on
every floor. Play mode adds a single gold player
arrow, an 8-unit A* nav grid, click-to-walk control, and a following camera. `docs/screenshots/rebuilt-overlay.png`
shows the finished level blended over the reference, and
`docs/screenshots/rebuilt-side-by-side.png` shows the same pair side by side.

## Build and run

The project is tested on Linux with X11 and OpenGL. It uses [fibs](https://github.com/floooh/fibs), Floh's Typescript cmake wrapper. It needs Deno, CMake, a C/C++ toolchain, and the X11, Xi, Xcursor, and GL development packages. Ninja is recommended.

Install Deno from https://docs.deno.com/runtime/getting_started/installation/

Fedora:

```sh
sudo dnf install gcc gcc-c++ cmake ninja-build libX11-devel libXi-devel libXcursor-devel mesa-libGL-devel
```

Debian and Ubuntu:

```sh
sudo apt install gcc g++ cmake ninja-build libx11-dev libxi-dev libxcursor-dev libgl1-mesa-dev
```

`./fibs verify` also needs `xdotool`, ImageMagick (for `import`, `convert`, and `compare`), and `Xvfb`. The editor itself does not use them. Verification runs on a private Xvfb display, so it never moves your mouse or touches your desktop. The harness tags every process with a per-run id, claims its own X display with a lock, and records itself in `/tmp/ink-ribbon-runs`. Two runs can go at once on separate displays. Each run kills only its own processes, and the next run reaps the processes and display lock of any run that was killed outright. `./fibs verify` also fails if its own run leaves anything behind.

Build and run from the repository root:

```sh
git submodule update --init --recursive
./fibs build
./fibs editor
```

`./fibs editor` builds the editor, then runs it from the project directory, so the relative `levels/` paths work. Load another level with `./fibs editor --level path/to/level.lvl`.

Run the headless core tests with:

```sh
./fibs test
```

The built executables live in `.fibs/dist/<config>/`; `./fibs config --get` prints the active config. Use `./fibs config linux-ninja-debug` (or `linux-make-debug`) for a debug build and `./fibs build --rebuild` for a clean rebuild. `./fibs clean --all` removes build output.

## What you can do

- Press 1 to 6 for Select, Line, Rectangle, Door, Stairs, and Label. After a drawing action the editor returns to Select. Hold Q while the action completes to keep the tool.
- Drag a rectangle with the Rectangle tool. A rectangle that overlaps or shares an edge with a shape joins it. The outline is rebuilt as one closed loop, so a plus or L shaped room built from several rectangles has no interior lines and no doubled walls. A rectangle that only touches a corner starts a new shape, and a rectangle drawn fully inside another stays as a separate nested outline.
- Hover a wall with the Door tool. A white ghost shows the door at the panel width, centred on the point projected onto the wall and snapped along it. Click to place the door. Drag either end with Select to change its length.
- Drag a rectangle with the Stairs tool. The run takes the panel's direction, step count, falloff, and faint/bright opacities. Flip reverses which end the tread fade starts from, and all of it is saved with the level. Drag any stair edge with Select to resize the footprint.
- Draw walls click by click with the Line tool. Every segment is horizontal or vertical and continues from the last click. Click the first point or press Enter to close the loop. Esc keeps the chain open.
- Add text labels with the Label tool. The new label is selected with its inspector text field focused, so typing renames it. The panel sets the size for the next label and the inspector resizes and moves any label.
- Press M (or tick "Marquee next drag" in the Select panel), then drag anywhere to start a marquee, including from inside a closed shape. A drag on empty canvas still starts one without arming. The marquee marks the crossed wall sections for cutting.
- Select, edit, duplicate, and delete the placed objects. Deleting a shape removes its walls and the doors hosted by them. Ctrl+D duplicates the selection one grid cell diagonally and keeps every setting, including door width/position and each stair's direction, steps, flip, falloff, and opacities; doors on duplicated walls come along.
- Lock the selected objects with L or the inspector checkbox. Locking a wall also locks the doors it hosts. Locked objects stay put while you select and edit other objects. Selecting a locked object and pressing L (or unticking the checkbox) unlocks it. A selection that contains any locked object cannot be moved, reshaped, deleted, duplicated, or edited.
- Click to select, shift-click to add, Ctrl-click to remove. Drag a box to select whole labels, stairs, and doors and to mark the crossed part of every wall it passes over. Delete cuts all the marked sections in one step and still removes the selected objects. Dragging a wall edge reshapes its shape, dragging a wall corner resizes the lines that meet there, and dragging labels or stairs moves them together.
- Snap to the grid on placement, dragging, and reshaping. The drawn grid follows the selected Grid size, so every line is a position objects can land on. The grid sizes are 4, 8, 16, and 32 world units, and the Snap checkbox turns snapping off. The Rectangle tool always snaps to the grid because a rectangle is a grid cell.
- Snap dragged lines, edges, corners, and every side of a moving selection to nearby wall lines and endpoints with the Snap objects checkbox. Object and grid snapping are independent, and an object target wins on the axis it lands on.
- In edit mode, press **Place player** in the Player section, then click a walkable nav cell. You can also press P for play mode and click the map. At most one player exists. Once placed, each play-mode click queues a waypoint at that cell. Green markers and the planned route show the sequence. The route is plain A* through the 8-unit grid cells, with no curve smoothing. **double-click** executes it, walking cell to cell through the waypoints in order and removing each marker as it is reached, then ending on the last cell centre. The player rotates toward the next cell and follows rooms and doors. Esc clears the queue.
- While walking, the camera follows the player at the current zoom. C recentres on the player without changing zoom. Middle-drag pans immediately, with no catch-up animation, and mouse-wheel zoom stays immediate.
- Pan, zoom, fit the level, undo and redo, save, and load.
- Duplicate with Ctrl+D and lock a selection with L.

## Floors

The reference building is three floors. The 3F document is shown by default,
and a vertical **3F / 2F / 1F** selector sits at the right edge of the canvas,
matching the in-game map screen. Click a tab to swap the active floor in edit or
play mode. The camera position and zoom level are kept, so the south-east
stairwell stays over the same parchment position while you compare floors.
Returning to the player's floor keeps that pan too; the camera recentres only
when the player starts moving or `C` is pressed. Changing the viewed floor does
not stop a walk: the player keeps advancing on the floor that owns it while
another floor is shown, and the new position appears when you return. With
several floors loaded,
**F** fits their union, so every floor is drawn at the same scale and the
selector never covers a wing.

Each floor is a complete `Level` with its own undo history, so undo never
crosses a floor. Ctrl+S writes every loaded floor back to its file; Save As
writes the active floor. A floor set is detected at startup when the two
companion files exist beside `levels/re2-library.lvl`; any other `--level` path
keeps the original single-level behavior.

The player belongs to exactly one floor. In the shipped files the marker starts
in the 1F Entrance; 2F and 3F carry none until the player is placed on them.
Clicking a floor tab only changes the view: the marker is drawn on the floor that owns it and is absent everywhere
else, and the camera fits the viewed floor when the player is elsewhere.
Walking on a stair run is ordinary movement and never changes floors: the
player stays in the document that owns the marker, whether the route crosses
the west stairwell, the main-hall flights, the library stair, or the east
stair. **Place player** in edit mode moves ownership to the current floor.

Reference linework that should draw without blocking movement is stored as a
detail wall: an optional seventh field `1` on a `wall` record. The 1F balcony
rails, counters and shelving outlines use it, so the plan keeps the reference's
detail while the nav grid only sees real walls. Detail walls render like walls
and are editable; they are simply skipped by collision and A*.

Level files may carry navigation-only openings as a sixth door field:
`door 0 <wall> <t> <width> 1`. The reference has archways and open room passages
that are not white bars; they are stored so the 8-unit nav grid can pass through
them, but the editor does not draw or select them. Visible doors omit the field
or write `0`. Older files with five fields still load.

## Controls

| Input | Action |
| --- | --- |
| 1, 2, 3, 4, 5, 6 | Select, Line, Rectangle, Door, Stairs, Label. Select is the default |
| Q | Toggle keeping the active drawing tool after an action |
| Left click | Use the active tool |
| Click a 3F / 2F / 1F tab | Switch floors in edit or play mode |
| Shift plus left click | Add an object to the selection |
| Ctrl plus left click | Toggle an object in the selection |
| Drag a wall edge | Reshape the shape by sliding that edge and stretching its neighbours |
| Drag a wall corner | Resize the walls that meet at that corner |
| Drag a stair edge | Resize the stair footprint on the grid |
| Drag on empty canvas | Marquee select and mark crossed wall sections; Shift adds to the selection |
| M | Arm the next marquee so it can start inside a closed shape |
| P | Toggle play mode; click to queue waypoints, double-click to walk |
| C | Recentre the camera on the player at the current zoom |
| Esc | In play mode, stop the player and clear queued waypoints |
| Ctrl+S | Save, including in play mode; it does not toggle Snap |
| Drag a section end | Resize a marked section along its wall |
| Middle drag, or Space plus left drag | Pan |
| Mouse wheel | Zoom around the cursor |
| Enter | Close a line chain |
| Esc | Clear the selection, cancel a draft, or cancel a drag |
| Backspace | Remove the last wall in the current chain |
| Delete | Cut every marked wall section and delete the selected objects |
| F | Fit the level |
| G | Toggle the grid |
| S | Toggle snapping |
| L | Lock or unlock the current selection |
| Ctrl+D | Duplicate the selection one grid cell diagonally |
| Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z | Undo, redo |
| Ctrl+S, Ctrl+Shift+S | Save, save as |
| Ctrl+O | Open |
| Ctrl+0 | Reset the zoom to 100 percent |
| H | Controls dialog |

## Editing model

Walls are independent axis-aligned segments. Corners meet because the Line tool starts each new segment at the previous click, and the Rectangle tool creates its four walls in loop order. A wall has a stable id. Doors store that id, a normalized centre position, and a world-space length, so a door follows its wall and never points at a deleted one.

A shape stores only its wall loop and a closed flag. The Rectangle tool adds a rectangle by splitting the shape and the rectangle on their own x and y coordinates, marking the occupied cells, and rebuilding the boundary from the cell edges. Consecutive edges merge into one wall, so the union is one closed loop. The operation replaces the shape's walls and keeps the shape id. A door whose wall survives moves to the matching new wall at the same world centre. A door whose wall becomes interior has no line left and is dropped. The union is stored as walls, so it survives save and load without new records.

Labels store a position, a size, and a string. Stairs store a min-corner box, a direction, a step count, which end of the run the fade starts from, a falloff curve (linear, ease in, ease out, or smooth), and the opacity at the faint and bright ends. `STAIR_STEP_DEPTH` and `STAIR_WIDTH` fix the tread spacing. The stair bottom flag, falloff, and opacities are optional trailing fields, so older stair records still load with the linear 0.18/0.68 default.

Selection is UI state and is never saved into a level. It is a bounded set per object kind plus a primary object. Shift-click adds an object, Ctrl-click toggles it, a box replaces or adds to the set, and Delete removes every selected object with one undo step. A box also marks the crossed part of each wall it passes over as a section, and Delete cuts every marked section in the same step. Dragging a wall edge reshapes its shape, and dragging a wall corner moves that corner and every shared vertex that must follow. Selected labels and stairs move as a group when dragged. A door stores its host wall id and a normalized position, so a door travels with its wall and cannot leave it. A door dragged alone still slides along its wall, and a door end still resizes it.

Snap applies to line points, labels, stair boxes and edges, door centres, door resize spans, reshapes, and object drags. A group drag snaps from the primary object, so the relative spacing of the group stays intact. Snap is on by default. The grid sizes are 4, 8, 16, and 32 world units.

Ctrl+D duplicates every selected object with its full settings and copies doors hosted by duplicated walls. A door selected without its wall gets a copy of the host wall under it. Locks live on the objects for the session: they are not saved with the level, but they survive selecting other objects. Locked objects remain selectable so they can be unlocked; any edit or delete involving a locked object is refused.

Snap objects is a second, independent toggle. While it is on, a dragged edge or corner also snaps to nearby wall lines and wall endpoints within 8 screen pixels. A rigid object drag checks every moving edge and wall endpoint on each axis, so a selection can land by its right or bottom side as well as its top or left. Object snapping wins on an axis and grid snapping fills the other. It is on by default.

Undo stores whole level snapshots. Each completed change writes one snapshot. A wall click, a drag, and a property edit each undo as their own step. The history keeps 64 levels.

## One look

The canvas is near-black. Every wall, stair tread, and selection helper is a line. Walls draw at a fixed faint alpha. Doors are always white 3 px bars. Stairs are neutral 3 px treads whose opacity follows the per-stair falloff and end opacities, with no footprint fill. Labels are light grey. The editor has no fill palette, no room colour, and no shape fill mode. Loading a level that still carries fill, room, or door colour columns succeeds: the fill and colour values are ignored and the room polygons are skipped. Saving writes the fill and door colour columns as 0 for older readers and writes no room records.

## Player and navigation

Play mode is a runtime view, not a seventh drawing tool. The player is one optional
level record (`player x y heading`), so a map holds at most one marker. In edit
mode, use **Place player** in the Player section and click a walkable cell; the
same click also works in play mode when no player exists. The nav
grid uses 8-unit cells over the level bounds. A cell is walkable when its centre
is inside a closed room/corridor outline and its player radius clears every solid
wall. Door spans are cut out of every collinear wall they sit on, so doors are
passable; stair treads are not walls, while stair-side rails and walls still
block. Stair ends stay open so a run can be climbed, and a rectangle/line
segment that would cut through a rail or solid wall is rejected. Stairs are
drawn and walked over like any other open floor; they do not switch levels. A*
uses 8-way movement, forbids corner cutting, and paths through door gaps. The route is the raw A* cell chain. No spline, no corner rounding and no
line-of-sight shortcut changes it, so the green line follows the grid with
right-angle steps. The player rotates on the spot to face the first cell, then
moves from cell centre to cell centre while turning toward each next cell. There
is no turning-circle arc and no turning ring.

The camera follows the player while walking but only changes its centre: zoom is
never altered by play mode, recentring, or path starts. When the player starts
moving with the camera offset, or when `C` is pressed, the camera makes a smooth
pan to the player at the current zoom. Middle-drag and wheel zoom apply
immediately, with no catch-up animation; middle-drag disables follow until the
next move or `C`.

The player graphic is authored in `assets/player.svg`, rasterised with Krita to
`assets/player.png`, and packed by `tools/gen_player_sprite.py` into
`src/player_sprite.h` so the editor needs no image decoder at runtime.

## Verification

```sh
./fibs test
./fibs verify
```

`./fibs verify` rebuilds, runs the core tests, loads a legacy level with a fill, a room polygon, and a coloured door, then drives the real X11 window with xdotool. It checks that the fill is ignored, the wall is faint, the door is white, and the six tool buttons activate in order. It draws two overlapping rectangles and checks from the saved file that one shape holds exactly the eight boundary walls with no interior lines, then reloads that file and compares every wall record. It hovers a wall and checks the ghost and the placed white door, then proves that the tool returned to Select and that Q held keeps it. It places a stair, checks the saved flip flag, and toggles the flag from the inspector. A recorded run:

```text
PASS build without warnings
PASS core tests (396 checks)
PASS empty room (040404), faint wall (max 0.340974), white door (max 0.94902)
PASS save writes fill 0, door colour 0, and no room record
PASS six tools activate in order on keys 1 to 6
PASS key 7 is not a tool
PASS the tool returned to Select after the rectangle action
PASS two overlapping rectangles are one eight-wall shape with no interior lines
PASS level structure, doors, stairs, and labels
PASS the union round-trips through save and load
PASS hover ghost, click placed a white 10-unit door at the projected point
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
| `fibs`, `fibs.ts` | fibs launcher and build graph |
| `src/nav.c`, `nav.h` | 8-unit nav grid, door-aware wall clearance, A* |
| `src/player_sprite.h` | RGBA player sprite embedded from the Krita export |
| `assets/player.svg`, `assets/player.png` | player design and Krita raster export |
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
| `tools/gen_floor_maps.py` | generate the 1F and 2F floor plans from reference pixels |
| `tools/gen_player_sprite.py` | repack the Krita PNG into the C sprite header |
| `tools/level_info.c` | print counts and bounds for a level file |

Only `main.c` and `editor.c` include sokol or ImGui headers. The `test_level` target in `fibs.ts` depends only on `ink-core`, so a core file that pulls in a UI header fails the build.

## Limits

- Walls are horizontal or vertical only, and an edge slides only along its normal, so a wall cannot be dragged diagonally or turned into a diagonal. Deleting an edge cascades its doors and reopens the shape if the loop breaks.
- The union rebuilds a shape from its current outline, not from the rectangles that formed it.
- A union drops a door whose wall becomes interior. The status strip reports how many doors were dropped.
- Fixed capacities are 512 shapes, 512 walls, 256 doors, 128 labels, and 64 stair runs. A capacity failure is reported in the status strip.
- One floor set is open at a time. New and Open collapse back to a single level and do not ask before discarding unsaved changes. There is no copy and paste and no native file dialog.
- Group drag moves walls, labels, and stairs. Doors follow their host walls, so a selected door whose wall is not selected stays put. A single selected door slides along its wall instead.
- Locks are session-only. They are not written to the level file and are cleared by opening or creating a level.
- The editor was tested on Linux and X11 only.

## Third party

- [sokol](https://github.com/floooh/sokol), zlib/libpng license.
- [dcimgui](https://github.com/floooh/dcimgui), Dear ImGui under the MIT license.
