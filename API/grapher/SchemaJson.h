#pragma once
/// @file
/// @brief A Schema as JSON: the one file format every host shares, so node, port and edge types can be written as data
///        and defined at runtime (a material node library, a shader graph's nodes). The format:
///
/// ```json
/// {
///   "subtypeFlow": "out-is-a",                  // or "in-is-a" (Schema::subtypeFlow)
///   "portTypes": [ { "id": "float", "label": "Float", "color": "#a0a0a0", "glyph": "circle",
///                    "bases": ["number"], "edge": "data" } ],
///   "conversions": [ { "from": "float", "to": "rgba" } ],
///   "rowStyles": [ { "id": "key", "color": "#dcb43c", "valueColor": "#8c919b", "prefix": "+ ", "bold": true, "italic": false } ],
///   "nodes": [ { "id": "mix", "label": "Mix", "band": "#4f8cff", "width": 240, "height": 120,
///                "shape": "card", "drillable": false, "titleField": "name", "bodyLabel": "Summary",
///                "defaultEdge": "data", "imageHeight": 0, "imagePlacement": "foot", "imageMinZoom": 0, "groupMode": "frame", "showCount": false,
///                "fields": [ { "id": "amount", "label": "Amount", "kind": "text", "choices": [], "onCard": true } ],
///                "ports":  [ { "id": "a", "label": "A", "type": "rgba", "dir": "in", "side": "auto",
///                              "maxWires": 1, "field": "", "onHeader": false, "typeFrom": "" } ] } ],
///   "edges": [ { "id": "data", "label": "data", "color": "#8c919b", "line": "curve", "dashed": false, "arrow": true,
///                "portColor": false, "dotted": false, "width": 1.5 } ]
/// }
/// ```
///
/// Every key but an id is optional and defaults as the structs do; a label defaults to its id. Names are byte-exact.
/// Enumerations are spelt in lower case: shape card | note | pill | group | dot | dial; kind text | path | url | tag; line curve |
/// straight | elbow; glyph circle | square | diamond | arrow; dir in | out; side auto | left | right | top | bottom;
/// groupMode frame | members.

#include <grapher/Schema.h>

#include <expected>
#include <string>
#include <string_view>

namespace grapher {

/// @return the schema, or why the text isn't one (with the line and column for bad JSON)
[[nodiscard]] std::expected<Schema, std::string> ParseSchema(std::string_view json);

/// @brief One node type, as an entry of "nodes" spells it: a type defined at runtime (Schema::PutNodeType adds it).
[[nodiscard]] std::expected<NodeType, std::string> ParseNodeType(std::string_view json);

/// @brief The schema as JSON that ParseSchema reads back the same; keys at their default are left out.
[[nodiscard]] std::string WriteSchema(const Schema& schema);

} // namespace grapher
