# Editor UI and interaction spec

This is the contract for `src/editor.c`, `src/editor.h`, and the editor hook in `src/main.c`. The level document and all mutations come from `src/level.h`; do not add another writer.

## Window layout

One sokol window titled `Ink Ribbon Level Editor`, 1280 by 800 initial size, near-black clear color.

The frame builds a full-viewport ImGui layout:

- A main menu bar at the top: File, Edit, View, Help.
- A left tool panel, 190 px wide, from below the menu bar to the bottom.
- A right inspector panel, 300 px wide, same vertical span.
- A canvas filling the middle. Canvas rect is stored in the editor so the mouse can be converted between screen and world.
- A one-line status strip overlaid at the bottom of the canvas. It shows the active tool hint, cursor world coordinates, zoom, and the latest action or error.

Panels use `ImGuiWindowFlags_NoTitleBar | NoResize | NoMove | NoCollapse | NoScrollbar`. Positions and sizes come from `igGetIO()->DisplaySize` and the menu bar height each frame.

## Visual language

Match the reference. Near-black canvas `#0b0c0e`. Subtle grid at 32 world units, `#15171a`, with a brighter grid line every 4th. Walls `#8b9097`, 2 screen pixels wide. Room blue `#2b3f50`, room red `#4a2028`, both opaque. Doors white `#f2f2f2`, red `#b3323b`, blue `#3d7fb5`. Labels `#e6e6e6`. Stairs `#767d85` treads on a slightly lighter footprint fill. Selection accent `#5aa0c8` with a 2 px outline or handles.

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
- Clicking within 12 screen pixels of the first point closes the chain: add the final wall back to the first point, then, when the fill prototype is `Auto`, add a room polygon from the chain points using the current fill color. End the chain. Select the new room.
- `Enter` also closes the chain when it has at least three points. `Esc` or right-click ends it open, keeping only the walls already added.
- `Backspace` removes the last wall added in the current chain and moves the chain tip back. This is a live document mutation; commit history after it.
- Show a preview segment from the last point to the constrained cursor, the first-point marker with a "close" highlight when near, and small vertex dots.
- Chain points are capped at `LEVEL_MAX_ROOM_PTS`; past that, closing still adds the wall but skips the room and reports why.

### Room

Click points to draw a polygon fill directly, independent of walls. Points snap to the grid, no orthogonality constraint. Clicking the first point, `Enter`, or a double-click closes it. `Esc` cancels the draft. On close, require 3 points and call `level_add_room` with the current fill color. Select it. This is the manual fill path and stays available in both prototype modes.

### Door

Door placement follows the prototype switch.

- `Click` mode: click near a wall; add a door centered at the projected t with width `min(44, wall_length)`, current door color. Select it.
- `Drag` mode: press near a wall, drag along it, release. Width is the dragged span length clamped to at least `DOOR_MIN_WIDTH` and at most the wall length, centered at the span midpoint. A press-release without movement behaves like click mode.
- Hover highlights the nearest wall within tolerance and previews the door bar.
- Door bar drawing follows the Door render prototype. `Cut` draws the wall in segments excluding each door span, then the bar, so the wall visually opens under the door. `Overlay` draws the full wall and the bar on top, then restrokes the wall through the bar so the difference is visible at normal zoom. Thickness is always `DOOR_THICKNESS`.
- In Select, dragging a door body slides it along its wall. Dragging within 8 screen pixels of either end resizes it around the opposite end. Clamp through `level_set_door_span`.

### Label

Click places a label at the snapped point with text `Room` and size 19. Select it and focus the inspector text field so typing immediately renames it. A default label is fine to create from a click; do not open a separate modal.

### Stairs

Click places stairs at the snapped point using the current direction and step count from the tool options. Select it. The inspector lets the user change direction and step count. The footprint follows the model constants.

### Select

Click hit-tests in this order: door, label, stairs, wall, room. Tolerance is 7 screen pixels for walls and doors. Labels use their measured text rect. Stairs use `level_stairs_rect`. Rooms use `level_room_at`.

