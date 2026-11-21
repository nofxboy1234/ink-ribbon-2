# Editor UI and interaction spec

This is the contract for `src/editor.c`, `src/editor.h`, and the editor hook in `src/main.c`. The level document and all mutations come from `src/level.h`; do not add another writer.

## Window layout

One sokol window titled `Ink Ribbon Level Editor`, 1280 by 800 initial size, near-black clear color.

The frame builds a full-viewport ImGui layout:

- A main menu bar at the top: File, Edit, View, Help.
- A left tool panel, 190 px wide, from below the menu bar to the bottom. It shows the six tool buttons, the Q lock row, then Grid, Snap, Show grid, and Snap objects, then only the active tool's settings, and a Player section at the bottom with Play mode, Remove player, and a recenter hint.
- A right inspector panel, 300 px wide, same vertical span.
- A canvas filling the middle. The canvas rect is stored in the editor so the mouse can be converted between screen and world.
- A one-line status strip overlaid at the bottom of the canvas. It shows the active tool hint, cursor world coordinates, zoom, the current snap size or `snap off`, and the latest action or error.

Panels use `ImGuiWindowFlags_NoTitleBar | NoResize | NoMove | NoCollapse | NoScrollbar`. Positions and sizes come from `igGetIO()->DisplaySize` and the menu bar height each frame.

## Visual language

One look. Near-black canvas `#040404`. Grid lines at the selected Grid size, subtle at `#333b45` with low alpha, a brighter line every 4 cells and the brightest every 16. The grid gets coarser only when the fine lines would be closer than 4 screen pixels.

Walls `#8b9097`, 2 screen pixels wide, at alpha 70 everywhere. There is no bright wall state, no fill, and no room colour. Doors are always white `#f2f2f2` 3 px bars. Labels are `#e6e6e6`. Stair treads are `#c8ccd2` with a fade that starts at one end of the run and no footprint fill. Selection uses accent `#5aa0c8` with a 2 px outline or handles. Nothing in the editor paints a filled polygon.

Dark ImGui theme: window background `#101114`, panel `#16181c`, frame `#1d2025`, borders `#2a2e34`, text `#d9dadd`, accent `#4e7f9e`. Rounded corners 3 px. Sentence-case headings.

The embedded scalable ImGui default font at 15 px is fine. Do not depend on system font files.

## Frame rate

The current frame rate draws in green at the top right of the canvas, inset 10 px from the right edge and 6 px from the top, so it never collides with the status strip. It reads `ImGuiIO::Framerate` each frame.

## Canvas transform and navigation

The camera stores a world-space center and a zoom in screen pixels per world unit. World y grows down. `screen_to_world` and `world_to_screen` are the only transforms.

ImGui clears the pointer position to `-FLT_MAX` when the window loses focus. Never feed that value into `screen_to_world` or into a preview. Check `mouse_available` first, draw no preview without a pointer, and release any drag at its last applied position.

- Mouse wheel zooms around the cursor, clamped 0.15 to 6. Zoom is immediate, with no catch-up animation.
- In play mode the camera follows the player while walking, changing only the centre and never the zoom. When a path starts with the camera offset, or when `C` is pressed, the camera smoothly pans to the player at the current zoom. Middle-drag pans immediately, cancels the pan, sets follow off, and has no catch-up animation. Wheel zoom is immediate and does not start a pan.
- Middle-drag pans. Space plus left-drag pans.
- `F` or View > Fit frames the level bounds with margin. If the level is empty, center 0,0 at zoom 1.
- On startup, load `levels/re2-library.lvl` if it exists, then fit.

## Tools, keys, and auto-select

Exactly six tools, in this order and on these keys:

1. Select
2. Line
3. Rectangle
4. Door
5. Stairs
6. Label

Select is the default. The tool is the only owner of which gesture a press starts, so there is no second mode flag.

