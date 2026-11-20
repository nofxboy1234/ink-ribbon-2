# Level editor architecture

## Problem

The editor must draw floor-plan style maps as white lines on a black canvas. The
map is built from orthogonal wall outlines, rectangles that union into one
outline, white doors that live on a wall and slide along it, text labels, and
stairs with an adjustable direction, step count, and fade end. Everything is
editable and deletable, and nothing is filled or coloured. The implementation is
C with sokol for the window and Dear ImGui for the UI. An earlier design had a
welded wall graph with vertex pools, generational handles, cycle walking, and
wall sliding. That is a CAD kernel for problems the request does not have. The
editor needs to create, edit, and delete objects, not to maintain a topological
graph under arbitrary wall edits.

## Shape

One pointer-free `Level` document of five typed pools: shapes, walls, doors,
labels, stairs. Every pool has a fixed capacity and a live count. A whole
document copy is a struct assignment, which makes snapshot undo and
load-after-validate trivial.

A shape is one drawn wall loop: a `Shape` record with a closed flag, plus the
walls that belong to it. Walls stay independent axis-aligned segments with a
monotonic `WallId` and a `ShapeId`. Wall order inside a shape is the order the
walls were added, so the loop polygon is a traversal, not a second geometry
copy. The Rectangle tool adds its four walls in loop order and the union writes
its loop in traversal order. There is no vertex pool: corners meet because the
Line tool starts each segment at the previous endpoint, and a reshape carries
every endpoint that coincides with the moved line. A corner drag groups the
coincident endpoints by coordinate in `level_move_vertex`, moves the grabbed
group, and carries each vertex that must follow along its wall's axis so every
wall stays orthogonal and the outline stays connected. `level_add_wall` rejects a
zero-length or non-orthogonal segment, so every stored wall is drawable and
every door host has a well-defined axis.

Doors store `{ WallId wall, float t, float width }`. `t` is the centre parameter
from 0 to 1. The thickness is the `DOOR_THICKNESS` constant for every door, and
the colour is always white. `level_set_door_span` clamps the span to the host
wall. Deleting a wall deletes its doors in the same operation, so a door never
points at a dead host.

Labels and stairs are independent placed objects. The stair footprint is an
explicit min-corner box, so its edges can be dragged, and `dir`, `steps`, the
fade-end flag, the falloff curve, and the faint/bright end opacities are stored
fields rather than derived UI state. Every object also carries a session-only
`locked` flag; it is not serialized, but it rides along with the object through
array shifts and undo snapshots. Older stair records omit the last three,
so they load with the linear 0.18/0.68 default.

Undo is a ring of `Level` snapshots in `History`. The UI commits one snapshot
per completed gesture. Command deltas would need a correct inverse for every
mutation, and cascade deletes are exactly where inverses are hard. Snapshot undo
needs none.

The core (`geom.*`, `level.*`) includes no sokol or ImGui header, so it is
tested headlessly. `main.c` owns the sokol lifecycle. `editor.*` owns UI state,
gestures, and drawing. The split is enforced by `fibs.ts`: `test_level` depends
only on the `ink-core` library.

## Rectangle union

A shape stores its outline, not the rectangles that formed it. Adding a
rectangle finds the newest overlapping closed shape and calls
`level_union_rect`. The union collects every x and y coordinate in the shape
polygon and the rectangle, which cuts the plane into cells. A cell is wholly
inside or outside the union, so one point-in-polygon test per cell is exact.
The boundary is every cell side whose neighbour is empty, merged into maximal
runs and chained into one loop. Interior lines cannot survive, because an
interior shared side has occupied cells on both sides. A rectangle that only
shares a corner leaves two 4-disconnected cell sets, so it is rejected as
disjoint and starts a new shape.

The union replaces the shape's walls and keeps the shape id. Doors are captured
by world centre, width, and axis before the walls go, then re-hosted onto the
matching new wall. A door on a wall that survives keeps its exact centre. A door
on a wall that becomes interior is dropped and counted. The union is ordinary
walls when saved, so the level format did not change.

