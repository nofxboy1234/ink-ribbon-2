# Editor UI and interaction spec

This is the contract for `src/editor.c`, `src/editor.h`, and the editor hook in `src/main.c`. The level document and all mutations come from `src/level.h`; do not add another writer.

## Window layout

One sokol window titled `Ink Ribbon Level Editor`, 1280 by 800 initial size, near-black clear color.

The frame builds a full-viewport ImGui layout:

- A main menu bar at the top: File, Edit, View, Help.
- A left tool panel, 190 px wide, from below the menu bar to the bottom.
- A right inspector panel, 300 px wide, same vertical span.
- A canvas filling the middle. Canvas rect is stored in the editor so the mouse can be converted between screen and world.
- A one-line status strip overlaid at the bottom of the canvas. It shows the active tool hint, cursor world coordinates, zoom, the current snap size or `snap off`, and the latest action or error.

Panels use `ImGuiWindowFlags_NoTitleBar | NoResize | NoMove | NoCollapse | NoScrollbar`. Positions and sizes come from `igGetIO()->DisplaySize` and the menu bar height each frame.

## Visual language

Match the reference. Near-black canvas `#0b0c0e`. Grid lines at the selected Grid size, subtle at `#333b45` with low alpha, a brighter line every 4 cells and the brightest every 16. The grid gets coarser only when the fine lines would be closer than 4 screen pixels. Walls `#8b9097`, 2 screen pixels wide. Room blue `#2b3f50`, room red `#4a2028`, both opaque. Doors white `#f2f2f2`, red `#b3323b`, blue `#3d7fb5`. Labels `#e6e6e6`. Stairs `#767d85` treads on a slightly lighter footprint fill. Selection accent `#5aa0c8` with a 2 px outline or handles.

Dark ImGui theme: window background `#101114`, panel `#16181c`, frame `#1d2025`, borders `#2a2e34`, text `#d9dadd`, accent `#4e7f9e`. Rounded corners 3 px. Sentence-case headings.

The embedded scalable ImGui default font at 15 px is fine. Do not depend on system font files.

## Canvas transform and navigation

Camera stores a world-space center and a zoom in screen pixels per world unit. World y grows down. `screen_to_world` and `world_to_screen` are the only transforms.

- Mouse wheel zooms around the cursor, clamped 0.15 to 6.
- Middle-drag pans. Space plus left-drag pans.
- `F` or View > Fit frames the level bounds with margin. If the level is empty, center 0,0 at zoom 1.
- On startup, load `levels/re2-library.lvl` if it exists, then fit.

## Tools and gestures

Tools: Select, Wall, Room, Door, Label, Stairs. Left panel buttons plus number keys 1 to 6. The active tool is visibly highlighted. Switching tools ends any open chain or polygon draft without committing it.

### Wall

Click sets points. Each click appends an orthogonal segment that continues from the last click, exactly as requested.

- The first click starts a chain at the grid-snapped point.
- Each later click uses `snap_ortho(chain_last, cursor_world, grid)` so a segment is horizontal when `|dx| > |dy|`, vertical otherwise, and the free coordinate snaps to the grid.
- Clicking within 12 screen pixels of the first point closes the chain: add the final wall back to the first point, then add a room polygon from the chain points using the current fill color. End the chain. Select the new room.
- `Enter` also closes the chain when it has at least three points. `Esc` or right-click ends it open, keeping only the walls already added.
- `Backspace` removes the last wall added in the current chain and moves the chain tip back. This is a live document mutation; commit history after it.
- Show a preview segment from the last point to the constrained cursor, the first-point marker with a "close" highlight when near, and small vertex dots.
- Chain points are capped at `LEVEL_MAX_ROOM_PTS`; past that, closing still adds the wall but skips the room and reports why.

### Room

Click points to draw a polygon fill directly, independent of walls. Points snap to the grid, no orthogonality constraint. Clicking the first point, `Enter`, or a double-click closes it. `Esc` cancels the draft. On close, require 3 points and call `level_add_room` with the current fill color. Select it.

### Door

Click near a wall to add a door centered at the projected t with width `min(44, wall_length)`, current door color. The center snaps to the grid along the wall axis. Select the new door.

- Hover highlights the nearest wall within tolerance and previews the snapped door bar.
- The wall is always drawn in segments that exclude each door span, then the bar. The wall visually opens under the door. Thickness is always `DOOR_THICKNESS`.
- In Select, dragging a door body slides it along its wall. Dragging within 8 screen pixels of either end resizes it around the opposite end. Both snap with the grid and clamp through `level_set_door_span`.

