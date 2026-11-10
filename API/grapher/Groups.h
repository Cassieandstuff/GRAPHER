#pragma once
/// @file
/// @brief Groups: nodes whose type has NodeShape::Group hold the nodes inside their frame. Membership is geometric, as
///        in JSON Canvas: a node belongs to the smallest group whose frame wholly holds it, so groups nest. Nothing is
///        stored; moving a node in or out of a frame is what changes it.

#include <grapher/Graph.h>
#include <grapher/Schema.h>

#include <string>
#include <string_view>
#include <vector>

namespace grapher {

/// @return whether the node's type draws as a group
[[nodiscard]] bool IsGroup(const Node& node, const Schema& schema);

/// @brief Whether `group` holds `node`: its frame wholly holds the node's, and when the node is a group too, it is
///        strictly larger (so two groups never hold each other).
[[nodiscard]] bool GroupHolds(const Node& group, const Node& node, const Schema& schema);

/// @return the id of the smallest group holding `node` (the first, on a tie); empty at the top level
[[nodiscard]] std::string ParentGroup(const Graph& graph, const Schema& schema, const Node& node);

/// @return every node the group holds, nested groups and their members included, in graph order
[[nodiscard]] std::vector<std::string> GroupMembers(const Graph& graph, const Schema& schema, std::string_view groupId);

} // namespace grapher
