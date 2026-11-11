#include <grapher/Kit.h>

#include "Json.h"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <utility>

namespace grapher {
namespace {

constexpr std::pair<std::string_view, PartKind> kParts[] = {
    {"card", PartKind::Card},   {"header", PartKind::Header}, {"body", PartKind::Body},   {"text", PartKind::Text},
    {"accent", PartKind::Accent}, {"pins", PartKind::Pins},   {"badge", PartKind::Badge}, {"button", PartKind::Button},
    {"chip", PartKind::Chip},   {"bar", PartKind::Bar},       {"glyph", PartKind::Glyph}, {"image", PartKind::Image},
    {"rim", PartKind::Rim},     {"stub", PartKind::Stub}};
constexpr std::string_view kReserved[] = {"pins", "rows", "groups", "edges", "actions"};

std::string_view PartName(PartKind k)
{
    for (const auto& [name, kind] : kParts)
        if (kind == k) return name;
    return "body";
}

/// @brief Reads typed members, keeping the first problem with where it was found.
class Reader {
public:
    std::string error;
    [[nodiscard]] bool Ok() const { return error.empty(); }
    void Fail(const std::string& where, const std::string& what)
    {
        if (error.empty()) error = where + ": " + what;
    }
    void Str(const json::Value& o, std::string_view key, std::optional<std::string>& out, const std::string& where)
    {
        if (const json::Value* v = o.Find(key)) {
            if (const std::string* s = v->String()) out = *s;
            else Fail(where, "\"" + std::string(key) + "\" is not a string");
        }
    }
    void Str(const json::Value& o, std::string_view key, std::string& out, const std::string& where)
    {
        std::optional<std::string> v;
        Str(o, key, v, where);
        if (v) out = *v;
    }
    void Num(const json::Value& o, std::string_view key, std::optional<float>& out, const std::string& where)
    {
        if (const json::Value* v = o.Find(key)) {
            if (const double* d = v->Number()) out = static_cast<float>(*d);
            else Fail(where, "\"" + std::string(key) + "\" is not a number");
        }
    }
    void Bool(const json::Value& o, std::string_view key, std::optional<bool>& out, const std::string& where)
    {
        if (const json::Value* v = o.Find(key)) {
            if (const bool* b = v->Bool()) out = *b;
            else Fail(where, "\"" + std::string(key) + "\" is not true or false");
        }
    }
    const json::Object* Obj(const json::Value& o, std::string_view key, const std::string& where)
    {
        const json::Value* v = o.Find(key);
        if (!v) return nullptr;
        if (const json::Object* m = v->Members()) return m;
        Fail(where, "\"" + std::string(key) + "\" is not an object");
        return nullptr;
    }

    /// @brief The look and text fields every part and patch may set.
    void Fields(const json::Value& o, PartSpec& p, const std::string& where)
    {
        Bool(o, "visible", p.visible, where);
        Str(o, "fill", p.fill, where);
        Str(o, "stroke", p.stroke, where);
        Str(o, "color", p.color, where);
        Num(o, "strokeWidth", p.strokeWidth, where);
        Num(o, "corner", p.corner, where);
        Num(o, "opacity", p.opacity, where);
        Bool(o, "dash", p.dash, where);
        Num(o, "width", p.width, where);
        Num(o, "top", p.top, where);
        Num(o, "height", p.height, where);
        Str(o, "from", p.from, where);
        Str(o, "placeholder", p.placeholder, where);
        Str(o, "placeholderColor", p.placeholderColor, where);
        Str(o, "font", p.font, where);
        Num(o, "size", p.size, where);
        Num(o, "scale", p.scale, where);
        Num(o, "box", p.box, where);
        Num(o, "advance", p.advance, where);
        Bool(o, "bold", p.bold, where);
        Bool(o, "italic", p.italic, where);
        Str(o, "align", p.align, where);
        if (p.align && *p.align != "left" && *p.align != "center" && *p.align != "right") Fail(where, "\"align\" takes left, center or right");
        Str(o, "if", p.condition, where);
        if (!p.condition.empty() && !ParseSelector(p.condition)) Fail(where, "\"if\" is no selector: " + p.condition);
        Str(o, "editor", p.editor, where);
        if (const json::Object* on = Obj(o, "on", where))
            for (const auto& [gesture, action] : *on) {
                if (const std::string* a = action.String()) p.on[gesture] = *a;
                else Fail(where, "\"on\"." + gesture + " is not an action id");
            }
    }

    void Part(const json::Value& o, PartSpec& p, const std::string& where)
    {
        if (!o.Members()) return Fail(where, "is not an object");
        std::string kind;
        Str(o, "part", kind, where);
        const auto* it = std::find_if(std::begin(kParts), std::end(kParts), [&](const auto& k) { return k.first == kind; });
        if (it == std::end(kParts) || it->second == PartKind::Card) return Fail(where, "\"part\" is \"" + kind + "\", not a part");
        p.kind = it->second;
        Str(o, "id", p.id, where);
        if (p.id.empty()) p.id = kind;
        Fields(o, p, where);
        Parts(o, "content", p.content, where + " > " + p.id);
    }

