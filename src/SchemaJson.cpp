#include <grapher/SchemaJson.h>

#include "Json.h"

#include <cmath>
#include <initializer_list>
#include <utility>

namespace grapher {

namespace {

template <class E> using Names = std::initializer_list<std::pair<std::string_view, E>>;

constexpr Names<NodeShape> kShapes{{"card", NodeShape::Card}, {"note", NodeShape::Note}, {"pill", NodeShape::Pill}, {"group", NodeShape::Group}, {"dot", NodeShape::Dot}, {"dial", NodeShape::Dial}};
constexpr Names<FieldKind> kKinds{{"text", FieldKind::Text}, {"path", FieldKind::Path}, {"url", FieldKind::Url}, {"tag", FieldKind::Tag}};
constexpr Names<EdgeLine>  kLines{{"curve", EdgeLine::Curve}, {"straight", EdgeLine::Straight}, {"elbow", EdgeLine::Elbow}};
constexpr Names<PortGlyph> kGlyphs{{"circle", PortGlyph::Circle}, {"square", PortGlyph::Square}, {"diamond", PortGlyph::Diamond}, {"arrow", PortGlyph::Arrow}};
constexpr Names<PortDir>   kDirs{{"in", PortDir::In}, {"out", PortDir::Out}};
constexpr Names<PortSide>  kSides{{"auto", PortSide::Auto}, {"left", PortSide::Left}, {"right", PortSide::Right}, {"top", PortSide::Top}, {"bottom", PortSide::Bottom}};
constexpr Names<SubtypeFlow> kFlows{{"out-is-a", SubtypeFlow::OutIsA}, {"in-is-a", SubtypeFlow::InIsA}};
constexpr Names<GroupMode>   kGroupModes{{"frame", GroupMode::Frame}, {"members", GroupMode::Members}};

template <class E> std::string_view NameOf(Names<E> names, E e)
{
    for (const auto& [name, value] : names)
        if (value == e) return name;
    return {};
}

/// @brief Reads typed members off JSON objects, keeping the first problem with where it was found.
class Reader {
public:
    std::string error;

    [[nodiscard]] bool Ok() const { return error.empty(); }

    void Fail(std::string_view where, std::string_view what)
    {
        if (error.empty()) error = std::string(where) + ": " + std::string(what);
    }

    void Str(const json::Value& o, std::string_view key, std::string& out, std::string_view where)
    {
        const json::Value* v = o.Find(key);
        if (!v) return;
        if (const std::string* s = v->String()) out = *s;
        else Fail(where, "\"" + std::string(key) + "\" is not a string");
    }

    void Num(const json::Value& o, std::string_view key, float& out, std::string_view where)
    {
        const json::Value* v = o.Find(key);
        if (!v) return;
        if (const double* d = v->Number()) out = static_cast<float>(*d);
        else Fail(where, "\"" + std::string(key) + "\" is not a number");
    }

    void Int(const json::Value& o, std::string_view key, int& out, std::string_view where)
    {
        const json::Value* v = o.Find(key);
        if (!v) return;
        const double* d = v->Number();
        if (d && std::floor(*d) == *d && std::abs(*d) < 1e9) out = static_cast<int>(*d);
        else Fail(where, "\"" + std::string(key) + "\" is not a whole number");
    }

    void Bool(const json::Value& o, std::string_view key, bool& out, std::string_view where)
    {
        const json::Value* v = o.Find(key);
        if (!v) return;
        if (const bool* b = v->Bool()) out = *b;
        else Fail(where, "\"" + std::string(key) + "\" is not true or false");
    }

    void Colour(const json::Value& o, std::string_view key, Color& out, std::string_view where)
    {
        std::string text;
        Str(o, key, text, where);
        if (text.empty()) return;
        if (const auto c = ParseColor(text)) out = *c;
        else Fail(where, "\"" + std::string(key) + "\" is not a colour: " + text);
    }

    void Strings(const json::Value& o, std::string_view key, std::vector<std::string>& out, std::string_view where)
    {
        const json::Value* v = o.Find(key);
        if (!v) return;
        const json::Array* a = v->Items();
        if (!a) return Fail(where, "\"" + std::string(key) + "\" is not a list");
        out.clear();
        for (const json::Value& item : *a) {
            if (const std::string* s = item.String()) out.push_back(*s);
            else return Fail(where, "\"" + std::string(key) + "\" holds something other than a string");
        }
    }

    template <class E> void Enum(const json::Value& o, std::string_view key, Names<E> names, E& out, std::string_view where)
    {
        std::string text;
        Str(o, key, text, where);
        if (!o.Find(key) || !Ok()) return;
        for (const auto& [name, value] : names)
            if (name == text) {
                out = value;
                return;
            }
        std::string allowed;
        for (const auto& [name, value] : names) allowed += (allowed.empty() ? "" : ", ") + std::string(name);
        Fail(where, "\"" + std::string(key) + "\" is \"" + text + "\"; it takes " + allowed);
    }

