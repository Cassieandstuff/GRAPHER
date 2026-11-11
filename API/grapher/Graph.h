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