After one drawing action the editor switches to Select unless Q is held when the action completes. A drawing action is one completed rectangle drag, one closed line chain, one placed door, one placed stair, or one placed label. An open line chain does not switch. The switch keeps the action's status message. A small `Q lock: tool stays` row in the left panel lights while Q is held. The row is always present so the panel does not jump. Typing Q into a label field is not a lock.

Switching tools ends any open chain without committing it. A drag that was live restores the last committed snapshot.

## Line

Click sets points. Each click appends an orthogonal segment that continues from the last click.

- The first click starts a chain at the grid-snapped point.
- Each later click uses `snap_ortho(chain_last, cursor_world, grid)` so a segment is horizontal when `|dx| > |dy|`, vertical otherwise, and the free coordinate snaps to the grid.
- The first click creates a shape with `level_add_shape`. Every later click calls `level_add_wall_to_shape`.
- Clicking within 12 screen pixels of the first point closes the chain: add the final wall back to the first point, call `level_close_shape`, end the chain, and hand the tool back to Select. A close needs at least four walls; report that when it fails.
- `Enter` also closes the chain when it has at least three points. `Esc` ends it open, keeping only the walls already added.
- `Backspace` removes the last wall added in the current chain and moves the chain tip back. That is a live document mutation; commit history after it.
- A plain drag does nothing. There is no Shift marquee.
- Show a preview segment from the last point to the constrained cursor, the first-point marker with a `close` highlight when near, and small vertex dots.
- Chain points are bounded by the wall pool and the shape pool; a failed add reports why in the status strip.

## Rectangle

Drag an axis-aligned rectangle. Both corners snap to the current grid on press and release whether or not Snap is on, because a rectangle is a grid cell and its outline is what later rectangles join. A drag under one grid cell on either axis is rejected and does not complete an action.

On release the rectangle tries to join a closed shape. Candidate shapes are examined from newest to oldest, and only shapes whose bounds overlap the rectangle are tried. `level_union_rect` returns:

- `LEVEL_UNION_OK` when the rectangle shares an edge or an area with the shape. The shape's walls are replaced by the union loop, the shape id stays, and the action commits as `Rectangle joined`. Doors on walls that survive are re-hosted at the same world centre. Doors on walls that become interior are dropped and the count is reported.
- `LEVEL_UNION_DISJOINT` when the rectangle only touches a corner or does not touch the shape. The editor tries the next shape, then creates a new closed four-wall shape with `level_add_rect_shape` and commits `Rectangle added`.

The union is the shape's own geometry, not a stored rectangle list. It survives save and load as ordinary walls.

The rectangle preview and the stair preview draw an accent outline. The empty look never fills them.

## Door

The Door tool uses the wall under the pointer. The search radius is 22 screen pixels converted to world units. The projected point gives the centre parameter. With Snap on, the parameter snaps along the wall only, so the ghost stays on the wall line.

A ghost bar draws at the panel door width, centred on the snapped point and clamped so it stays on the wall. It shows the door a click will place. Click places a white door with `level_add_door`, selects it, and commits `Door added`. The tool then returns to Select unless Q is held. Clicking away from every wall reports `Hover a wall to place a door` and does not complete an action.

There is no marquee door workflow and no door colour menu. The inspector slides the door along its wall and changes its width. Dragging either end with Select resizes it.

## Stairs

Drag a rectangle. Both corners snap with the same rule as the Rectangle tool. The release needs at least one grid cell on each axis, or `STAIR_MIN_SIZE` when Snap is off. The panel's falloff and faint/bright opacity sliders are copied to the new stair.

The stair takes the panel's direction and step count and the panel's Flip flag. Flip sets which end of the run the tread fade starts from. The stair is selected, the action commits `Stairs added`, and the tool returns to Select unless Q is held.

Treads draw as 3 px neutral lines. The falloff shapes the way opacity travels from the faint end to the bright end, and the two opacity sliders set those end values. Flip chooses which end of the run is the faint one. There is no footprint fill. Dragging a stair edge with Select resizes the footprint. The inspector changes direction, steps, Flip, falloff, and both opacities for the selected stair, and each change commits.

