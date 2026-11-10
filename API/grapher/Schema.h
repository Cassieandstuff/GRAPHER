#pragma once
/// @file
/// @brief What the nodes and edges of a graph ARE: the data a host hands GRAPHER so it can draw and edit them without
///        knowing the domain. A behavior editor fills it from a class schema; an architecture tool from a JSON file.

#include <grapher/Geometry.h>

#include <string>
#include <string_view>
#include <vector>

namespace grapher {

enum class FieldKind {
    Text,   ///< one line of free text
    Path,   ///< a file or folder on disk
    Url,    ///< a link
    Tag,    ///< a short tag, usually one of `choices` but free entry is allowed
};

struct FieldDef {
    std::string              id;          ///< the key in Node::fields, byte-exact
    std::string              label;       ///< what a person reads
    FieldKind                kind = FieldKind::Text;
    std::vector<std::string> choices;     ///< Tag: the suggested values
    bool                     onCard = true;   ///< drawn on the node's card (an inspector shows every field)
};

enum class NodeShape {
    Card,   ///< a title, a type line, its fields, then the body as a summary
    Note,   ///< a text card: the body fills it
};

struct NodeType {
    std::string           id;            ///< Node::type refers to it, byte-exact
    std::string           label;
    Color                 band{120, 124, 132};   ///< the accent strip; also the type's colour when exported
    float                 width  = 240.f;        ///< a new node's size
    float                 height = 120.f;
    NodeShape             shape  = NodeShape::Card;
    bool                  drillable = false;     ///< it can hold a graph of its own (Node::link)
    std::vector<FieldDef> fields;
    std::string           titleField = "name";   ///< the field the card's title shows; the type label when empty
    std::string           bodyLabel  = "Summary";
};

enum class EdgeLine { Curve, Straight, Elbow };

struct EdgeType {
    std::string id;
    std::string label;
    Color       color{140, 145, 155};
    EdgeLine    line   = EdgeLine::Curve;
    bool        dashed = false;
    bool        arrow  = true;   ///< an arrowhead at the target end
};

struct Schema {
    std::vector<NodeType> nodeTypes;
    std::vector<EdgeType> edgeTypes;   ///< the first is what a newly drawn edge gets

    [[nodiscard]] const NodeType* FindNode(std::string_view id) const;
    [[nodiscard]] const EdgeType* FindEdge(std::string_view id) const;
};

} // namespace grapher
