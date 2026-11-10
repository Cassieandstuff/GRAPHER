# GRAPHER

C++23 data-driven graph canvas. Hand it a schema of node and edge types and a graph, and it draws and edits it. The
core knows no UI toolkit; backends paint its draw lists.

| Header | Contents |
|---|---|
| `grapher/Schema.h` | `NodeType` (fields, shape: card / note / pill / group, colour, drillable, default edge), `EdgeType` (line, dash, arrow), `Schema` |
| `grapher/Graph.h` | `Node`, `Edge`, `Graph` — a value type, so undo is a copy |
| `grapher/Groups.h` | group membership: a node belongs to the smallest group whose frame holds it (`ParentGroup`, `GroupMembers`) |
| `grapher/Routing.h` | `RouteWires` — ports spread along node sides; wires that would cut through a node routed around it (orthogonal A*), drawn in their edge type's style |
| `grapher/Canvas.h` | `Camera`, `Canvas` — gestures (select, move, resize, connect, marquee, pan, zoom) and `Draw()` |

**Groups** (`NodeShape::Group`) are frames that hold nodes, as JSON Canvas groups do: membership is geometric, so they
nest. A group draws behind the edges and nodes, with its title (and its other on-card fields) in a header strip. Its
header, resize grip and connect handle pick it; its empty interior is empty canvas, so a press there starts a marquee
(which takes a group only when it holds all of it). Dragging a group moves everything it holds; a node dropped or added
with its centre in a group's frame slides wholly inside it (when it fits), and dragging its centre out takes it out.
Neither router treats a group as an obstacle: wires run over it, and a wire can end on its outline.

| `grapher/DrawList.h` | what a backend paints: rects, polylines, polygons, circles, text, in screen pixels |
| `grapher/Style.h` | background, grid and card chrome |
| `grapher/qt/CanvasWidget.h` | the Qt backend: `CanvasWidget` and `Paint(QPainter&, DrawList)` |

Include only `<grapher/Grapher.h>` (plus a backend header).

## How a host uses it

1. Build a `grapher::Schema` from its own data (a class schema, a JSON file).
2. Own a `grapher::Graph` and a `grapher::Canvas` over it.
3. Wire `onBeforeChange` (snapshot for undo), `onChanged` (save, repaint), `onSelectionChanged`.
4. Feed pointer events to the canvas and paint `canvas.Draw()` — or use `grapher::qt::CanvasWidget`, which does both.

## Consuming (FetchContent)

```cmake
set(GRAPHER_QT ON CACHE BOOL "" FORCE)   # only if you want the Qt backend; you provide Qt6
FetchContent_Declare(GRAPHER GIT_REPOSITORY https://github.com/Cassieandstuff/Grapher.git GIT_TAG main)
FetchContent_MakeAvailable(GRAPHER)
target_link_libraries(MyTarget PRIVATE grapher::grapher grapher::qt)
```

Local checkout: set `FETCHCONTENT_SOURCE_DIR_GRAPHER` to it (a CMakeUserPresets entry).

## Build and test (core only, no dependencies)

```
cmake --preset dev
cmake --build --preset debug
ctest --preset debug
```

`Directory.Build.props` caps the compiler at half the machine's cores (`BUILD_CORES` overrides).

## libavoid routing backend (optional)

`GRAPHER_AVOID=ON` (preset `avoid`) builds `grapher::avoid`: `grapher::avoid::MakeRouter()` returns a `WireRouter` a
canvas uses through `canvas.router`. It routes with [libavoid](https://github.com/mjwybrow/adaptagrams) (LGPL-2.1+):
orthogonal routes around nodes, parallel runs nudged apart and ordered so they nest, pins on every side so ends spread,
and incremental rerouting as nodes move. Without it the canvas uses the built-in `RouteWires`.

libavoid is fetched at a pinned commit and built as `avoid.dll`, with one change, `avoid/patches/export-vertid.cmake`
(exports `VertID` so its headers work across the DLL boundary). A program using it ships `avoid.dll` and
`GRAPHER_AVOID_LICENSE` (libavoid's LGPL text) beside itself, and must not prevent replacing the DLL. `avoid/compat/afx.h`
stands in for MFC, which libavoid's MSVC debug assertions would otherwise need.
