# Level editor architecture

## Problem

The editor must reproduce the reference map: orthogonal walls drawn click by click, blue and red room fills, colored doors that live on a wall and slide along it, text labels, and hatched stairs with an adjustable step count. Everything is editable and deletable. The implementation is C with sokol for the window and Dear ImGui for the UI. Two designs from the parallel exploration proposed a welded wall graph with vertex pools, generational handles, cycle walking, and wall sliding. That is a CAD kernel. It solves problems the request does not have, such as moving a wall while keeping its junctions welded. The editor needs to create, edit, color, and delete objects, not to maintain a topological graph under arbitrary wall edits.

## Shape

One pointer-free `Level` document of five typed pools: walls, rooms, doors, labels, stairs. Every pool has a fixed capacity and a live count. A whole document copy is a struct assignment, which makes snapshot undo and load-after-validate trivial.

Walls are independent axis-aligned segments with a monotonic `WallId`. Corners connect visually because the wall tool starts each new segment at the previous click. No vertex pool exists, so a wall cannot dangle and no epsilon weld is needed. `level_add_wall` rejects a zero-length or non-orthogonal segment, so every stored wall is drawable and every door host has a well-defined axis.

Doors store `{ WallId wall, float t, float width, DoorColor color }`. `t` is the center parameter from 0 to 1. The thickness is the `DOOR_THICKNESS` constant for every door, per the request. `level_set_door_span` clamps the span to the host wall. Deleting a wall deletes its doors in the same operation, so a door never points at a dead host.

Rooms are explicit polygons created when a wall chain closes. They are not derived from wall faces. The reference fills are not exact faces, and an explicit polygon keeps its color and identity when nearby walls change. The canvas fills them with `ImDrawList_AddConcavePolyFilled`, which the vendored ImGui 1.93 provides, so no triangulation code exists.

Labels and stairs are independent placed objects. Stairs use the constants `STAIR_STEP_DEPTH`, `STAIR_WIDTH`, and an integer step count, so "standard stairs, adjustable count" is encoded rather than repeated in UI code. Their footprint is a pure function of the object.

Undo is a ring of `Level` snapshots in `History`. The UI commits one snapshot per completed gesture. Command deltas would need a correct inverse for every mutation, and cascade deletes are exactly where inverses are hard. Snapshot undo needs none.

The core (`geom.*`, `level.*`) includes no sokol or ImGui header and builds with gcc alone, so it is tested headlessly. `main.c` owns the sokol lifecycle. `editor.*` owns UI state, gestures, and drawing. The split is enforced by the Makefile: `make test` links only the core.

## Fixed interaction choices

The prototype comparison picked click door placement, automatic room fill on wall-loop close, and cut door rendering. Those are now the only behavior. The Prototype panel and the drag-placement, manual-fill, and overlay-render branches were removed. The Room tool still draws explicit fills for painted areas that do not follow walls.

Selection is UI state in `editor.c`, not part of `Level`. It is five bounded per-kind sets plus a primary reference. Shift-click toggles, a marquee replaces or adds, and Delete removes every selected object in one snapshot. Labels, stairs, and rooms move as a group. Doors keep door-only slide and resize gestures.

Snap applies to placement and to label, stair, room, door, and group drags. Group drags snap from the primary object's anchor so relative spacing is preserved. The Snap checkbox and grid size remain user controls.

## Alternatives rejected

- Welded vertex graph and generational handles. It makes connectivity structural and survives wall sliding. It also adds a vertex invariant, stale handle repair, cycle extraction, and a wall-move rule that the request never exercises. The simpler independent-segment document has one reference invariant (door to wall) and one cascade.
- Rooms derived from wall faces. Fill identity churns when walls change, and the reference fills are not faces.
- Tagged-union entity array. `Room` carries 64 points, so a union charges every door and label about half a kilobyte and every render loop still switches on kind.
- Command-delta undo. More code, more inverses, no benefit at this document size.

## Tradeoffs

- Fixed capacities (512 walls, 64 rooms, 256 doors, 128 labels, 64 stairs) are enough for a floor plan and keep the format and snapshots simple. Capacity failure is reported in the status line.
- Rooms are duplicated geometry rather than derived topology. A room can drift from its walls after wall deletion; that matches the reference, where fills are painted areas.
- Selection is a bounded set per object kind plus a primary reference. It is cleared when an undo changes pool counts, so indices never retarget silently. Doors keep stable wall IDs, so no generational handles are needed.
- `make test` covers the pure core only. Canvas behavior is proven by driving the real window and screenshotting it.

## Synthesis decision

The base is the independent segment document above. Candidate A proposed a welded vertex graph with generational handles, cycle walking, and a wall run slide. Candidate B proposed the same graph with integer centimetre coordinates, typed wall and room ids, and rooms stored as wall cycles. Both are sound CAD designs, and both solve wall dragging and splitting at crossings. The request asks for drawing, coloring, doors on walls, labels, stairs, and deletion. It never asks to drag a wall or split one at a crossing. The graph would add a vertex invariant, stale handle repair, and a cycle query that no requested gesture uses. The simpler document has one cross-object invariant, the door host, and one cascade on wall delete.

From candidate A we kept stable wall ids, the door host reference, the delete cascade, whole-document snapshots, the all-or-nothing load, and the fixed capacity constants. From candidate B we kept the typed pools, one gesture union, and the discovery that ImGui 1.93 has `ImDrawList_AddConcavePolyFilled`, which removes the triangulation A proposed. We rejected the vertex pool, generational handles, integer coordinates, and rooms as wall cycles.

The prototype comparison picked Click for door placement, Auto for room fill, and Cut for door render. Those choices are now fixed, the panel is gone, and multi-selection plus grid snapping sit on top of the same document model. The evidence is in `README.md` and `tools/verify.sh`.
