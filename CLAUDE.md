# GRAPHER — working notes

README.md has the surface and build. Rules:

- **The core is UI-agnostic and dependency-free.** `API/grapher/` and `src/` include only the standard library. Toolkit
  code lives in a backend folder (`qt/`), which only paints `DrawList`s and forwards input to `Canvas`.
- **Data-driven: no domain names in code.** Node and edge types, fields and colours come from the host's `Schema`.
  Anything the canvas needs to know about a type is a schema field, not an `if (type == ...)`.
- **The host owns the graph and its history.** The canvas mutates the graph only between `onBeforeChange` and
  `onChanged`; a click that doesn't move never snapshots.
- **Draw lists are in screen pixels.** Every backend draws the same picture from the same list.
- Names (node ids, type ids, field keys) are byte-exact; never change their case.
- Never set `CMAKE_MSVC_RUNTIME_LIBRARY`; it inherits the consumer's.
- Builds are capped by `Directory.Build.props`; never pass an uncapped `-j`.
- Tests: `tests/grapher_test.cpp`, plain asserts. Add a check for every gesture or draw rule you change.
- Consumers: Architect Designer (`D:\Projects\Architect-Designer`, first). Behavior Kit adopts it later; don't edit BK
  from here.