    void Parts(const json::Value& o, std::string_view key, std::vector<PartSpec>& out, const std::string& where)
    {
        const json::Value* v = o.Find(key);
        if (!v) return;
        const json::Array* a = v->Items();
        if (!a) return Fail(where, "\"" + std::string(key) + "\" is not a list");
        for (std::size_t i = 0; i < a->size() && Ok(); ++i) {
            PartSpec p;
            Part((*a)[i], p, where + " part " + std::to_string(i + 1));
            out.push_back(std::move(p));
        }
    }

    void Metrics(const json::Value& o, MetricsSpec& m, const std::string& where)
    {
        const json::Object* obj = Obj(o, "metrics", where);
        if (!obj) return;
        const json::Value& v = *o.Find("metrics");
        Num(v, "padding", m.padding, where);
        Num(v, "titleSize", m.titleSize, where);
        Num(v, "fontSize", m.fontSize, where);
        Num(v, "header", m.header, where);
        Num(v, "row", m.row, where);
        Num(v, "groupHeader", m.groupHeader, where);
    }
};

// ── writing ──

void PutStr(json::Object& o, std::string_view key, const std::optional<std::string>& v)
{
    if (v) o.emplace_back(std::string(key), *v);
}
void PutNum(json::Object& o, std::string_view key, const std::optional<float>& v)
{
    if (v) o.emplace_back(std::string(key), static_cast<double>(*v));
}
void PutBool(json::Object& o, std::string_view key, const std::optional<bool>& v)
{
    if (v) o.emplace_back(std::string(key), *v);
}

void PutFields(json::Object& o, const PartSpec& p)
{
    PutBool(o, "visible", p.visible);
    PutStr(o, "fill", p.fill);
    PutStr(o, "stroke", p.stroke);
    PutStr(o, "color", p.color);
    PutNum(o, "strokeWidth", p.strokeWidth);
    PutNum(o, "corner", p.corner);
    PutNum(o, "opacity", p.opacity);
    PutBool(o, "dash", p.dash);
    PutNum(o, "width", p.width);
    PutNum(o, "top", p.top);
    PutNum(o, "height", p.height);
    PutStr(o, "from", p.from);
    PutStr(o, "placeholder", p.placeholder);
    PutStr(o, "placeholderColor", p.placeholderColor);
    PutStr(o, "font", p.font);
    PutNum(o, "size", p.size);
    PutNum(o, "scale", p.scale);
    PutNum(o, "box", p.box);
    PutNum(o, "advance", p.advance);
    PutBool(o, "bold", p.bold);
    PutBool(o, "italic", p.italic);
    PutStr(o, "align", p.align);
    if (!p.condition.empty()) o.emplace_back("if", p.condition);
    if (!p.editor.empty()) o.emplace_back("editor", p.editor);
    if (!p.on.empty()) {
        json::Object on;
        for (const auto& [g, a] : p.on) on.emplace_back(g, a);
        o.emplace_back("on", std::move(on));
    }
}

json::Value PartJson(const PartSpec& p)
{
    json::Object o;
    o.emplace_back("part", std::string(PartName(p.kind)));
    if (p.id != PartName(p.kind)) o.emplace_back("id", p.id);
    PutFields(o, p);
    if (!p.content.empty()) {
        json::Array a;
        for (const PartSpec& c : p.content) a.push_back(PartJson(c));
        o.emplace_back("content", std::move(a));
    }
    return o;
}

void PutMetrics(json::Object& o, const MetricsSpec& m)
{
    json::Object v;
    PutNum(v, "padding", m.padding);
    PutNum(v, "titleSize", m.titleSize);
    PutNum(v, "fontSize", m.fontSize);
    PutNum(v, "header", m.header);
    PutNum(v, "row", m.row);
    PutNum(v, "groupHeader", m.groupHeader);
    if (!v.empty()) o.emplace_back("metrics", std::move(v));
}

std::string SelectorText(const Selector& s)
{
    std::string out = s.kind.empty() ? std::string("*") : s.kind;
    for (const std::string& t : s.tags) out += "[" + t + "]";
    static constexpr std::pair<std::uint32_t, std::string_view> kStates[] = {
        {StateSelected, "selected"}, {StateHovered, "hovered"}, {StateDimmed, "dimmed"},
        {StateAlias, "alias"},       {StateCollapsed, "collapsed"}, {StateInGroup, "inGroup"}};
    for (const auto& [bit, name] : kStates)
        if (s.states & bit) out += ":" + std::string(name);
    if (!s.part.empty()) out += " > " + s.part;
    return out;
}

} // namespace

std::optional<Kit> ParseKit(std::string_view text, std::string& error)
{
    const std::optional<json::Value> doc = json::Parse(text, error);
    if (!doc) return std::nullopt;
    if (!doc->Members()) return error = "a kit is a JSON object", std::nullopt;
    Kit    kit;
    Reader r;
    if (const json::Object* tokens = r.Obj(*doc, "tokens", "tokens"))
        for (const auto& [name, value] : *tokens) {
            if (const std::string* s = value.String()) kit.tokens[name] = *s;
            else r.Fail("tokens." + name, "is not a string");
        }
    if (const json::Object* fonts = r.Obj(*doc, "fonts", "fonts"))
        for (const auto& [name, value] : *fonts) {
            FontSpec f;
            const std::string where = "fonts." + name;
            r.Str(value, "family", f.family, where);
            r.Num(value, "size", f.size, where);
            r.Bool(value, "bold", f.bold, where);
            r.Bool(value, "italic", f.italic, where);
            kit.fonts[name] = std::move(f);
        }
    r.Metrics(*doc, kit.metrics, "metrics");
    if (const json::Object* nodes = r.Obj(*doc, "nodes", "nodes"))
        for (const auto& [id, value] : *nodes) {
            const std::string where = "nodes." + id;
            if (!value.Members()) {
                r.Fail(where, "is not an object");
                break;
            }
            NodeKind k;
            k.id = id;
            r.Fields(value, k.card, where);
            r.Parts(value, "parts", k.card.content, where);
            r.Metrics(value, k.metrics, where);
            r.Num(value, "minWidth", k.minWidth, where);
            r.Num(value, "maxWidth", k.maxWidth, where);
            r.Bool(value, "fitWidth", k.fitWidth, where);
            kit.nodes.push_back(std::move(k));
        }
    if (const json::Value* rules = doc->Find("rules")) {
        const json::Array* a = rules->Items();
        if (!a) r.Fail("rules", "is not a list");
        for (std::size_t i = 0; a && i < a->size() && r.Ok(); ++i) {
            const std::string where = "rule " + std::to_string(i + 1);
            std::string       select;
            r.Str((*a)[i], "select", select, where);
            const auto sel = ParseSelector(select);
            if (!sel) {
                r.Fail(where, "\"select\" is no selector: " + select);
                break;
            }
            Rule rule{*sel, {}};
            r.Fields((*a)[i], rule.patch, where);
            kit.rules.push_back(std::move(rule));
        }
    }
    for (const std::string_view key : kReserved)
        if (const json::Value* v = doc->Find(key)) kit.reserved[std::string(key)] = json::Write(*v);
    if (!r.Ok()) return error = r.error, std::nullopt;

    // Every $token a colour names must exist.
    std::vector<std::string> exprs;
    auto collect = [&](const auto& self, const PartSpec& p) -> void {
        for (const auto* e : {&p.fill, &p.stroke, &p.color, &p.placeholderColor})
            if (*e) exprs.push_back(**e);
        for (const PartSpec& c : p.content) self(self, c);
    };
    for (const NodeKind& k : kit.nodes) collect(collect, k.card);
    for (const Rule& rule : kit.rules) collect(collect, rule.patch);
    for (const auto& [name, value] : kit.tokens) exprs.push_back(value);
    for (const std::string& e : exprs)
        if (!ResolveColor(e, Color{}, kit)) return error = "\"" + e + "\" is no colour (or names a token the kit lacks)", std::nullopt;
    return kit;
}

std::string WriteKit(const Kit& kit)
{
    json::Object doc;
    if (!kit.tokens.empty()) {
        json::Object t;
        for (const auto& [k, v] : kit.tokens) t.emplace_back(k, v);
        doc.emplace_back("tokens", std::move(t));
    }
    if (!kit.fonts.empty()) {
        json::Object fonts;
        for (const auto& [k, f] : kit.fonts) {
            json::Object o;
            if (!f.family.empty()) o.emplace_back("family", f.family);
            PutNum(o, "size", f.size);
            PutBool(o, "bold", f.bold);
            PutBool(o, "italic", f.italic);
            fonts.emplace_back(k, std::move(o));
        }
        doc.emplace_back("fonts", std::move(fonts));
    }
    PutMetrics(doc, kit.metrics);
    if (!kit.nodes.empty()) {
        json::Object nodes;
        for (const NodeKind& k : kit.nodes) {
            json::Object o;
            PutFields(o, k.card);
            PutMetrics(o, k.metrics);
            PutNum(o, "minWidth", k.minWidth);
            PutNum(o, "maxWidth", k.maxWidth);
            PutBool(o, "fitWidth", k.fitWidth);
            if (!k.card.content.empty()) {
                json::Array parts;
                for (const PartSpec& p : k.card.content) parts.push_back(PartJson(p));
                o.emplace_back("parts", std::move(parts));
            }
            nodes.emplace_back(k.id, std::move(o));
        }
        doc.emplace_back("nodes", std::move(nodes));
    }
    if (!kit.rules.empty()) {
        json::Array rules;
        for (const Rule& rule : kit.rules) {
            json::Object o;
            o.emplace_back("select", SelectorText(rule.select));
            PutFields(o, rule.patch);
            rules.push_back(std::move(o));
        }
        doc.emplace_back("rules", std::move(rules));
    }
    for (const auto& [key, text] : kit.reserved) {
        std::string error;
        if (const auto v = json::Parse(text, error)) doc.emplace_back(key, *v);
    }
    return json::Write(doc);
}

} // namespace grapher