There is no marquee stairs workflow.

## Label

Click places a label at the snapped point with the left panel's label size, selects it, and focuses the inspector text field so typing immediately renames it. `Enter` commits the text. The inspector also edits size and position. The tool returns to Select unless Q is held, and the label stays selected.

## Select

Click hit-tests in this order: door, label, stairs, wall, closed shape interior. Tolerance is 7 screen pixels for walls and doors. Labels use their measured text rect. Stairs use `level_stairs_rect`.

A wall hit selects that edge. Clicking inside a closed shape selects all of its edges. Dragging a selected wall starts a reshape: a horizontal wall slides in y, a vertical one in x, the drag snaps to the grid, and `level_slide_edges` carries every coincident endpoint so connected lines stretch and opposite lines stay. The gesture commits one snapshot on release and restores the previous snapshot on Esc.

Selection is a bounded set per object kind plus a primary reference. Walls are selected as individual edges. The set is never serialized.

- A selection can be locked with `L` or the inspector's Lock selection checkbox. Locking a wall also locks the doors hosted by that wall. Locks live on the selected objects for the session, so other objects can still be selected and edited. Locked objects remain selectable so they can be unlocked. A selection containing any locked object refuses canvas drags, `Delete`, `Ctrl+D`, and inspector edits; the lock checkbox stays enabled. Locks are not saved with the level.
- Locked walls are skipped by marquee section marking and section cutting.
- Plain click on an unselected object replaces the selection. Plain click on a selected object inside a multi-selection keeps the group and makes that object primary.
- Shift-click adds the hit object and never removes one. Ctrl-click toggles it in or out.
- A plain press within 7 screen pixels of a wall endpoint starts a corner drag. The level moves the shared vertex and every vertex that must follow, so attached walls stay orthogonal and the outline stays connected. Snap puts the corner on the grid; Snap objects instead aligns it to nearby wall lines and endpoints within 8 screen pixels. Select draws hollow squares at wall endpoints as handles.
- Press `M` or tick **Marquee next drag** to arm one marquee. The next Select drag starts a marquee even when the press lands inside a closed shape, allowing a box over a nested inner room without moving the outer shape. The arm clears when the marquee starts. A drag on empty canvas still starts a marquee without arming.
- Plain press on empty canvas clears the selection and the marked sections, then starts a marquee. Shift-press on empty canvas starts an additive marquee. Travel under 4 screen pixels is a click.
- On marquee release, select whole non-wall objects inside the rectangle. Doors use segment intersection, labels and stairs use rect overlap. Walls are never selected whole by a box; the crossed part of every wall is marked as a section instead, clipped in wall parameters to the snapped box.
- Dragging a selected edge slides that edge, and every other selected edge, along its own normal. A corner shared by two selected edges takes both axes.
- Dragging when the selection mixes kinds, or when the selected edges are exactly all the edges of one shape, moves everything rigidly: walls via `level_move_walls`, labels and stairs by their own move calls, and doors follow their host wall. Object snapping checks every moving wall endpoint and label or stair box edge on each axis, so the nearest side wins; grid snapping still measures the primary anchor.
- A release without movement collapses a multi-selection to the pressed object.
- There is no right-click menu. Right-click does nothing.
- `Ctrl+D` duplicates the selection one grid cell diagonally. Wall shapes keep their closed flag, labels keep text and size, stairs keep every setting (direction, steps, flip, falloff, and both opacities), and doors are re-hosted on the duplicated wall with the same position and width. A selected door whose wall was not duplicated gets a duplicated open wall to sit on.
- `Delete` or Edit > Delete removes every selected object in one history snapshot. Deleting a wall cascades its doors.
- The marked sections are a `Span` list of wall id plus two ordered parameters, canonical per wall: intervals merge when they overlap and are cut from the far end back. Each section draws over its wall at double width in the accent colour with a square at each end.
- `Delete` cuts every marked section through `level_cut_wall_span` in one history snapshot and reports the section and door counts. A selected door, label, or stair still deletes in the same press. Whole selected walls delete only when no section is marked.
- A press elsewhere or a tool switch clears the marked sections. `Esc` clears them first, then the selection, cancels a draft, or restores a live drag.

