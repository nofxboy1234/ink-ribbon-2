# Level editor architecture

## Problem

The editor must reproduce the reference map: orthogonal walls drawn click by click, a steel-blue floor with black holes cut out and red rooms, colored doors that live on a wall and slide along it, text labels, and hatched stairs with an adjustable step count. Everything is editable and deletable. The implementation is C with sokol for the window and Dear ImGui for the UI. Two designs from the parallel exploration proposed a welded wall graph with vertex pools, generational handles, cycle walking, and wall sliding. That is a CAD kernel. It solves problems the request does not have, such as moving a wall while keeping its junctions welded. The editor needs to create, edit, color, and delete objects, not to maintain a topological graph under arbitrary wall edits.

## Shape

One pointer-free `Level` document of six typed pools: shapes, walls, rooms, doors, labels, stairs. Every pool has a fixed capacity and a live count. A whole document copy is a struct assignment, which makes snapshot undo and load-after-validate trivial.

A shape is one drawn wall chain: a `Shape` record with a closed flag and a fill colour, plus the walls that belong to it. Walls stay independent axis-aligned segments with a monotonic `WallId` and a `ShapeId`. Wall order inside a shape is the order the walls were added, so the loop polygon is a traversal, not a second geometry copy. There is no vertex pool: corners meet because the Line tool starts each segment at the previous endpoint, and a reshape carries every endpoint that coincides with the moved line. `level_add_wall` rejects a zero-length or non-orthogonal segment, so every stored wall is drawable and every door host has a well-defined axis.

Doors store `{ WallId wall, float t, float width, DoorColor color }`. `t` is the center parameter from 0 to 1. The thickness is the `DOOR_THICKNESS` constant for every door, per the request. `level_set_door_span` clamps the span to the host wall. Deleting a wall deletes its doors in the same operation, so a door never points at a dead host.

A closed shape carries its own fill and the fill polygon is the shape's wall loop. The fills are Blue, Red, Floor Blue, Floor Red, Hole, and Empty. Floor Blue paints in a first pass under every room, so a later Hole cuts to the canvas and a red room can sit over it. Empty draws no fill, only a faint wall outline. Standalone Room polygons still load for older levels and painted areas that do not follow walls; the editor no longer creates them. Both are filled by `polygon_triangulate` in `geom.c`, an ear-clipping pass over the polygon, and drawn as triangles. The vendored ImGui `ImDrawList_AddConcavePolyFilled` renders the bounding rectangle for some valid polygons, including a plus-shaped loop, so it is not used for fills.

Labels and stairs are independent placed objects. Stairs use the constants `STAIR_STEP_DEPTH`, `STAIR_WIDTH`, and an integer step count, so "standard stairs, adjustable count" is encoded rather than repeated in UI code. Their footprint is a pure function of the object.

Undo is a ring of `Level` snapshots in `History`. The UI commits one snapshot per completed gesture. Command deltas would need a correct inverse for every mutation, and cascade deletes are exactly where inverses are hard. Snapshot undo needs none.

The core (`geom.*`, `level.*`) includes no sokol or ImGui header and builds with gcc alone, so it is tested headlessly. `main.c` owns the sokol lifecycle. `editor.*` owns UI state, gestures, and drawing. The split is enforced by the Makefile: `make test` links only the core.

## Fixed interaction choices

The shipped UI has four tools: Select, Line, Label, and Room, with Line as the default. The prototype comparison picked context-menu door placement, automatic fill when a wall loop closes, and cut door rendering. The chosen room design drags an axis-aligned rectangle, stores it as a closed shape, and takes its fill from one Fill palette. The palette also feeds the Line tool when it closes a loop. A right-click Mode submenu changes a room after creation, and Alt+drag copies one. The other prototype, which detected wall faces and painted a clicked region, was removed with its region grid and Editor fields. The document still loads standalone Room polygons from older files.

Selection is UI state in `editor.c`, not part of `Level`. It is five bounded per-kind sets plus a primary reference. Walls are selected as edges, and hitting a wall selects that edge; clicking inside a closed shape selects all of its edges. Shift-click adds, Ctrl-click toggles, a marquee replaces or adds, and Delete removes every selected object in one snapshot. A drag has two modes. When the selection is edges that are not exactly a whole shape, `GESTURE_RESHAPE` slides each selected edge along its own normal through `level_slide_edges`, so corners between two selected edges take both axes and unselected neighbours stretch. When the selection mixes kinds or is exactly a whole shape, `GESTURE_MOVE` translates the selected walls with `level_move_walls` and moves rooms, labels, and stairs. A door stores a wall id and a normalized position, so it follows its host wall and cannot be translated independently. A door dragged alone keeps its slide and end-resize gestures.

Snap applies to placement, reshaping, and label, stair, room, door, and group drags. Group drags snap from the primary object's anchor so relative spacing is preserved. The Snap checkbox and grid size remain user controls.

## Alternatives rejected

- Welded vertex graph and generational handles. It makes connectivity structural and survives arbitrary wall sliding. It also adds a vertex invariant, stale handle repair, and cycle extraction. The shape pool keeps walls as segments and adds one invariant (wall to shape), one ordered traversal, and one cascade.
- Rooms derived from wall faces. Fill identity churns when walls change, and the reference fills are not faces.
- Tagged-union entity array. `Room` carries 64 points, so a union charges every door and label about half a kilobyte and every render loop still switches on kind.
- Command-delta undo. More code, more inverses, no benefit at this document size.

## Tradeoffs

- Fixed capacities (512 shapes, 512 walls, 64 rooms, 256 doors, 128 labels, 64 stairs) are enough for a floor plan and keep the format and snapshots simple. Capacity failure is reported in the status line.
- Shape fills are derived from the shape's walls, so they follow a reshape. Floor Blue is an underlay pass, so holes always cut it regardless of creation order. Standalone rooms are painted and can drift; that matches the reference, where some fills are painted areas.
- Selection is a bounded set per object kind plus a primary reference. It is cleared when an undo changes pool counts, so indices never retarget silently. Doors keep stable wall IDs, so no generational handles are needed.
- `make test` covers the pure core only. Canvas behavior is proven by driving the real window and screenshotting it.

## Synthesis decision

The base is the shape document above. The earlier independent-segment design dropped wall dragging on the argument that the request never asked for it. A later request did: walls must form one shape with draggable lines. The shape pool answers that by grouping existing segments and adding one slide rule. A welded graph would still be the wrong size: it adds a vertex invariant, stale handle repair, and a cycle query, while the request needs one gesture.

From candidate A we kept stable wall ids, the door host reference, the delete cascade, whole-document snapshots, the all-or-nothing load, and the fixed capacity constants. From candidate B we kept the typed pools and one gesture union. We rejected the vertex pool, generational handles, integer coordinates, and rooms as wall cycles. The triangulation A proposed came back after `ImDrawList_AddConcavePolyFilled` was measured filling a plus loop's bounding rectangle; `geom.c` now ear-clips.

The prototype comparison picked Click for door placement, Auto for room fill, and Cut for door render. Those choices are now fixed, the panel is gone, and multi-selection plus grid snapping sit on top of the same document model. The evidence is in `README.md` and `tools/verify.sh`.
