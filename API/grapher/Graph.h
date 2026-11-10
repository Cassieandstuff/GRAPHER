#pragma once
/// @file
/// @brief The document GRAPHER edits: nodes with a type, a frame, named fields and a free text body, and the typed
///        edges between them. A value type, so a host can snapshot it for undo by copying.

#include <grapher/Geometry.h>

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace grapher {

struct Node {
    std::string id;     ///< unique in its graph, stable across saves
    std::string type;   ///< a NodeType id; an unknown one draws neutral
    Rect        frame;  ///< world units
    std::map<std::string, std::string, std::less<>> fields;
    std::string body;   ///< free text (a summary, pseudo-code, notes)
    std::string link;   ///< where the node's own graph lives (host-defined); empty = none yet

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