## Inspector

Show properties for the selected object, all edits through `level.*` and committed once on `igIsItemDeactivatedAfterEdit`:

- Wall: shape id, closed state, wall count, edge index, Delete.
- Door: wall id, position slider over the host t range, width slider from `DOOR_MIN_WIDTH` to host length, Delete.
- Label: single-line text input, size slider 8 to 48, position, Delete.
- Stairs: direction buttons, step count slider `STAIR_MIN_STEPS` to `STAIR_MAX_STEPS`, Flip checkbox, falloff combo, faint and bright opacity sliders, size, position, Delete.

Above the object properties, show a Lock selection checkbox when anything is selected. It is checked when every selected object is locked; ticking it locks all selected objects and unticking unlocks them. If any selected object is locked, the properties and Delete are disabled; the checkbox stays enabled so the selection can be unlocked. When exactly one object is selected, show its properties. When more than one is selected, show the total, per-kind counts, a short hint, and Delete. When nothing is selected, show level counts and a short controls list.

## Player and play mode

At most one player record exists. The Player section at the bottom of the left panel has a Play mode checkbox (also `P`), a Place player button when no player exists, and a Remove player button once one does. On entering play mode the nav grid is rebuilt and the selection and marked sections are cleared.

- In edit mode, Place player arms a click on a walkable nav cell and places the player at that cell's centre, without entering play mode. Esc cancels the armed placement. The same placement happens on the first play-mode click when no player exists.
- With a player, a play-mode click builds an A* path and starts walking.
- The nav grid is 8 world units per cell. A cell is open when its centre is inside a closed room or corridor and clears every solid wall by the player radius. Door openings are removed from every collinear wall record, so doors are passable. Stair treads are not walls; stair-side walls remain solid.
- Hovering in play mode draws the nav cell as a green-bordered square when walkable, and a muted red border when blocked. It also draws the planned A* route from the player to that cell as a green line. After a click, the remaining route is drawn in green while the player walks. The same green hover cell appears while `Place player` is armed in edit mode.
- A* uses 8-way movement and forbids diagonal corner cutting. Its cell route is line-of-sight filtered, then each interior corner is replaced by a quadratic Bezier arc. Arc radii start at 10 world units and shrink until the sampled arc stays on open cells. The resulting dense waypoints are what is drawn, previewed, and walked.
- Any filtered segment that crosses a door line near the opening is split at that door's centre only. The centre is a fixed, unrounded waypoint: no sideways pre/post detour is inserted, which previously produced loops near doorways.
- The player consumes the frame's travel across as many dense curve samples as needed, giving a constant-speed follow instead of stopping at each sample.
- If the next destination is directly horizontal or vertical behind the player, the player pivots on the spot to face it. Otherwise it steers with a limited turn rate and moves along its facing direction, tracing a turning-circle arc; normal path points are approached directly so the player cannot orbit a close waypoint. A visible 16-unit turning ring is drawn whenever the player is moving.
- `C` recentres on the player at the current zoom. `Esc` stops the player. `P` returns to editing. The player position and heading are saved in the level.

## Tool options in the left panel

Below the six tool buttons and the Q lock row:

- Grid size: combo 4, 8, 16, 32; default 8. A Snap toggle, default on. A Show grid toggle, default on. A Snap objects toggle, default on.
- Select: a **Marquee next drag** checkbox (same as pressing M) and a hint that a wall corner drags to resize.
- Line: a hint.
- Rectangle: a hint that overlapping rectangles union.
- Door: a hint and a door width slider from `DOOR_MIN_WIDTH` to 200, default 10. New doors are 3 px thick.
- Stairs: direction buttons, a steps slider, a Flip checkbox, a falloff combo (Linear, Ease in, Ease out, Smooth), faint and bright opacity sliders, and a hint.
- Label: a size slider from 8 to 48.

