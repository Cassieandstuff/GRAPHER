# GRAPHER

C++23 data-driven graph canvas. Hand it a schema of node and edge types and a graph, and it draws and edits it. The
core knows no UI toolkit; backends paint its draw lists.

| Header | Contents |
|---|---|
| `grapher/Schema.h` | `NodeType` (fields, shape: card / note / pill, colour, drillable, default edge), `EdgeType` (line, dash, arrow), `Schema` |
| `grapher/Graph.h` | `Node`, `Edge`, `Graph` — a value type, so undo is a copy |
| `grapher/Canvas.h` | `Camera`, `Canvas` — gestures (select, move, resize, connect, marquee, pan, zoom) and `Draw()` |
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
