#include <grapher/Ports.h>

#include <grapher/Groups.h>

#include <algorithm>

namespace grapher {

namespace {

Vec2 NormalOf(PortSide s)
{
    switch (s) {
    case PortSide::Left: return {-1, 0};
    case PortSide::Right: return {1, 0};
    case PortSide::Top: return {0, -1};
    default: return {0, 1};
    }
}

/// @brief The point at fraction `t` along a side.
Vec2 Along(const Rect& r, PortSide s, float t)
{
    switch (s) {
    case PortSide::Left: return {r.x, r.y + r.h * t};
    case PortSide::Right: return {r.Right(), r.y + r.h * t};
    case PortSide::Top: return {r.x + r.w * t, r.y};
    default: return {r.x + r.w * t, r.Bottom()};
    }
}

bool IsCard(const Node& node, const Schema& schema)
{
    const NodeType* t = schema.FindNode(node.type);
    return !t || t->shape == NodeShape::Card;
}

} // namespace

std::vector<PortDef> PortsOf(const Node& node, const Schema& schema)
{
    std::vector<PortDef> out;
    if (const NodeType* t = schema.FindNode(node.type)) out = t->ports;
    for (const PortDef& p : node.ports) {
        const auto same = std::find_if(out.begin(), out.end(), [&](const PortDef& q) { return q.id == p.id; });
        if (same != out.end()) *same = p;
        else out.push_back(p);
    }
    return out;
}

std::vector<PortSlot> LayoutPorts(const Node& node, const Schema& schema, const CardMetrics& m)
{
    std::vector<PortSlot> out;
    const std::vector<PortDef> ports = PortsOf(node, schema);
    if (ports.empty()) return out;
    const Rect& f    = node.frame;
    const bool  card = IsCard(node, schema);
    const bool  group = !card && IsGroup(node, schema);

    // Ports that spread along their side: every port of a non-card shape, and a card's top and bottom ports.
    std::size_t spreadCount[5] = {};
    for (const PortDef& p : ports) {
        const PortSide s = SideOf(p);
        const bool     rowed = card && (s == PortSide::Left || s == PortSide::Right);
        if (!rowed && !(p.onHeader && (card || group))) ++spreadCount[static_cast<int>(s)];
    }
    std::size_t spreadAt[5] = {};
    int         rowAt[2]    = {0, 0};   // left, right

    out.reserve(ports.size());
    for (const PortDef& p : ports) {
        PortSlot slot;
        slot.def    = p;
        slot.side   = SideOf(p);
        slot.normal = NormalOf(slot.side);
        const bool horizontal = slot.side == PortSide::Left || slot.side == PortSide::Right;
        const float x = slot.side == PortSide::Left ? f.x : f.Right();
        if (p.onHeader && horizontal && (card || group)) {
            // On the title line (a group's header strip).
            const float y = card ? f.y + m.Top() + m.titleSize * 0.65f : f.y + std::min(f.h, m.groupHeader) * 0.5f;
            slot.at = {x, std::min(y, f.Bottom())};
        } else if (card && horizontal) {
            int&        k = rowAt[slot.side == PortSide::Left ? 0 : 1];
            const float y = f.y + m.Header() + m.Row() * static_cast<float>(k);
            slot.row = {f.x, y, f.w, m.Row()};
            slot.at  = {x, y + m.Row() * 0.5f};
            ++k;
        } else {
            const int   s = static_cast<int>(slot.side);
            const float t = (static_cast<float>(spreadAt[s]) + 1.f) / (static_cast<float>(spreadCount[s]) + 1.f);
            ++spreadAt[s];
            slot.at = Along(f, slot.side, t);
        }
        out.push_back(std::move(slot));
    }
    return out;
}

const PortSlot* FindSlot(const std::vector<PortSlot>& slots, std::string_view id)
{
    for (const PortSlot& s : slots)
        if (s.def.id == id) return &s;
    return nullptr;
}

int PortRows(const Node& node, const Schema& schema)
{
    if (!IsCard(node, schema)) return 0;
    int left = 0, right = 0;
    for (const PortDef& p : PortsOf(node, schema)) {
        if (p.onHeader) continue;
        const PortSide s = SideOf(p);
        if (s == PortSide::Left) ++left;
        else if (s == PortSide::Right) ++right;
    }
    return std::max(left, right);
}

namespace {

std::string TypeOf(const Graph& graph, const Schema& schema, const Node& node, std::string_view port, int depth)
{
    const std::vector<PortDef> ports = PortsOf(node, schema);
    const auto def = std::find_if(ports.begin(), ports.end(), [&](const PortDef& p) { return p.id == port; });
    if (def == ports.end()) return {};
    if (def->typeFrom.empty() || depth > 64) return def->type;   // the depth bound stops a loop of reroutes
    const auto from = std::find_if(ports.begin(), ports.end(), [&](const PortDef& p) { return p.id == def->typeFrom; });
    if (from != ports.end())
        for (const Edge& e : graph.edges) {
            const bool in = from->dir == PortDir::In;
            if (in ? (e.to != node.id || e.toPort != from->id) : (e.from != node.id || e.fromPort != from->id)) continue;
            const Node* other = graph.FindNode(in ? e.from : e.to);
            const std::string& otherPort = in ? e.fromPort : e.toPort;
            if (!other || otherPort.empty()) continue;
            if (std::string t = TypeOf(graph, schema, *other, otherPort, depth + 1); !t.empty()) return t;
        }
    return def->type;
}

} // namespace

std::string PortTypeOf(const Graph& graph, const Schema& schema, const Node& node, std::string_view port)
{
    return TypeOf(graph, schema, node, port, 0);
}

float CardHeight(const Node& node, const Schema& schema, const CardMetrics& m)
{
    const NodeType* t = schema.FindNode(node.type);
    std::vector<std::string> bound;
    for (const PortDef& p : PortsOf(node, schema))
        if (!p.field.empty()) bound.push_back(p.field);
    int fields = 0;
    if (t)
        for (const FieldDef& f : t->fields)
            if (f.onCard && f.id != t->titleField && std::find(bound.begin(), bound.end(), f.id) == bound.end()) ++fields;
    return m.Header() + m.Row() * static_cast<float>(PortRows(node, schema) + fields) + m.padding;
}

} // namespace grapher
