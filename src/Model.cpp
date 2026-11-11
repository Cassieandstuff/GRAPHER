#include <grapher/Geometry.h>
#include <grapher/Graph.h>
#include <grapher/Schema.h>

#include <algorithm>
#include <cstdio>
#include <random>

namespace grapher {

// ── Color ────────────────────────────────────────────────────────────────────

std::string Color::ToHex() const
{
    char buf[10];
    if (a == 255)
        std::snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
    else
        std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", r, g, b, a);
    return buf;
}

namespace {
int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

std::optional<Color> ParseColor(std::string_view text)
{
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    for (char c : text)
        if (HexDigit(c) < 0) return std::nullopt;
    auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(HexDigit(text[i]) * 16 + HexDigit(text[i + 1])); };
    switch (text.size()) {
    case 3: {
        auto nib = [&](std::size_t i) { return static_cast<std::uint8_t>(HexDigit(text[i]) * 17); };
        return Color{nib(0), nib(1), nib(2), 255};
    }
    case 6: return Color{byte(0), byte(2), byte(4), 255};
    case 8: return Color{byte(0), byte(2), byte(4), byte(6)};
    default: return std::nullopt;
    }
}

// ── Schema ───────────────────────────────────────────────────────────────────

const NodeType* Schema::FindNode(std::string_view id) const
{
    for (const NodeType& t : nodeTypes)
        if (t.id == id) return &t;
    return nullptr;
}

const EdgeType* Schema::FindEdge(std::string_view id) const
{
    for (const EdgeType& t : edgeTypes)
        if (t.id == id) return &t;
    return nullptr;
}

std::string Schema::EdgeFor(std::string_view fromType, std::string_view toType) const
{
    for (std::string_view t : {fromType, toType})
        if (const NodeType* n = FindNode(t); n && !n->defaultEdge.empty() && FindEdge(n->defaultEdge)) return n->defaultEdge;
    return edgeTypes.empty() ? std::string{} : edgeTypes.front().id;
}

const PortType* Schema::FindPortType(std::string_view id) const
{
    for (const PortType& t : portTypes)
        if (t.id == id) return &t;
    return nullptr;
}

const RowStyle* Schema::FindRowStyle(std::string_view id) const
{
    for (const RowStyle& s : rowStyles)
        if (s.id == id) return &s;
    return nullptr;
}

bool Schema::IsA(std::string_view type, std::string_view base) const
{
    if (type == base) return true;
    // Breadth-first up the bases; `seen` keeps a cycle in bad data from looping.
    std::vector<std::string_view> todo{type}, seen{type};
    for (std::size_t i = 0; i < todo.size(); ++i) {
        const PortType* t = FindPortType(todo[i]);
        if (!t) continue;
        for (const std::string& b : t->bases) {
            if (b == base) return true;
            if (std::find(seen.begin(), seen.end(), b) != seen.end()) continue;
            seen.push_back(b);
            todo.push_back(b);
        }
    }
    return false;
}

bool Schema::CanConnect(std::string_view out, std::string_view in) const
{
    if (out.empty() || in.empty()) return true;
    if (subtypeFlow == SubtypeFlow::OutIsA ? IsA(out, in) : IsA(in, out)) return true;
    for (const Conversion& c : conversions)
        if (c.from == out && c.to == in) return true;
    return false;
}

void Schema::PutNodeType(NodeType type)
{
    ++revision;
    for (NodeType& t : nodeTypes)
        if (t.id == type.id) {
            t = std::move(type);
            return;
        }
    nodeTypes.push_back(std::move(type));
}

// ── Graph ────────────────────────────────────────────────────────────────────

const Row* Node::FindRow(std::string_view id) const
{
    for (const Row& r : rows)
        if (r.id == id) return &r;
    return nullptr;
}

const std::string& Node::Field(std::string_view key) const
{
    static const std::string kEmpty;
    const auto it = fields.find(key);
    return it == fields.end() ? kEmpty : it->second;
}

Node* Graph::FindNode(std::string_view id)
{
    for (Node& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

const Node* Graph::FindNode(std::string_view id) const
{
    for (const Node& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

Edge* Graph::FindEdge(std::string_view id)
{
    for (Edge& e : edges)
        if (e.id == id) return &e;
    return nullptr;
}

const Edge* Graph::FindEdge(std::string_view id) const
{
    for (const Edge& e : edges)
        if (e.id == id) return &e;
    return nullptr;
}

void Graph::RemoveNodes(const std::set<std::string, std::less<>>& ids)
{
    std::erase_if(nodes, [&](const Node& n) { return ids.contains(n.id); });
    std::erase_if(edges, [&](const Edge& e) { return ids.contains(e.from) || ids.contains(e.to); });
}

void Graph::RemoveEdges(const std::set<std::string, std::less<>>& ids)
{
    std::erase_if(edges, [&](const Edge& e) { return ids.contains(e.id); });
}

Rect Graph::Bounds() const
{
    if (nodes.empty()) return {};
    Rect r = nodes.front().frame;
    for (const Node& n : nodes) r = Rect::Union(r, n.frame);
    return r;
}

std::string NewId()
{
    thread_local std::mt19937_64 rng{std::random_device{}()};
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(rng()));
    return buf;
}

} // namespace grapher
