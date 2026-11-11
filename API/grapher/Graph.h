#pragma once
/// @file
/// @brief The document GRAPHER edits: nodes with a type, a frame, named fields and a free text body, and the typed
///        edges between them. A value type, so a host can snapshot it for undo by copying.

#include <grapher/Geometry.h>
#include <grapher/Port.h>

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace grapher {

/// @brief What a row does when clicked.
enum class RowAction {
    None,     ///< nothing: a click on it is a click on its node
    Click,    ///< the host acts on it (Canvas::TakeRowClick)
    Button,   ///< drawn as a button at its right end; the host acts on it
    Choice,   ///< offers `choices`; picking one sets `field` (Canvas::PickChoice)
};

/// @brief One line of a node's card that the host fills: a fact (label and value), a line a port's pin sits on, a
///        button, a choice. A node's rows follow its free port rows and come before its on-card fields.
struct Row {
    std::string              id;      ///< unique on its node, byte-exact
    std::string              label;   ///< on the left
    std::string              value;   ///< on the right
    std::string              style;   ///< a RowStyle id; empty = plain
    std::string              port;    ///< a port of the node whose pin sits on this line (its row is this one)
    RowAction                action = RowAction::None;
    std::vector<std::string> choices;   ///< Choice: what a pick offers
    std::string              field;     ///< Choice: the node field a pick sets
    std::string              tooltip;
    bool                     separator = false;   ///< a faint rule above it
    friend bool operator==(const Row&, const Row&) = default;
};

/// @brief Where a node sits against another: on one of its sides, in an ordered list that reads outward (a child
///        stacked under its parent), or beside one of its rows (a card hanging off a line). A docked node's place is
///        derived from its parent's and travels with it; dragging it away undocks it, dropping it on a slot docks it.
struct Dock {
    std::string parent;                   ///< the node it docks to; empty = not docked
    PortSide    side  = PortSide::Bottom; ///< Auto counts as Bottom
    int         order = 0;                ///< its place on the side, nearest first
    std::string row;                      ///< a row of the parent to sit beside (side Left or Right) instead of the list
    bool        flush = false;            ///< no gap: it touches its parent (a group headed by a card)
    friend bool operator==(const Dock&, const Dock&) = default;
};

/// @brief A ring a node's rows stand around: each row that claims a port sits on the rim at the angle its numeric value
///        has on the axis. A cyclic axis goes round the whole circle at true angles between `min` and `max`; an open one
///        lies on a 270° arc with its gap at the bottom, its rows evenly spaced in order and the axis linear between
///        neighbours. A needle shows `value`.
struct Dial {
    bool        cyclic = false;
    float       min = 0.f, max = 1.f;   ///< a cyclic axis' range
    float       value = 0.f;            ///< where the needle points
    std::string label;                  ///< under the needle
    friend bool operator==(const Dial&, const Dial&) = default;
};

struct Node {
    std::string id;     ///< unique in its graph, stable across saves
    std::string type;   ///< a NodeType id; an unknown one draws neutral
    Rect        frame;  ///< world units
    std::map<std::string, std::string, std::less<>> fields;
    std::string body;   ///< free text (a summary, pseudo-code, notes)
    std::string link;   ///< where the node's own graph lives (host-defined); empty = none yet
    /// @brief Ports of this node alone, after its type's (a list's elements, a block's extra inputs). One with the id of
    ///        a type port replaces it in place.
    std::vector<PortDef> ports;
    std::vector<Row>     rows;      ///< card lines the host fills (see Row)
    std::string          tooltip;   ///< shown over the node where no row has one of its own
    std::string          image;     ///< a picture the backend resolves by this key (a thumbnail); see NodeType::imageHeight
    /// @brief The member group (GroupMode::Members) that holds this node; empty = the top level, or a frame group's
    ///        geometry.
    std::string          parent;
    /// @brief This node stands in for node `aliasOf` where it is also shown (a node reached from far away): drawn
    ///        dimmed with a dashed outline, and a click on it goes to the original.
    std::string          aliasOf;
    Dock                 dock;
    /// @brief Shown only while this node or one of these is selected (a card belonging to a busy hub); empty = always.
    std::vector<std::string> showWith;
    Dial                 dial;      ///< a NodeShape::Dial node's axis and needle

    /// @return the row `id`, or nullptr
    [[nodiscard]] const Row* FindRow(std::string_view rowId) const;
    /// @return the field's value, or an empty string
    [[nodiscard]] const std::string& Field(std::string_view key) const;
    friend bool operator==(const Node&, const Node&) = default;
};

struct Edge {
    std::string id;
    std::string from;   ///< a Node id
    std::string to;     ///< a Node id
    std::string type;   ///< an EdgeType id
    std::string label;
    std::string fromPort;   ///< an Out port of `from`; empty = the node as a whole
    std::string toPort;     ///< an In port of `to`; empty = the node as a whole
    friend bool operator==(const Edge&, const Edge&) = default;
};

struct Graph {
    std::string       title;
    std::vector<Node> nodes;   ///< draw order, back to front
    std::vector<Edge> edges;

    [[nodiscard]] Node*       FindNode(std::string_view id);
    [[nodiscard]] const Node* FindNode(std::string_view id) const;
    [[nodiscard]] Edge*       FindEdge(std::string_view id);
    [[nodiscard]] const Edge* FindEdge(std::string_view id) const;

    /// @brief Removes the nodes and every edge touching them.
    void RemoveNodes(const std::set<std::string, std::less<>>& ids);
    void RemoveEdges(const std::set<std::string, std::less<>>& ids);

    /// @return the bounds of every node; an empty Rect when there are none
    [[nodiscard]] Rect Bounds() const;

    friend bool operator==(const Graph&, const Graph&) = default;
};

/// @brief A fresh id: 16 lowercase hex digits, the form JSON Canvas uses.
[[nodiscard]] std::string NewId();

} // namespace grapher
