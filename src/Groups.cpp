#include <grapher/Groups.h>

#include <set>

namespace grapher {

bool IsGroup(const Node& node, const Schema& schema)
{
    const NodeType* t = schema.FindNode(node.type);
    return t && t->shape == NodeShape::Group;
}

bool IsMemberGroup(const Node& node, const Schema& schema)
{
    const NodeType* t = schema.FindNode(node.type);
    return t && t->shape == NodeShape::Group && t->groupMode == GroupMode::Members;
}

bool GroupHolds(const Node& group, const Node& node, const Schema& schema)
{
    if (&group == &node || group.id == node.id || !IsGroup(group, schema)) return false;
    if (IsMemberGroup(group, schema)) return node.parent == group.id;
    if (!node.parent.empty() || !group.frame.Holds(node.frame)) return false;   // a member is its member group's alone
    return !IsGroup(node, schema) || node.frame.Area() < group.frame.Area();
}

std::string ParentGroup(const Graph& graph, const Schema& schema, const Node& node)
{
    if (!node.parent.empty())
        if (const Node* p = graph.FindNode(node.parent); p && p->id != node.id && IsMemberGroup(*p, schema)) return p->id;
    const Node* best = nullptr;
    for (const Node& g : graph.nodes)
        if (!IsMemberGroup(g, schema) && GroupHolds(g, node, schema) && (!best || g.frame.Area() < best->frame.Area())) best = &g;
    return best ? best->id : std::string{};
}

std::vector<std::string> GroupMembers(const Graph& graph, const Schema& schema, std::string_view groupId)
{
    std::vector<std::string> out;
    const Node* group = graph.FindNode(groupId);
    if (!group) return out;
    if (IsMemberGroup(*group, schema)) {
        // Every node whose parent chain reaches the group, in graph order; a cycle in the parents ends.
        std::set<std::string, std::less<>> held{std::string(groupId)};
        for (bool grew = true; grew;) {
            grew = false;
            for (const Node& n : graph.nodes)
                if (!n.parent.empty() && held.contains(n.parent) && !held.contains(n.id)) {
                    held.insert(n.id);
                    grew = true;
                }
        }
        for (const Node& n : graph.nodes)
            if (n.id != groupId && held.contains(n.id)) out.push_back(n.id);
        return out;
    }
    // Holding is transitive (a frame inside a frame inside the group is inside the group), so this is the whole tree.
    for (const Node& n : graph.nodes)
        if (GroupHolds(*group, n, schema)) out.push_back(n.id);
    return out;
}

} // namespace grapher