    /// @return the list under `key` (empty when absent); fails when it is something else
    const json::Array& List(const json::Value& o, std::string_view key, std::string_view where)
    {
        static const json::Array kNone;
        const json::Value* v = o.Find(key);
        if (!v) return kNone;
        if (const json::Array* a = v->Items()) return *a;
        Fail(where, "\"" + std::string(key) + "\" is not a list");
        return kNone;
    }

    /// @brief The id of an entry (required) and its label (the id when absent).
    bool Named(const json::Value& o, std::string& id, std::string& label, std::string_view what, std::size_t index)
    {
        const std::string where = std::string(what) + " " + std::to_string(index + 1);
        if (!o.Members()) return Fail(where, "is not an object"), false;
        Str(o, "id", id, where);
        if (Ok() && id.empty()) Fail(where, "has no id");
        label = id;
        Str(o, "label", label, where);
        return Ok();
    }
};

PortDef ReadPort(Reader& r, const json::Value& o, std::size_t index, std::string_view owner)
{
    PortDef p;
    if (!r.Named(o, p.id, p.label, std::string(owner) + " port", index)) return p;
    const std::string where = std::string(owner) + " port " + p.id;
    r.Str(o, "type", p.type, where);
    r.Enum(o, "dir", kDirs, p.dir, where);
    r.Enum(o, "side", kSides, p.side, where);
    r.Int(o, "maxWires", p.maxWires, where);
    r.Str(o, "field", p.field, where);
    r.Bool(o, "onHeader", p.onHeader, where);
    r.Str(o, "typeFrom", p.typeFrom, where);
    r.Str(o, "tooltip", p.tooltip, where);
    return p;
}

NodeType ReadNodeType(Reader& r, const json::Value& o, std::size_t index)
{
    NodeType t;
    if (!r.Named(o, t.id, t.label, "node type", index)) return t;
    const std::string where = "node type " + t.id;
    r.Colour(o, "band", t.band, where);
    r.Num(o, "width", t.width, where);
    r.Num(o, "height", t.height, where);
    r.Enum(o, "shape", kShapes, t.shape, where);
    r.Bool(o, "drillable", t.drillable, where);
    r.Str(o, "titleField", t.titleField, where);
    r.Str(o, "bodyLabel", t.bodyLabel, where);
    r.Str(o, "defaultEdge", t.defaultEdge, where);
    r.Num(o, "imageHeight", t.imageHeight, where);
    r.Enum(o, "groupMode", kGroupModes, t.groupMode, where);
    r.Bool(o, "showCount", t.showCount, where);
    r.Bool(o, "fitHeight", t.fitHeight, where);
    r.Bool(o, "fixed", t.fixed, where);
    const json::Array& fields = r.List(o, "fields", where);
    for (std::size_t k = 0; k < fields.size() && r.Ok(); ++k) {
        FieldDef f;
        if (!r.Named(fields[k], f.id, f.label, where + " field", k)) break;
        const std::string fw = where + " field " + f.id;
        r.Enum(fields[k], "kind", kKinds, f.kind, fw);
        r.Strings(fields[k], "choices", f.choices, fw);
        r.Bool(fields[k], "onCard", f.onCard, fw);
        t.fields.push_back(std::move(f));
    }
    const json::Array& ports = r.List(o, "ports", where);
    for (std::size_t k = 0; k < ports.size() && r.Ok(); ++k) t.ports.push_back(ReadPort(r, ports[k], k, where));
    return t;
}

std::expected<json::Value, std::string> Document(std::string_view text)
{
    std::string error;
    std::optional<json::Value> doc = json::Parse(text, error);
    if (!doc) return std::unexpected("not valid JSON: " + error);
    if (!doc->Members()) return std::unexpected(std::string("not a JSON object"));
    return std::move(*doc);
}

// ── Writing ──────────────────────────────────────────────────────────────────

/// @brief Adds `key` only when the value differs from the default.
template <class T> void Put(json::Object& o, std::string_view key, const T& value, const T& def, json::Value written)
{
    if (!(value == def)) o.emplace_back(std::string(key), std::move(written));
}

void PutLabel(json::Object& o, const std::string& id, const std::string& label)
{
    o.emplace_back("id", id);
    if (label != id) o.emplace_back("label", label);
}

json::Value Strings(const std::vector<std::string>& v)
{
    json::Array a;
    for (const std::string& s : v) a.emplace_back(s);
    return a;
}

json::Value WritePort(const PortDef& p)
{
    const PortDef def;
    json::Object o;
    PutLabel(o, p.id, p.label);
    Put(o, "type", p.type, def.type, p.type);
    Put(o, "dir", p.dir, def.dir, std::string(NameOf(kDirs, p.dir)));
    Put(o, "side", p.side, def.side, std::string(NameOf(kSides, p.side)));
    Put(o, "maxWires", p.maxWires, def.maxWires, p.maxWires);
    Put(o, "field", p.field, def.field, p.field);
    Put(o, "onHeader", p.onHeader, def.onHeader, p.onHeader);
    Put(o, "typeFrom", p.typeFrom, def.typeFrom, p.typeFrom);
    Put(o, "tooltip", p.tooltip, def.tooltip, p.tooltip);
    return o;
}

json::Value WriteNodeType(const NodeType& t)
{
    const NodeType def;
    json::Object o;
    PutLabel(o, t.id, t.label);
    Put(o, "band", t.band, def.band, t.band.ToHex());
    Put(o, "width", t.width, def.width, static_cast<double>(t.width));
    Put(o, "height", t.height, def.height, static_cast<double>(t.height));
    Put(o, "shape", t.shape, def.shape, std::string(NameOf(kShapes, t.shape)));
    Put(o, "drillable", t.drillable, def.drillable, t.drillable);
    Put(o, "titleField", t.titleField, def.titleField, t.titleField);
    Put(o, "bodyLabel", t.bodyLabel, def.bodyLabel, t.bodyLabel);
    Put(o, "defaultEdge", t.defaultEdge, def.defaultEdge, t.defaultEdge);
    Put(o, "imageHeight", t.imageHeight, def.imageHeight, static_cast<double>(t.imageHeight));
    Put(o, "groupMode", t.groupMode, def.groupMode, std::string(NameOf(kGroupModes, t.groupMode)));
    Put(o, "showCount", t.showCount, def.showCount, t.showCount);
    Put(o, "fitHeight", t.fitHeight, def.fitHeight, t.fitHeight);
    Put(o, "fixed", t.fixed, def.fixed, t.fixed);
    if (!t.fields.empty()) {
        json::Array fields;
        for (const FieldDef& f : t.fields) {
            const FieldDef fd;
            json::Object fo;
            PutLabel(fo, f.id, f.label);
            Put(fo, "kind", f.kind, fd.kind, std::string(NameOf(kKinds, f.kind)));
            Put(fo, "choices", f.choices, fd.choices, Strings(f.choices));
            Put(fo, "onCard", f.onCard, fd.onCard, f.onCard);
            fields.emplace_back(std::move(fo));
        }
        o.emplace_back("fields", std::move(fields));
    }
    if (!t.ports.empty()) {
        json::Array ports;
        for (const PortDef& p : t.ports) ports.push_back(WritePort(p));
        o.emplace_back("ports", std::move(ports));
    }
    return o;
}

} // namespace

std::expected<Schema, std::string> ParseSchema(std::string_view text)
{
    auto doc = Document(text);
    if (!doc) return std::unexpected(doc.error());
    const json::Value& root = *doc;
    Reader             r;
    Schema             s;
    r.Enum(root, "subtypeFlow", kFlows, s.subtypeFlow, "schema");

    const json::Array& portTypes = r.List(root, "portTypes", "schema");
    for (std::size_t k = 0; k < portTypes.size() && r.Ok(); ++k) {
        PortType t;
        if (!r.Named(portTypes[k], t.id, t.label, "port type", k)) break;
        const std::string where = "port type " + t.id;
        r.Colour(portTypes[k], "color", t.color, where);
        r.Enum(portTypes[k], "glyph", kGlyphs, t.glyph, where);
        r.Strings(portTypes[k], "bases", t.bases, where);
        r.Str(portTypes[k], "edge", t.edge, where);
        s.portTypes.push_back(std::move(t));
    }
    const json::Array& conversions = r.List(root, "conversions", "schema");
    for (std::size_t k = 0; k < conversions.size() && r.Ok(); ++k) {
        Conversion c;
        const std::string where = "conversion " + std::to_string(k + 1);
        r.Str(conversions[k], "from", c.from, where);
        r.Str(conversions[k], "to", c.to, where);
        if (r.Ok() && (c.from.empty() || c.to.empty())) r.Fail(where, "needs both \"from\" and \"to\"");
        s.conversions.push_back(std::move(c));
    }
    const json::Array& rowStyles = r.List(root, "rowStyles", "schema");
    for (std::size_t k = 0; k < rowStyles.size() && r.Ok(); ++k) {
        RowStyle    st;
        std::string label;
        if (!r.Named(rowStyles[k], st.id, label, "row style", k)) break;
        const std::string where = "row style " + st.id;
        for (const auto& [key, slot] : {std::pair{"color", &st.color}, std::pair{"valueColor", &st.valueColor}})
            if (rowStyles[k].Find(key)) {
                Color c;
                r.Colour(rowStyles[k], key, c, where);
                *slot = c;
            }
        r.Str(rowStyles[k], "prefix", st.prefix, where);
        r.Bool(rowStyles[k], "bold", st.bold, where);
        r.Bool(rowStyles[k], "italic", st.italic, where);
        r.Bool(rowStyles[k], "chip", st.chip, where);
        s.rowStyles.push_back(std::move(st));
    }
    const json::Array& nodes = r.List(root, "nodes", "schema");
    for (std::size_t k = 0; k < nodes.size() && r.Ok(); ++k) s.nodeTypes.push_back(ReadNodeType(r, nodes[k], k));
    const json::Array& edges = r.List(root, "edges", "schema");
    for (std::size_t k = 0; k < edges.size() && r.Ok(); ++k) {
        EdgeType t;
        if (!r.Named(edges[k], t.id, t.label, "edge type", k)) break;
        const std::string where = "edge type " + t.id;
        r.Colour(edges[k], "color", t.color, where);
        r.Enum(edges[k], "line", kLines, t.line, where);
        r.Bool(edges[k], "dashed", t.dashed, where);
        r.Bool(edges[k], "arrow", t.arrow, where);
        r.Bool(edges[k], "portColor", t.portColor, where);
        r.Bool(edges[k], "dotted", t.dotted, where);
        r.Num(edges[k], "width", t.width, where);
        s.edgeTypes.push_back(std::move(t));
    }
    if (!r.Ok()) return std::unexpected(r.error);
    return s;
}

std::expected<NodeType, std::string> ParseNodeType(std::string_view text)
{
    auto doc = Document(text);
    if (!doc) return std::unexpected(doc.error());
    Reader   r;
    NodeType t = ReadNodeType(r, *doc, 0);
    if (!r.Ok()) return std::unexpected(r.error);
    return t;
}

std::string WriteSchema(const Schema& s)
{
    json::Object root;
    if (s.subtypeFlow != SubtypeFlow::OutIsA) root.emplace_back("subtypeFlow", std::string(NameOf(kFlows, s.subtypeFlow)));
    if (!s.portTypes.empty()) {
        json::Array a;
        for (const PortType& t : s.portTypes) {
            const PortType def;
            json::Object o;
            PutLabel(o, t.id, t.label);
            Put(o, "color", t.color, def.color, t.color.ToHex());
            Put(o, "glyph", t.glyph, def.glyph, std::string(NameOf(kGlyphs, t.glyph)));
            Put(o, "bases", t.bases, def.bases, Strings(t.bases));
            Put(o, "edge", t.edge, def.edge, t.edge);
            a.emplace_back(std::move(o));
        }
        root.emplace_back("portTypes", std::move(a));
    }
    if (!s.conversions.empty()) {
        json::Array a;
        for (const Conversion& c : s.conversions) a.emplace_back(json::Object{{"from", c.from}, {"to", c.to}});
        root.emplace_back("conversions", std::move(a));
    }
    if (!s.rowStyles.empty()) {
        json::Array a;
        for (const RowStyle& st : s.rowStyles) {
            const RowStyle def;
            json::Object o;
            o.emplace_back("id", st.id);
            if (st.color) o.emplace_back("color", st.color->ToHex());
            if (st.valueColor) o.emplace_back("valueColor", st.valueColor->ToHex());
            Put(o, "prefix", st.prefix, def.prefix, st.prefix);
            Put(o, "bold", st.bold, def.bold, st.bold);
            Put(o, "italic", st.italic, def.italic, st.italic);
            Put(o, "chip", st.chip, def.chip, st.chip);
            a.emplace_back(std::move(o));
        }
        root.emplace_back("rowStyles", std::move(a));
    }
    json::Array nodes;
    for (const NodeType& t : s.nodeTypes) nodes.push_back(WriteNodeType(t));
    root.emplace_back("nodes", std::move(nodes));
    json::Array edges;
    for (const EdgeType& t : s.edgeTypes) {
        const EdgeType def;
        json::Object o;
        PutLabel(o, t.id, t.label);
        Put(o, "color", t.color, def.color, t.color.ToHex());
        Put(o, "line", t.line, def.line, std::string(NameOf(kLines, t.line)));
        Put(o, "dashed", t.dashed, def.dashed, t.dashed);
        Put(o, "arrow", t.arrow, def.arrow, t.arrow);
        Put(o, "portColor", t.portColor, def.portColor, t.portColor);
        Put(o, "dotted", t.dotted, def.dotted, t.dotted);
        Put(o, "width", t.width, def.width, static_cast<double>(t.width));
        edges.emplace_back(std::move(o));
    }
    root.emplace_back("edges", std::move(edges));
    return json::Write(root);
}

} // namespace grapher
