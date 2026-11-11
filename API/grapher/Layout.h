#pragma once
/// @file
/// @brief Automatic layout. LayoutColumns places items in columns by how far each sits from an item nothing wires to,
///        orders each column to cross as few wires as it can and draws each item level with what it wires to. It knows
///        nothing of nodes, so LayoutGraph runs it inside out over a graph's member groups (GroupMode::Members): the
///        deepest groups first, each then one item of the group holding it. Both stay fast on tens of thousands of
///        items: O(E log E) per sweep and a fixed number of sweeps.

#include <grapher/Graph.h>
#include <grapher/Schema.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace grapher {

struct LayoutGaps {
    float column = 90.f;   ///< between columns
    float row    = 14.f;   ///< between items of a column
};

/// @brief The top-left of each item, the first column at x = 0 and the highest item at y = 0.
/// @param sizes each item's width and height
/// @param edges (from, to) into `sizes`; self edges and cycles are fine (an item in a cycle nothing enters starts a column at the end)
[[nodiscard]] std::vector<Vec2> LayoutColumns(const std::vector<Vec2>& sizes, const std::vector<std::pair<int, int>>& edges,
                                              LayoutGaps gaps = {});

struct LayoutOptions {
    LayoutGaps gaps;
    float      padding = 14.f;   ///< between a member group's outline and what it holds
    float      header  = 30.f;   ///< a member group's header strip (Style::groupHeader)
    /// @brief An open group pushes what it would cover: what starts right of this much of its width moves right, the rest
    ///        moves down, until nothing overlaps.
    float      pushReach = 24.f;
};

/// @brief Lay out every node of `graph` that `keep` doesn't name, inside out through its member groups (Node::parent).
///        A member group's children (nodes and groups) lay out in columns, wired by the edges between what each holds;
///        a collapsed group (in `collapsed`) is one header-high item. Kept nodes stay where they are, and a placed node
///        that would overlap one slides down until clear. Then an open group pushes its siblings clear of it.
/// @param origin where the top level starts
/// @return every node's frame, member groups included (each wraps what it holds, below its header)
[[nodiscard]] std::vector<std::pair<std::string, Rect>> LayoutGraph(const Graph& graph, const Schema& schema,
                                                                    const std::set<std::string, std::less<>>& keep,
                                                                    const std::set<std::string, std::less<>>& collapsed,
                                                                    const LayoutOptions& options = {}, Vec2 origin = {});

} // namespace grapher
