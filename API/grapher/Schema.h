#pragma once
/// @file
/// @brief What the nodes and edges of a graph ARE: the data a host hands GRAPHER so it can draw and edit them without
///        knowing the domain. A behavior editor fills it from a class schema; an architecture tool from a JSON file.

#include <grapher/Geometry.h>
#include <grapher/Port.h>

#include <cstdint>
#include <optional>
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
    friend bool operator==(const FieldDef&, const FieldDef&) = default;
};

enum class NodeShape {
    Card,   ///< a title, a type line, its fields, then the body as a summary
    Note,   ///< a text card: the body fills it
    Pill,   ///< a small capsule that edges run through: its on-card field values on one line, joined by " · "
    /// @brief A frame that holds other nodes, drawn behind them with its title in a header strip. Membership is
    ///        geometric (grapher/Groups.h): moving the group moves what it holds, and wires pass over it.
    Group,
    /// @brief A small round node with no text, its ports on its outline: a reroute point a wire passes through.
    Dot,
    /// @brief A round card whose rows stand around its rim by value (Node::dial): a radial layout, with a handle per row
    ///        to drag its value round the ring.
    Dial,
};

/// @brief What a group holds.
enum class GroupMode {
    Frame,     ///< the nodes inside its frame, as JSON Canvas groups do; moving a node in or out changes it
    Members,   ///< the nodes whose Node::parent names it; its frame wraps them below its header, and it can collapse
};

/// @brief How a kind of row looks: a key's colour, a binding's prefix, a muted unset value.
struct RowStyle {
    std::string           id;
    std::optional<Color>  color;        ///< the label's; unset = the style's text colour
    std::optional<Color>  valueColor;   ///< the value's; unset = muted
    std::string           prefix;       ///< put before the label ("+ ", "← ")
    bool                  bold   = false;
    bool                  italic = false;
    bool                  chip   = false;   ///< a rounded patch of its colour behind the label
    friend bool operator==(const RowStyle&, const RowStyle&) = default;
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
    std::string           defaultEdge;   ///< an EdgeType id a new edge to or from this type gets; empty = the schema's first
    std::vector<PortDef>  ports;         ///< every node of the type has these; a node's own ports follow (Node::ports)
    float                 imageHeight = 0.f;   ///< room for Node::image at the foot of a card, world units; 0 = none
    GroupMode             groupMode = GroupMode::Frame;   ///< a group's: what it holds
    bool                  showCount = false;   ///< a group's header says how many nodes it holds, nested ones included
    friend bool operator==(const NodeType&, const NodeType&) = default;
};

enum class EdgeLine { Curve, Straight, Elbow };

struct EdgeType {
    std::string id;
    std::string label;
    Color       color{140, 145, 155};
    EdgeLine    line   = EdgeLine::Curve;
    bool        dashed = false;
    bool        arrow  = true;   ///< an arrowhead at the target end
    /// @brief A wire of this type takes the colour of the type its Out port carries (PortTypeOf), when it has one.
    bool        portColor = false;
    bool        dotted    = false;   ///< drawn dotted (wins over dashed)
    bool        onTop     = false;   ///< drawn over the nodes (a wire between two groups that must stay readable)
    float       width     = 1.5f;    ///< the line's width at zoom 1, in pixels
    friend bool operator==(const EdgeType&, const EdgeType&) = default;
};

struct Schema {
    std::vector<NodeType>   nodeTypes;
    std::vector<EdgeType>   edgeTypes;   ///< the first is what a newly drawn edge gets, unless an end's type says otherwise
    std::vector<PortType>   portTypes;
    std::vector<Conversion> conversions;
    std::vector<RowStyle>   rowStyles;
    SubtypeFlow             subtypeFlow = SubtypeFlow::OutIsA;
    /// @brief Bump after changing the types in place (a type defined at runtime), so a canvas drops what it cached.
    ///        PutNodeType bumps it.
    std::uint64_t           revision = 0;

    [[nodiscard]] const NodeType* FindNode(std::string_view id) const;
    [[nodiscard]] const EdgeType* FindEdge(std::string_view id) const;
    [[nodiscard]] const PortType* FindPortType(std::string_view id) const;
    [[nodiscard]] const RowStyle* FindRowStyle(std::string_view id) const;
    /// @brief The edge type a new edge between these node types gets: the source's default, else the target's, else the
    ///        first edge type; empty when there are none.
    [[nodiscard]] std::string EdgeFor(std::string_view fromType, std::string_view toType) const;

    /// @brief Whether port type `type` is `base`: the same, or `base` is among its `bases`, at any depth.
    [[nodiscard]] bool IsA(std::string_view type, std::string_view base) const;
    /// @brief Whether a wire may run from an Out port of type `out` to an In port of type `in`: an untyped end takes
    ///        anything; else the types are the same, or one is the other by `subtypeFlow`, or a Conversion says so.
    [[nodiscard]] bool CanConnect(std::string_view out, std::string_view in) const;

    /// @brief Add a node type, or replace the one with its id (a type defined at runtime from data); bumps `revision`.
    void PutNodeType(NodeType type);

    friend bool operator==(const Schema&, const Schema&) = default;
};

} // namespace grapher
