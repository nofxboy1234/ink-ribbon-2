# Editor UI and interaction spec

This is the contract for `src/editor.c`, `src/editor.h`, and the editor hook in `src/main.c`. The level document and all mutations come from `src/level.h`; do not add another writer.

## Window layout

One sokol window titled `Ink Ribbon Level Editor`, 1280 by 800 initial size, near-black clear color.

The frame builds a full-viewport ImGui layout:

- A main menu bar at the top: File, Edit, View, Help.
- A left tool panel, 190 px wide, from below the menu bar to the bottom. It shows the tool buttons, then Grid, Snap, and Show grid, then only the options the active tool uses.
- A right inspector panel, 300 px wide, same vertical span.
- A canvas filling the middle. Canvas rect is stored in the editor so the mouse can be converted between screen and world.
- A one-line status strip overlaid at the bottom of the canvas. It shows the active tool hint, cursor world coordinates, zoom, the current snap size or `snap off`, and the latest action or error.

Panels use `ImGuiWindowFlags_NoTitleBar | NoResize | NoMove | NoCollapse | NoScrollbar`. Positions and sizes come from `igGetIO()->DisplaySize` and the menu bar height each frame.

## Visual language

Match the reference. Near-black canvas `#0b0c0e`. Grid lines at the selected Grid size, subtle at `#333b45` with low alpha, a brighter line every 4 cells and the brightest every 16. The grid gets coarser only when the fine lines would be closer than 4 screen pixels. Walls `#8b9097`, 2 screen pixels wide. Fill blue `#2b3f50`, fill red `#4a2028`, both opaque. A closed wall shape can also use None, which draws no fill. Fills are ear-clipped into triangles, so a shape fills its own outline and never its bounding rectangle. Doors white `#f2f2f2`, red `#b3323b`, blue `#3d7fb5`. Labels `#e6e6e6`. Stairs `#767d85` treads on a slightly lighter footprint fill. Selection accent `#5aa0c8` with a 2 px outline or handles.

Dark ImGui theme: window background `#101114`, panel `#16181c`, frame `#1d2025`, borders `#2a2e34`, text `#d9dadd`, accent `#4e7f9e`. Rounded corners 3 px. Sentence-case headings.

The embedded scalable ImGui default font at 15 px is fine. Do not depend on system font files.

## Canvas transform and navigation

Camera stores a world-space center and a zoom in screen pixels per world unit. World y grows down. `screen_to_world` and `world_to_screen` are the only transforms.

ImGui clears the pointer position to `-FLT_MAX` when the window loses focus. Never feed that value into `screen_to_world` or into a preview. Check `mouse_available` first, draw no preview without a pointer, and release any drag at its last applied position.

- Mouse wheel zooms around the cursor, clamped 0.15 to 6.
- Middle-drag pans. Space plus left-drag pans.
- `F` or View > Fit frames the level bounds with margin. If the level is empty, center 0,0 at zoom 1.
- On startup, load `levels/re2-library.lvl` if it exists, then fit.

## Tools and gestures

Tools: Select, Line, Label, Room. Number keys 1, 2, 3, 4. Line is the default. Switching tools ends any open chain without committing it.

### Line

Click sets points. Each click appends an orthogonal segment that continues from the last click.

- The first click starts a chain at the grid-snapped point.
- Each later click uses `snap_ortho(chain_last, cursor_world, grid)` so a segment is horizontal when `|dx| > |dy|`, vertical otherwise, and the free coordinate snaps to the grid.
- The first click creates a shape with `level_add_shape`. Every later click calls `level_add_wall_to_shape`.
- Clicking within 12 screen pixels of the first point closes the chain: add the final wall back to the first point, call `level_close_shape`, then `level_set_shape_fill` with the current Fill palette choice. End the chain. A close needs at least four walls; report that when it fails.
- `Enter` also closes the chain when it has at least three points. `Esc` ends it open, keeping only the walls already added.
- `Backspace` removes the last wall added in the current chain and moves the chain tip back. That is a live document mutation; commit history after it.
- A plain drag does nothing. `Shift` plus drag starts a marquee and clips every crossed wall to the dragged box.
- Show a preview segment from the last point to the constrained cursor, the first-point marker with a "close" highlight when near, and small vertex dots.
- Chain points are bounded by the wall pool and the shape pool; a failed add reports why in the status strip.

