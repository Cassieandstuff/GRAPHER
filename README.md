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
| `grapher/Groups.h` | group membership: a frame group holds what lies inside it, a member group what names it as `parent` (`ParentGroup`, `GroupMembers`) |
| `grapher/Dial.h` | a dial's geometry: `DialAxis` (cyclic or open), its rim (`Ring`: a circle or an octagon), `DialOf` (an octagon dial's body, header and listed rows) |
| `grapher/Search.h` | `Search`: nodes by their text (titles, ids, type names, rows, fields, bodies), best first |
| `grapher/Layout.h` | `LayoutColumns` (columns by wire distance, few crossings) and `LayoutGraph` (inside out through member groups, kept nodes stay, open groups push) |
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

**Rows** are card lines the host fills (`Node::rows`): a label and a value, styled by a `RowStyle` from the schema
(colour, value colour, prefix, bold, italic), with a rule above when `separator` is set. A row may carry a port's pin
(`Row::port`), so a list of pointers reads as a list of rows each wired from its own line. A row can act: `Click` and
`Button` rows are handed to the host (`Canvas::TakeRowClick`, the Qt widget's `rowClicked`), and a `Choice` row offers
its `choices` and sets a node field (`PickChoice`; the Qt widget pops the menu itself). Rows and nodes carry tooltips
(`TooltipAt`). A node type with an `imageHeight` shows `Node::image` at the foot of the card: the draw list's `Image`
command names it by key and the backend resolves it (`CanvasWidget::setImageSource`).

**Two edit modes.** In `EditMode::Direct` (the default) the canvas edits the host's graph between `onBeforeChange`
and `onChanged`. In `EditMode::Propose` it never touches it: a gesture plays out on a copy and arrives at `onIntent` as
an `Intent` (`MoveNodes`, `ConnectWire` with the wires it replaces, `RemoveItems`, `CreateNode`, `SetValue`); the
host applies it through its own history, or doesn't, and calls `Refresh`. A wire let go over empty canvas arrives as
`ConnectToNothing` in either mode. `CancelGesture` (Esc) undoes a gesture under way.

**Navigation and selection** are the host's to shape: `PanBy`, `ZoomAbout` (within `minZoom`..`maxZoom`), `Frame`
(some nodes or all), `CenterOn`, `EnsureVisible`, and `panWithButtons = false` when the host reads the middle button
and wheel itself (`CanvasWidget::setHostNavigation`). `SelectKeys::ExtendSubtract` makes Shift add and Ctrl take away;
the default toggles. `CanvasWidget::setOwnKeys(false)` leaves every key to the host's commands.

**Member groups** (`GroupMode::Members`) hold the nodes whose `Node::parent` names them, at any depth, and their frame
is drawn around what they hold, below the header. They open and close (the arrow at the start of the header, or a
double-click on it in the Qt widget; `SetCollapsed`, `SetAllCollapsed`, `Reveal`): a closed group is just its header,
what it holds is not drawn, and the wires into it end on it, several joining the same ends merged into one dashed wire.
Open and closed is the canvas's view state, never an edit (`onCollapseChanged` tells the host). Dragging a member group
carries what it holds; a node dropped on a member group joins the innermost one under the pointer, and dropped on none
leaves its own (an edit: `MoveNodes::parents` when proposed). A header can show how many nodes the group holds
(`showCount`). An alias (`Node::aliasOf`) is a node standing in for another where that one is also reached: it draws
dimmed with a dashed outline, and a click on it selects the original and opens the groups around it.
`HoldInPlace` keeps a node's header still on screen through the host's next `Refresh`, so laying the graph out again
after an edit doesn't move the view. `LayoutGraph` is the layout to run then.

**Docks.** A node can dock to another (`Node::dock`): on one of its sides, in a list that reads outward (children
stacked under a parent), beside one of its rows (a card hanging off a line), or flush (a member group headed by a card).
A docked node's place is worked out from its parent's and travels with it, docked nodes of its own included. With
`Canvas::docking` on, dragging a docked node away undocks it where it is let go, and dragging a node onto a slot beside
a parent it may dock to (`canDock`; by default, one wiring to it) docks it there, the slot shown while it drags and the
side renumbered (an edit: `MoveNodes::docks` when proposed). Hovering a card docked beside a row washes that row and
rings what its wires enter. `Node::showWith` shows a node only while it, or one of the nodes it names, is selected.

**Dials** (`NodeShape::Dial`) are a radial layout: each row that claims a port stands on the ring at the angle its value
has on the axis (`Node::dial`: cyclic at true angles, or open on a 270° arc with the rows evenly spaced), its pin
leaving outward. A needle shows `dial.value`. A handle per row drags its value round the ring, Shift snapping to
sixteenths, and sets it like a choice (`SetValue`).
A kit's rim part may be `"shape": "octagon"` (with `"chamfer"`, the corners' cut as a fraction): then the node is that
octagon, its header band on the top edge and its other rows (`dial.rim` names those on the rim) listed under it with
their pins on its edges; a row stands where the ray at its angle meets the rim, its pin leaving square to the edge and
its handle dragging along the edges, and an open dial's gap is the flat bottom edge, where the needle's reading sits.