- Drag a label, stair, or room to move it. Rooms move all points. A drag mutates live and commits one history snapshot on release.
- Drag a wall: select only. No wall endpoint editing in this version.
- Click empty space clears the selection.
- `Delete` or Edit > Delete removes the selection. Deleting a wall cascades its doors through `level_delete_wall`; clear the selection after.
- `Esc` clears selection, then cancels chain or polygon draft.

## Inspector

Show properties for the selected object, all edits through `level.*` and committed once on `igIsItemDeactivatedAfterEdit`:

- Wall: id, axis, length, endpoints, Delete, and an Add door button that places a door at the wall midpoint.
- Room: color selector Blue/Red, vertex count, Delete.
- Door: color White/Red/Blue, position slider over the host t range, width slider from `DOOR_MIN_WIDTH` to host length, Delete.
- Label: single-line text input, size slider 8 to 48, position, Delete.
- Stairs: direction Horizontal/Vertical, step count slider `STAIR_MIN_STEPS` to `STAIR_MAX_STEPS`, position, Delete.

When nothing is selected, show level counts and a short controls list.

## Tool options in the left panel

Below the tool buttons:

- Grid size: combo 4, 8, 16, 32; default 8. A snap toggle, default on.
- Fill color: Blue / Red swatches, used for new rooms and room recolor.
- Door color: White / Red / Blue swatches.
- Stairs: direction toggle and step count slider.
- Prototype panel, either here or in the inspector: three radio groups for Door placement, Room fill, and Door render, with a one-line explanation and a "recommended" label on Click / Auto / Cut. These are comparison controls, not decorative.

## File menu and persistence

- New: `level_init`, `history_reset`, clear selection, fit, path `levels/untitled.lvl`.
- Open: modal with a path input plus a small clickable list of `levels/*.lvl`. On load success, history_reset, clear selection, fit. On failure, show the error in the status strip and keep the current level.
- Save writes to the current path. Save As opens a path modal with the current path prefilled and lists `levels/*.lvl`. Report success or the error in the status strip.
- `Ctrl+S` saves, `Ctrl+Shift+S` opens Save As, `Ctrl+O` opens the Open modal.
- `Ctrl+Z` undo, `Ctrl+Shift+Z` or `Ctrl+Y` redo, disabled while a drag is active.

Use `igInputText` with a stable `char` buffer in the editor. Do not use native file dialogs.

## Undo

`History` stores whole-level snapshots. The UI commits after each completed mutation: add wall, close chain, add room, add door, add label, add stairs, delete, and a finished drag or inspector edit. Do not commit every mouse move. Undo/redo clears transient state and validates the selection.

## Keyboard summary

1 to 6 tools, `Delete` delete, `Esc` cancel/deselect, `Enter` close, `Backspace` undo last chain wall, `F` fit, `G` toggle grid, `Ctrl+Z/Y` undo/redo, `Ctrl+S/O` save/open, `H` help.

## Acceptance checks

The UI owner must verify all of these on the real X11 window and report the commands and screenshots:

1. `make` builds with no warnings from `src/`.
2. Launch with the demo level. Screenshot shows blue and red fills, white/red/blue doors, labels, and hatched stairs. Sample pixels to prove the fill colors exist.
3. Draw a closed four-click rectangle with the Wall tool. Screenshot shows the walls and, in Auto mode, a new fill. Verify with `level_stats` text or pixel sampling.
4. Switch to Select, click the new fill, press Delete, screenshot or status confirms removal.
5. Place a door on a wall, drag it along the wall, resize it, change its color in the inspector.
6. Add a label, type in the inspector, move it. Add stairs, change step count and direction.
7. Save to a temp path, New, Open the temp path, confirm the same object counts.
8. Undo and redo at least one add and one delete.

Screenshots go to `/tmp/ink-ribbon-verify-*.png`. Do not commit them.