## Fixed interaction choices

The shipped UI has six tools: Select, Line, Rectangle, Door, Stairs, and Label,
on keys 1 to 6, with Select as the default. A completed drawing action returns
the tool to Select unless Q is held. The Q lock row in the left panel shows the
held state. One action is a closed chain, a completed rectangle or stair drag, a
placed door, or a placed label. An open chain is not an action.

The Rectangle tool always snaps to the grid. The Door tool replaced the
marquee and context menu: the wall under the pointer projects a snapped centre
and draws a ghost bar at the panel width, and a click places the door. The
Stairs tool replaced the marquee box: a drag becomes a run with the panel's
direction, steps, and fade end, and the fade end is saved.

Selection is UI state in `editor.c`, not part of `Level`. It is four bounded
per-kind sets plus a primary reference. Walls are selected as edges, and
hitting a wall selects that edge; clicking inside a closed shape selects all of
its edges. Shift-click adds, Ctrl-click toggles, a box replaces or adds, and
Delete removes every selected object in one snapshot. A box skips whole walls
and instead marks the crossed part of each wall as a canonical `Span` list, so
Delete cuts every marked section through `level_cut_wall_span` in that same
snapshot. A drag has two modes. When
the selection is edges that are not exactly a whole shape, `GESTURE_RESHAPE`
slides each selected edge along its own normal through `level_slide_edges`, so
corners between two selected edges take both axes and unselected neighbours
stretch. When the selection mixes kinds or is exactly a whole shape,
`GESTURE_MOVE` translates the selected walls with `level_move_walls` and moves
labels and stairs. `GESTURE_VERTEX` starts within 7 screen pixels of a wall
endpoint and calls `level_move_vertex` on each drag step. A door stores a wall id and a normalized position, so it
follows its host wall and cannot be translated independently.

Snap applies to placement, reshaping, and label, stair, door, and group drags.
Group drags snap from the primary object's anchor so relative spacing is
preserved. The Snap checkbox and grid size remain user controls.

## Alternatives rejected

- Rectangle list as canonical geometry. It keeps each rectangle as editable
  geometry, but every Select reshape and every wall delete would desync the
  list from the outline, so the shape would have two sources of truth. The
  outline is the observable object, so it is the model.
- Orthogonal polygon clipping. It is exact but needs vertex classification and
  edge stitching code that the cell decomposition replaces with one occupancy
  grid.
- Welded vertex graph and generational handles. It makes connectivity
  structural and survives arbitrary wall sliding. It also adds a vertex
  invariant, stale handle repair, and cycle extraction. The shape pool keeps
  walls as segments and adds one invariant (wall to shape), one ordered
  traversal, and one cascade.
- Command-delta undo. More code, more inverses, no benefit at this document
  size.

## Tradeoffs

- Fixed capacities (512 shapes, 512 walls, 256 doors, 128 labels, 64 stairs) are
  enough for a floor plan and keep the format and snapshots simple. Capacity
  failure is reported in the status line.
- The union loses which rectangles formed a shape. The outline and the area
  are what later edits and the file format need.
- A union that removes a wall removes its doors. The count is reported.
- Selection is a bounded set per object kind plus a primary reference. It is
  cleared when an undo changes pool counts, so indices never retarget silently.
  Doors keep stable wall IDs, so no generational handles are needed.
- `./fibs test` covers the pure core only. Canvas behavior is proven by driving
  the real window and screenshotting it.

## Synthesis decision

The base is the shape document above. The earlier design carried painted room
polygons and shape fills; the empty look deleted both rather than keeping a
colourless copy. Rectangle union came from a later request. The two candidates
were a stored rectangle list and an outline rebuilt by clipping. The rebuild won
because the outline is what every other feature already uses: hit testing, edge
dragging, save and load, and door hosts. The cell decomposition made the rebuild
small, exact, and easy to test, and the door re-host rule kept doors on walls
through the rewrite. The evidence is in `README.md`, `docs/editor-ui-spec.md`,
and `tools/verify.sh`.