### Line context menu

Right-click in Line mode opens a menu built from the last marquee or the click:

- Door appears when the marquee clipped at least one wall. White, Red, and Blue add a door to each covered wall span. A span already contained in a longer span is skipped, so a painted fill border drawn over the same line does not add a second door.
- Mode appears inside a closed shape when the marquee selected no wall. Blue, Red, Floor Blue, Floor Red, Hole, and Empty set the shape fill and become the current palette choice. Fills are ear-clipped to the shape's own loop.
- Stairs appears when the marquee box is at least one grid cell wide and tall, even when it touches a wall. The drag's dominant axis picks the direction, the run is divided by `STAIR_STEP_DEPTH`, and the lower edge is the bottom of the run.
- Clear selection empties the marquee and the object selection.

### Room

Drag an axis-aligned rectangle on the grid. On release, `create_rect_room` adds a closed shape whose four walls are the rectangle and sets the current Fill palette choice. A rectangle needs at least one grid cell on each axis. The Fill palette paints Blue and Red in creation order, Floor Blue in an underlay pass, Floor Red in creation order, Hole in the canvas colour, and Empty as no fill with a faint outline.

Alt+drag a closed shape starts `GESTURE_ROOM_COPY` with that shape's polygon; the release creates the same polygon translated by the drag delta. A copy keeps the source shape's fill. Right-click any closed shape and pick Mode to change it after creation.

### Label

Click places a label at the snapped point with the left panel's label size, selects it, and focuses the inspector text field so typing immediately renames it. `Enter` commits the text. The inspector also edits size and position.

### Select

Click hit-tests in this order: door, label, stairs, wall, room. Tolerance is 7 screen pixels for walls and doors. Labels use their measured text rect. Stairs use `level_stairs_rect`. Rooms use `level_room_at`.

A wall hit selects that edge. Clicking inside a closed shape selects all of its edges. Dragging a selected wall starts a reshape: a horizontal wall slides in y, a vertical one in x, the drag snaps to the grid, and `level_slide_edges` carries every coincident endpoint so connected lines stretch and opposite lines stay. The gesture commits one snapshot on release and restores the previous snapshot on Esc.

Selection is a bounded set per object kind plus a primary reference. Walls are selected as individual edges. The set is never serialized.

- Plain click on an unselected object replaces the selection. Plain click on a selected object inside a multi-selection keeps the group and makes that object primary.
- Shift-click adds the hit object and never removes one. Ctrl-click toggles it in or out. Neither starts a drag.
- Plain press on empty canvas clears the selection and starts a marquee. Shift-press starts an additive marquee. Travel under 4 screen pixels is a click.
- On marquee release, select every object intersecting the rectangle. Walls and doors use segment intersection, labels and stairs use rect overlap, rooms use polygon intersection. An empty marquee box of at least one grid cell is also offered to the Stairs menu.
- Dragging a selected edge slides that edge, and every other selected edge, along its own normal. A corner shared by two selected edges takes both axes. This is the reshape mode.
- Dragging when the selection mixes kinds, or when the selected edges are exactly all the edges of one shape, moves everything rigidly: walls via `level_move_walls`, rooms, labels, and stairs by their own move calls, and doors follow their host wall. This is the move mode.
- A release without movement collapses a multi-selection to the pressed object.
- Right-click opens the same context menu from the current selection or click. Selected walls feed Door, a closed shape under the click feeds Fill, and the last empty marquee feeds Stairs.
- `Delete` or Edit > Delete removes every selected object in one history snapshot. Deleting a wall cascades its doors.
- `Esc` clears selection, cancels a draft, or restores a live drag.