### Label

Click places a label at the snapped point with text `Room` and size 19. Select it and focus the inspector text field so typing immediately renames it. A default label is fine to create from a click; do not open a separate modal.

### Stairs

Click places stairs at the snapped point using the current direction and step count from the tool options. Select it. The inspector lets the user change direction and step count. The footprint follows the model constants.

### Select

Click hit-tests in this order: door, label, stairs, wall, room. Tolerance is 7 screen pixels for walls and doors. Labels use their measured text rect. Stairs use `level_stairs_rect`. Rooms use `level_room_at`.

Selection is a bounded set per object kind plus a primary reference. The set is never serialized.

- Plain click on an unselected object replaces the selection. Plain click on a selected object inside a multi-selection keeps the group and makes that object primary.
- Shift-click adds the hit object and never removes one. Ctrl-click toggles it in or out. Neither starts a drag.
- Plain press on empty canvas clears the selection and starts a marquee. Shift-press starts an additive marquee. Travel under 4 screen pixels is a click.
- On marquee release, select every object intersecting the rectangle. Walls and doors use segment intersection, labels and stairs use rect overlap, rooms use polygon intersection.
- Drag a selected wall to move it, or the whole group when it is part of a multi-selection. Selected rooms, labels, and stairs move with it. The group snaps from the primary object's anchor and commits one history snapshot on release. A wall keeps its id and stays axis-aligned.
- A door follows its host wall, so it moves with a selected wall. A door whose host wall is not selected stays put and the status line says so. A door dragged when it is the only selected object slides along its wall, and dragging an end handle resizes it.
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
- Fill color: Blue / Red swatches, used for new rooms and room recolor.
- Door color: White / Red / Blue swatches.
- Stairs: direction toggle and step count slider.
- Selection hints under a Selection heading: shift-click adds, drag empty canvas to marquee, and snap follows the grid size.

## File menu and persistence

- New: `level_init`, `history_reset`, clear selection, fit, path `levels/untitled.lvl`.
- Open: modal with a path input plus a small clickable list of `levels/*.lvl`. On load success, history_reset, clear selection, fit. On failure, show the error in the status strip and keep the current level.
- Save writes to the current path. Save As opens a path modal with the current path prefilled and lists `levels/*.lvl`. Report success or the error in the status strip.
- `Ctrl+S` saves, `Ctrl+Shift+S` opens Save As, `Ctrl+O` opens the Open modal.
- `Ctrl+Z` undo, `Ctrl+Shift+Z` or `Ctrl+Y` redo, disabled while a drag is active.

Use `igInputText` with a stable `char` buffer in the editor. Do not use native file dialogs.

## Undo

`History` stores whole-level snapshots. The UI commits after each completed mutation: add wall, close chain, add room, add door, add label, add stairs, delete, and a finished drag or inspector edit. Do not commit every mouse move. Undo/redo records the five pool counts. If any count changed, the selection clears. If counts are equal, it prunes invalid references.

## Keyboard summary

1 to 6 tools, `Delete` delete the selection, `Shift` plus click add to selection, `Ctrl` plus click toggle, drag empty canvas marquee, `Esc` cancel/deselect, `Enter` close, `Backspace` undo last chain wall, `F` fit, `G` toggle grid, `Ctrl+Z/Y` undo/redo, `Ctrl+S/O` save/open, `H` help.

## Acceptance checks

The UI owner must verify all of these on the real X11 window and report the commands and screenshots:

1. `make` builds with no warnings from `src/`.
2. Launch with the demo level. Screenshot shows blue and red fills, white/red/blue doors, labels, and hatched stairs. Sample pixels to prove the fill colors exist.
3. Draw a closed four-click rectangle with the Wall tool. Screenshot shows the walls and a new fill. Verify with `level_stats` text or pixel sampling.
4. Switch to Select, click the new fill, press Delete, screenshot or status confirms removal.
5. Place a door on a wall, drag it along the wall, resize it, change its color in the inspector.
6. Add a label, type in the inspector, move it. Add stairs, change step count and direction.
7. Save to a temp path, New, Open the temp path, confirm the same object counts.
8. Undo and redo at least one add and one delete.
9. Shift-click two objects, drag them, confirm the group moves and snaps, then delete both in one step.
10. Drag a marquee across the level, delete all, undo, and confirm the counts return.

Screenshots go to `/tmp/ink-ribbon-verify-*.png`. Do not commit them.
