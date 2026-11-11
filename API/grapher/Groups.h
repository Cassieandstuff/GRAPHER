#pragma once
/// @file
/// @brief Groups: nodes whose type has NodeShape::Group hold other nodes. A frame group (GroupMode::Frame) holds what
///        lies inside its frame, as in JSON Canvas: a node belongs to the smallest frame group wholly holding it, so
///        groups nest, and moving a node in or out is what changes it. A member group (GroupMode::Members) holds the
///        nodes whose Node::parent names it, at any depth; its frame is drawn around them.

#include <grapher/Graph.h>
#include <grapher/Schema.h>

#include <string>
#include <string_view>
#include <vector>

namespace grapher {

/// @return whether the node's type draws as a group
[[nodiscard]] bool IsGroup(const Node& node, const Schema& schema);
/// @return whether the node is a member group (GroupMode::Members)
[[nodiscard]] bool IsMemberGroup(const Node& node, const Schema& schema);

/// @brief Whether `group` holds `node` directly. A member group: the node's parent names it. A frame group: its frame
///        wholly holds the node's, and when the node is a group too, it is strictly larger (so two never hold each other).
[[nodiscard]] bool GroupHolds(const Node& group, const Node& node, const Schema& schema);

/// @return the member group the node's parent names, else the smallest frame group holding it (the first, on a tie);
///         empty at the top level
[[nodiscard]] std::string ParentGroup(const Graph& graph, const Schema& schema, const Node& node);

/// @return every node the group holds, nested groups and their members included, in graph order
[[nodiscard]] std::vector<std::string> GroupMembers(const Graph& graph, const Schema& schema, std::string_view groupId);

} // namespace grapher