## Inspector

Show properties for the selected object, all edits through `level.*` and committed once on `igIsItemDeactivatedAfterEdit`:

- Wall: id, axis, length, endpoints, Delete, and an Add door button that places a door at the wall midpoint.
- Room: color selector Blue/Red, vertex count, Delete.
- Door: color White/Red/Blue, position slider over the host t range, width slider from `DOOR_MIN_WIDTH` to host length, Delete.
- Label: single-line text input, size slider 8 to 48, position, Delete.
- Stairs: direction Horizontal/Vertical, step count slider `STAIR_MIN_STEPS` to `STAIR_MAX_STEPS`, position, Delete.

When exactly one object is selected, show its properties. When more than one is selected, show the total, per-kind counts, a short hint, and Delete. When nothing is selected, show level counts and a short controls list.

## Tool options in the left panel

Below the tool buttons:

- Grid size: combo 4, 8, 16, 32; default 8. A snap toggle, default on.
- Line: None / Blue / Red fill swatches, applied when the chain closes.
- Label: a size slider for new labels.
- Select: hints for click, Shift, Ctrl, marquee, reshape, and the right-click menu.

## File menu and persistence

- New: `level_init`, `history_reset`, clear selection, fit, path `levels/untitled.lvl`.
- Open: modal with a path input plus a small clickable list of `levels/*.lvl`. On load success, history_reset, clear selection, fit. On failure, show the error in the status strip and keep the current level.
- The save format is `ink-level 3`. Version 1 and 2 files still load; their stair records stored a centre and a derived size, which is converted to the explicit min-corner box.
- Save writes to the current path. Save As opens a path modal with the current path prefilled and lists `levels/*.lvl`. Report success or the error in the status strip.
- `Ctrl+S` saves, `Ctrl+Shift+S` opens Save As, `Ctrl+O` opens the Open modal.
- `Ctrl+Z` undo, `Ctrl+Shift+Z` or `Ctrl+Y` redo, disabled while a drag is active.

Use `igInputText` with a stable `char` buffer in the editor. Do not use native file dialogs.

## Undo

`History` stores whole-level snapshots. The UI commits after each completed mutation: add wall, close chain, add room, add door, add label, add stairs, delete, and a finished drag or inspector edit. Do not commit every mouse move. Undo/redo records the five pool counts. If any count changed, the selection clears. If counts are equal, it prunes invalid references.

## Keyboard summary

1 to 4 tools, `Delete` delete the selection, `Shift` plus click add to selection, `Ctrl` plus click toggle, drag empty canvas marquee, `Esc` cancel/deselect, `Enter` close, `Backspace` undo last chain wall, `F` fit, `G` toggle grid, `Ctrl+Z/Y` undo/redo, `Ctrl+S/O` save/open, `H` help.

## Acceptance checks

The UI owner must verify all of these on the real X11 window and report the commands and screenshots:

1. `make` builds with no warnings from `src/`.
2. Launch with the demo level. Screenshot shows blue and red fills, white/red/blue doors, labels, and hatched stairs. Sample pixels to prove the fill colors exist.
3. Draw a closed four-click rectangle with the Line tool. Screenshot shows the walls, and the right-click Mode submenu adds a fill. Verify with `level_stats` text or pixel sampling.
4. Switch to Select, click inside the new shape, press Delete, screenshot or status confirms removal.
5. Shift-drag a wall section, right-click Door, add a door. Drag it along the wall, resize it, change its color in the inspector.
6. Add a label with the Label tool, type in the inspector, move it. Shift-drag an empty box, right-click Stairs, change step count and direction.
7. Save to a temp path, New, Open the temp path, confirm the same object counts.
8. Undo and redo at least one add and one delete.
9. Shift-click two objects, drag them, confirm the group moves and snaps, then delete both in one step.
10. Drag a marquee across the level, delete all, undo, and confirm the counts return.

Screenshots go to `/tmp/ink-ribbon-verify-*.png`. Do not commit them.
