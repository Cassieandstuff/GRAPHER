# GRAPHER

C++23 data-driven graph canvas. Hand it a schema of node and edge types and a graph, and it draws and edits it. The
core knows no UI toolkit; backends paint its draw lists.

| Header | Contents |
|---|---|
| `grapher/Schema.h` | `NodeType` (fields, ports, shape: card / note / pill / group, colour, drillable, default edge), `EdgeType` (line, dash, arrow), `Schema` (with port types, conversions, `CanConnect`) |
| `grapher/Port.h` | `PortType` (colour, glyph, bases), `PortDef` (direction, side, wire limit, the field it shows), `Conversion` |
| `grapher/Ports.h` | where a node's ports sit (`LayoutPorts`), the height a card needs (`CardHeight`) |
| `grapher/SchemaJson.h` | the schema as JSON: `ParseSchema`, `ParseNodeType` (a type defined at runtime), `WriteSchema` |
| `grapher/Graph.h` | `Node` (with ports of its own), `Edge` (port to port, or node to node), `Graph` — a value type, so undo is a copy |
| `grapher/Groups.h` | group membership: a node belongs to the smallest group whose frame holds it (`ParentGroup`, `GroupMembers`) |
| `grapher/Routing.h` | `RouteWires` — ports spread along node sides; wires that would cut through a node routed around it (orthogonal A*), drawn in their edge type's style |
| `grapher/Canvas.h` | `Camera`, `Canvas` — gestures (select, move, resize, connect, marquee, pan, zoom) and `Draw()` |

**Groups** (`NodeShape::Group`) are frames that hold nodes, as JSON Canvas groups do: membership is geometric, so they
nest. A group draws behind the edges and nodes, with its title (and its other on-card fields) in a header strip. Its
header, resize grip and connect handle pick it; its empty interior is empty canvas, so a press there starts a marquee
(which takes a group only when it holds all of it). Dragging a group moves everything it holds; a node dropped or added
with its centre in a group's frame slides wholly inside it (when it fits), and dragging its centre out takes it out.
Neither router treats a group as an obstacle: wires run over it, and a wire can end on its outline.

**Ports** are the named, typed places a wire connects to. A node type declares them in the schema (`NodeType::ports`),
and a node can add its own (`Node::ports`: a list whose length is data, a block with a variable number of inputs). A
wire runs from an Out port to an In port (`Edge::fromPort` / `toPort`); an edge with no ports joins the nodes as a
whole, as before. Whether a wire may run is the schema's to say, from data: the port types are the same; or one is the
other through its `bases` (`subtypeFlow` says which end may be the more specific: a value flows into a port of its own
type or a base of it, while a pointer of a class takes a node of that class or a subclass); or a listed `Conversion`
allows it. An In port takes one wire unless `maxWires` says otherwise, and a new wire into a full port replaces the
oldest. On a card, left and right ports stack in rows under the header (an input and an output can share a row), a port
`onHeader` sits on the title line, and top and bottom ports spread along their side; a port bound to a `field` shows its
value while unwired. Drag from a pin to a pin, or onto a node to land on its first port that takes the wire; while you
drag, the ports it may land on are ringed. A node with ports has no connect handle. Both routers end wires on the pins.

The schema can be written as JSON (`grapher/SchemaJson.h` documents the format), so node types can come from data at
runtime: parse one with `ParseNodeType` and add it with `Schema::PutNodeType`.

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
FetchContent_Declare(GRAPHER GIT_REPOSITORY https://github.com/Cassieandstuff/GRAPHER.git GIT_TAG main)
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