**Wire styles.** An edge type can be dotted and set its width. With a canvas, a wire keeps the sides it leaves and enters
by until its ends have moved well past where they would turn (`RoutingOptions::sideHysteresis`), so dragging along the
middle doesn't flicker. A row style can be a chip (a rounded patch behind its label).

**Highlights** are what a host lights over the graph without editing it (`Canvas::highlights`): a glow behind nodes
and wires (a closed group glows with the brightest thing it holds, a merged wire with the brightest it stands for),
coloured rings, dimmed nodes, and arrows between nodes (a dashed arc whose head fills with `progress`, a flash, a loop
on one node). `Style::highlightSelectedWires` draws the wires of a selected node in the selection colour, and an edge
type `onTop` draws over the nodes.

**Minimap and search.** `Canvas::DrawMinimap` draws the whole graph small (a graph too big to read whole keeps a
readable scale and follows the view), and `MinimapToWorld` / `CenterOnWorld` move the view from it; the Qt widget shows
it in the bottom right (`setMinimapShown`). `grapher::Search` ranks nodes by their text; the Qt widget's search box
(`openSearch`, or a host's own ranking through `setSearchFunction`) walks the hits, opening the groups around each and
ringing the rest, keeps the hit on Enter and goes back on Esc. `addOverlay` keeps a host's widgets over a corner.

**Host hooks for richer editors.** `Canvas::checkWire` gives the host a say on each wire after the schema's (refuse a
cycle; mark a converting wire with its own edge type or a label). `wholeNodeWires = false` makes every wire run port to
port. A node type with `fitHeight` grows its cards to show all their ports and rows (a port count that is data). A port
can carry its own `tooltip`. A reroute dot is grabbed by its middle, its pins by its rim, and has no resize grip.

**Large graphs.** The canvas keeps its routes, per-node and per-edge tables until a frame, port, edge or option
changes, culls what is off screen, and hit-tests only what is near the pointer. On a graph of more than 2000 nodes a
drag reroutes only the dragged nodes' wires, and past 400 obstacles each wire that needs a detour routes on a grid of
the nodes around it. `grapher_test` holds a 20,000-node case with its budgets (an optimised build: a frame ≤ 4 ms with
routes cached, a hit test ≤ 0.2 ms, a drag step ≤ 8 ms; panning and zooming never reroute).

| `grapher/DrawList.h` | what a backend paints: rects, polylines, polygons, circles, text, in screen pixels |
| `grapher/Style.h` | background, grid and card chrome |
| `grapher/Kit.h` | kits: node kinds as part trees (header, body, accent, text lines with formats; a group's title bar and its closed pill with end pins; a pill's glyphs, bars, chips and stubs; a dial's rim), pin kinds (by port type, or the kind a `PortType::pin` names), row kinds (layout, an editor, list reordering) and edge kinds (line, width, dash, arrows, colour), colour tokens, palettes (`palette(name, tag:key)`: an entry a node's tag picks) and a card's `band` expression; prefix kinds (`card:*`) that stand for the types a host makes from data, the longest match winning; rules by selector (kind, `Node::tags` or `Edge::tags`, state, `pin:` / `row:` / `edge:` parts), gestures bound to host actions (`InvokeAction`), as JSON (`ParseKit`, `WriteKit`); `Canvas::SetKit` draws with one, and with none draws exactly as before (`KitFromSchema`) |
| `grapher/qt/CanvasWidget.h` | the Qt backend: `CanvasWidget` and `Paint(QPainter&, DrawList)` |

Include only `<grapher/Grapher.h>` (plus a backend header).

## How a host uses it

1. Build a `grapher::Schema` from its own data (a class schema, a JSON file).
2. Own a `grapher::Graph` and a `grapher::Canvas` over it.
3. Wire `onBeforeChange` (snapshot for undo), `onChanged` (save, repaint), `onSelectionChanged`.
4. Feed pointer events to the canvas and paint `canvas.Draw()` — or use `grapher::qt::CanvasWidget`, which does both.
5. Say how it looks with a kit (`Canvas::SetKit`): a JSON file the host ships. `schema/kit.schema.json` is the kit
   format as a JSON Schema (a kit may name it as its `"$schema"`, for an editor's completion and checks).

### An example: a material graph

`examples/material/` is the pattern for a host with typed values: `material.schema.json` (SchemaJson) declares the value
types (float, vec2, vec3, colour, texture) and the conversions between them, and node types by family (`input:float`,
`math:add`, `texture:sample`, `output:surface`); `material.kit.json` gives each value type a pin kind (its colour and
glyph), each family one prefix kind (`input:*`, `math:*`) with its header band, a frame its colour from a palette by its
`frame:` tag, parameter rows their editors (slider, colour, path) and wires the colour of the type they carry
(`pinColor`). The host fills only content: a node's title and rows, and its tags.

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