There is no Fill palette.

## File menu and persistence

- New: `level_init`, `history_reset`, clear selection, fit, path `levels/untitled.lvl`.
- Open: modal with a path input plus a small clickable list of `levels/*.lvl`. On load success, history_reset, clear selection, fit. On failure, show the error in the status strip and keep the current level.
- The save format is `ink-level 3`. A level may carry one `player x y heading` record. The shape record keeps its fill column but writes 0, and the door record keeps its colour column but writes 0. A loaded fill or door colour is ignored. Room records from version 1 to 3 are read and skipped. Stair records keep the optional trailing fade-end flag, falloff, and faint/bright opacities; omitted values load with the linear 0.18/0.68 defaults.
- Save writes to the current path. Save As opens a path modal with the current path prefilled and lists `levels/*.lvl`. Report success or the error in the status strip.
- `Ctrl+S` saves, including in play mode, and never toggles Snap. `Ctrl+Shift+S` opens Save As, `Ctrl+O` opens the Open modal.
- `Ctrl+Z` undo, `Ctrl+Shift+Z` or `Ctrl+Y` redo, disabled while a drag is active.

Use `igInputText` with a stable `char` buffer in the editor. Do not use native file dialogs.

## Undo

`History` stores whole-level snapshots. The UI commits after each completed mutation: add wall, close chain, add or join a rectangle, place a door, add a label, add stairs, delete, and a finished drag or inspector edit. Do not commit every mouse move. Undo/redo records the five pool counts. If any count changed, the selection clears. If counts are equal, it prunes invalid references.

## Keyboard summary

1 to 6 tools, `Q` held to keep the drawing tool, `L` lock/unlock the selection, `M` arm the next marquee, `P` play mode, `C` recentre the player, `Ctrl+D` duplicate, `Delete` cut the marked sections and delete selected objects, `Shift` plus click add to selection, `Ctrl` plus click toggle, drag a box on empty canvas to marquee and mark sections, drag a wall corner or edge to resize, `Esc` cancel/deselect, `Enter` close, `Backspace` undo last chain wall, `F` fit, `G` toggle grid, `Ctrl+Z/Y` undo/redo, `Ctrl+S/O` save/open, `H` help.

## Acceptance checks

The UI owner must verify these on the real X11 window and report the commands and screenshots:

1. `./fibs build` builds with no warnings from `src/`.
2. Load a legacy level with fills, a room polygon, and a coloured door. Screenshot shows faint walls, a white door, and a black interior. Save it and confirm fill 0, door colour 0, and no room record.
3. Press 1 to 6 and confirm one tool button lights per key and key 7 changes nothing.
4. Draw two overlapping rectangles. The saved level has one shape with exactly the boundary walls, no interior lines, and the union area.
5. Reload that saved level and compare every wall record.
6. Hover a wall with the Door tool and confirm the ghost. Click and confirm a white door at the panel width and projected point. Confirm the next click does not add a door.
7. Hold Q while placing a door and confirm the tool stays for the next click.
8. Drag a stair with Flip set and confirm the saved flag. Toggle Flip in the inspector and confirm the saved flag changes.
9. Lose focus during a line draft and confirm the editor redraws afterwards.
10. Drag a box from above the top wall of a rectangle to below it and confirm the accent highlight on the crossed part. Press Delete and save. The level has five walls with a gap where the box crossed. A door inside the gap is gone and a door outside keeps its saved world centre. Drag a rectangle corner and confirm both walls at that corner share the new endpoint and every saved wall is still horizontal or vertical.

Screenshots go to `/tmp/ink-ribbon-six-*.png`. Do not commit them.
