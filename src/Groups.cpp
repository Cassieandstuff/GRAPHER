#include <grapher/Groups.h>

namespace grapher {

bool IsGroup(const Node& node, const Schema& schema)
{
    const NodeType* t = schema.FindNode(node.type);
    return t && t->shape == NodeShape::Group;
}

bool GroupHolds(const Node& group, const Node& node, const Schema& schema)
{
    if (&group == &node || group.id == node.id || !IsGroup(group, schema) || !group.frame.Holds(node.frame)) return false;
    return !IsGroup(node, schema) || node.frame.Area() < group.frame.Area();
}

std::string ParentGroup(const Graph& graph, const Schema& schema, const Node& node)
{
    const Node* best = nullptr;
    for (const Node& g : graph.nodes)
        if (GroupHolds(g, node, schema) && (!best || g.frame.Area() < best->frame.Area())) best = &g;
    return best ? best->id : std::string{};
}

std::vector<std::string> GroupMembers(const Graph& graph, const Schema& schema, std::string_view groupId)
{
    std::vector<std::string> out;
    const Node* group = graph.FindNode(groupId);
    if (!group) return out;
    // Holding is transitive (a frame inside a frame inside the group is inside the group), so this is the whole tree.
    for (const Node& n : graph.nodes)
        if (GroupHolds(*group, n, schema)) out.push_back(n.id);
    return out;
}

} // namespace grapher
