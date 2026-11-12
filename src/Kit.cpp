#include <grapher/Kit.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>

namespace grapher {

bool KindCovers(std::string_view kind, std::string_view id)
{
    if (kind == id) return true;
    return !kind.empty() && kind.back() == '*' && id.starts_with(kind.substr(0, kind.size() - 1));
}

namespace {

// The kind with `id`, else the longest prefix kind that covers it.
template <class K> const K* BestKind(const std::vector<K>& kinds, std::string_view id)
{
    const K* best = nullptr;
    for (const K& k : kinds) {
        if (k.id == id) return &k;
        if (KindCovers(k.id, id) && (!best || k.id.size() > best->id.size())) best = &k;
    }
    return best;
}

} // namespace

const NodeKind* Kit::FindNode(std::string_view id) const { return BestKind(nodes, id); }
const PinKind*  Kit::FindPin(std::string_view id) const { return BestKind(pins, id); }

void Kit::PutNodeKind(NodeKind kind)
{
    ++revision;
    for (NodeKind& k : nodes)
        if (k.id == kind.id) {
            k = std::move(kind);
            return;
        }
    nodes.push_back(std::move(kind));
}

namespace {

/// @brief The part tree every node kind gets from a Schema: today's card, as parts.
PartSpec TextPart(std::string id, std::string color, float size)
{
    PartSpec p{PartKind::Text, std::move(id)};
    p.color = std::move(color);
    p.size  = size;
    return p;
}

// A group: a tinted frame with a title bar holding its title (an arrow and a count where its type has them) and its
// on-card fields.
NodeKind DefaultGroupKind(const NodeType& t, const Style& s)
{
    NodeKind k;
    k.id               = t.id;
    k.card.fill        = Color{t.band.r, t.band.g, t.band.b, s.groupFill}.ToHex();
    k.card.stroke      = "band";
    k.card.strokeWidth = 1.25f;
    k.card.corner      = s.corner;
    PartSpec bar{PartKind::Header, "header"};
    bar.fill = Color{t.band.r, t.band.g, t.band.b, static_cast<std::uint8_t>(std::min(255, s.groupFill * 2))}.ToHex();
    PartSpec title = TextPart("title", "$text", s.fontSize);
    if (!t.titleField.empty()) title.from = "field:" + t.titleField;
    title.placeholder      = "Untitled {type}";
    title.placeholderColor = "$muted";
    title.scale            = 1.1f;
    title.bold             = true;
    std::string format = "{value}";
    if (t.groupMode == GroupMode::Members && !t.fixed) format = "{arrow}" + format;
    if (t.showCount) format += " ({count})";
    title.format       = format;
    PartSpec detail    = TextPart("detail", "$muted", s.fontSize);
    detail.from        = "fields";
    bar.content        = {title, detail};
    k.card.content     = {bar};
    return k;
}

// A pill: a capsule with its on-card fields on one line, bold and centred (its type's label, muted, when it has none).
NodeKind DefaultPillKind(const NodeType& t, const Style& s)
{
    NodeKind k;
    k.id               = t.id;
    k.card.fill        = "$nodeFill";
    k.card.stroke      = "band";
    k.card.strokeWidth = 1.5f;
    k.card.corner      = s.corner;
    PartSpec text      = TextPart("text", "$text", s.fontSize);
    text.from             = "fields:all";
    text.placeholder      = "{type}";
    text.placeholderColor = "$muted";
    text.bold             = true;
    text.align            = "center";
    k.card.content        = {text};
    return k;
}

// A dial's ring in its band; the needle and hub in the text colour; gold diamond handles; labels outside the rim and the
// reading under the hub.
PartSpec DefaultRim()
{
    PartSpec rim{PartKind::Rim, "rim"};
    rim.stroke = "band";
    rim.width  = 3.f;
    PartSpec needle{PartKind::Bar, "needle"};
    needle.color = "$text";
    needle.width = 2.f;
    PartSpec handle{PartKind::Glyph, "handle"};
    handle.glyph  = "diamond";
    handle.color  = "#dcb43c";
    handle.size   = 5.f;
    PartSpec label = TextPart("label", "$text", 0.f);
    label.size.reset();
    label.scale = 0.9f;
    PartSpec reading = TextPart("reading", "$muted", 0.f);
    reading.size.reset();
    rim.content = {needle, handle, label, reading};
    return rim;
}

NodeKind DefaultKind(const NodeType& t, const Style& s)
{
    if (t.shape == NodeShape::Group) return DefaultGroupKind(t, s);
    if (t.shape == NodeShape::Pill) return DefaultPillKind(t, s);
    NodeKind k;
    k.id               = t.id;
    PartSpec& card     = k.card;
    card.fill          = t.shape == NodeShape::Note ? "$noteFill" : "$nodeFill";
    card.stroke        = "$nodeStroke";
    card.strokeWidth   = 1.f;
    card.corner        = s.corner;
    PartSpec accent{PartKind::Accent, "accent"};
    accent.width = s.band;
    accent.fill  = "band";
    PartSpec title{PartKind::Text, "title"};
    title.from             = t.titleField.empty() ? std::string() : "field:" + t.titleField;
    title.placeholder      = "Untitled {type}";
    title.placeholderColor = "$muted";
    title.color            = "$text";
    title.size             = s.titleSize;
    title.scale            = 1.f;
    title.box              = 1.3f;
    title.advance          = 1.35f;
    title.bold             = true;
    PartSpec type{PartKind::Text, "type"};
    type.from    = "type";
    type.color   = "band";
    type.size    = s.fontSize;
    type.scale   = 0.9f;
    type.box     = 1.3f;
    type.advance = 1.5f;
    PartSpec header{PartKind::Header, "header"};
    header.top     = 0.8f;
    header.content = {title, type};
    card.content   = {accent, header, PartSpec{PartKind::Body, "body"}};
    if (t.shape == NodeShape::Dial) card.content.push_back(DefaultRim());
    return k;
}

// Tags, then states, then the kind: an exact kind over a prefix kind (`card:*`) over none.
int Specificity(const Selector& s)
{
    const int kind = s.kind.empty() ? 0 : s.kind.back() == '*' ? 1 : 2;
    return static_cast<int>(s.tags.size()) * 1024 + std::popcount(s.states) * 4 + kind;
}

bool Matches(const Selector& s, std::string_view kind, const std::vector<std::string>& tags, std::uint32_t states)
{
    if (!s.kind.empty() && !KindCovers(s.kind, kind)) return false;
    if ((s.states & states) != s.states) return false;
    for (const std::string& t : s.tags)
        if (std::find(tags.begin(), tags.end(), t) == tags.end()) return false;
    return true;
}

PartSpec* FindPart(PartSpec& root, std::string_view id)
{
    if (root.id == id) return &root;
    for (PartSpec& c : root.content)
        if (PartSpec* p = FindPart(c, id)) return p;
    return nullptr;
}

void Apply(const PartSpec& patch, PartSpec& to)
{
    auto set = [](const auto& from, auto& into) {
        if (from) into = from;
    };
    set(patch.visible, to.visible);
    set(patch.fill, to.fill);
    set(patch.stroke, to.stroke);
    set(patch.color, to.color);
    set(patch.strokeWidth, to.strokeWidth);
    set(patch.corner, to.corner);
    set(patch.opacity, to.opacity);
    set(patch.dash, to.dash);
    set(patch.width, to.width);
    set(patch.top, to.top);
    set(patch.height, to.height);
    set(patch.from, to.from);
    set(patch.placeholder, to.placeholder);
    set(patch.placeholderColor, to.placeholderColor);
    set(patch.font, to.font);
    set(patch.size, to.size);
    set(patch.scale, to.scale);
    set(patch.box, to.box);
    set(patch.advance, to.advance);
    set(patch.bold, to.bold);
    set(patch.italic, to.italic);
    set(patch.align, to.align);
    set(patch.shape, to.shape);
    set(patch.format, to.format);
    set(patch.pin, to.pin);
    set(patch.glyph, to.glyph);
    set(patch.hollow, to.hollow);
    set(patch.side, to.side);
    set(patch.length, to.length);
    set(patch.direction, to.direction);
    set(patch.band, to.band);
    set(patch.chamfer, to.chamfer);
}

void ApplyPin(const PinSpec& patch, PinSpec& to)
{
    auto set = [](const auto& from, auto& into) {
        if (from) into = from;
    };
    set(patch.glyph, to.glyph);
    set(patch.size, to.size);
    set(patch.fill, to.fill);
    set(patch.color, to.color);
    set(patch.hollow, to.hollow);
    set(patch.hit, to.hit);
    set(patch.label, to.label);
    for (const auto& [g, a] : patch.on) to.on[g] = a;
}

void ApplyRow(const RowSpec& patch, RowSpec& to)
{
    auto set = [](const auto& from, auto& into) {
        if (from) into = from;
    };
    set(patch.color, to.color);
    set(patch.valueColor, to.valueColor);
    set(patch.prefix, to.prefix);
    set(patch.suffix, to.suffix);
    set(patch.align, to.align);
    set(patch.valueAlign, to.valueAlign);
    set(patch.bold, to.bold);
    set(patch.italic, to.italic);
    set(patch.chip, to.chip);
    set(patch.editor, to.editor);
    set(patch.min, to.min);
    set(patch.max, to.max);
    set(patch.step, to.step);
    set(patch.reorder, to.reorder);
    for (const auto& [g, a] : patch.on) to.on[g] = a;
}

void ApplyEdge(const EdgeSpec& patch, EdgeSpec& to)
{
    auto set = [](const auto& from, auto& into) {
        if (from) into = from;
    };
    // `line` is the kind's alone: the route follows it, and a rule must not reroute a wire by its state.
    set(patch.width, to.width);
    set(patch.widen, to.widen);
    set(patch.dash, to.dash);
    set(patch.dotted, to.dotted);
    set(patch.onTop, to.onTop);
    set(patch.arrow, to.arrow);
    set(patch.arrowSize, to.arrowSize);
    set(patch.color, to.color);
    set(patch.pinColor, to.pinColor);
    set(patch.opacity, to.opacity);
    set(patch.labelColor, to.labelColor);
    set(patch.labelFill, to.labelFill);
    set(patch.labelScale, to.labelScale);
}

constexpr std::pair<std::string_view, EdgeLine> kEdgeLines[] = {{"curve", EdgeLine::Curve}, {"straight", EdgeLine::Straight}, {"elbow", EdgeLine::Elbow}};

std::string_view LineName(EdgeLine l)
{
    for (const auto& [name, v] : kEdgeLines)
        if (v == l) return name;
    return "curve";
}

EdgeKind DefaultEdgeKind(const EdgeType& t)
{
    EdgeKind k;
    k.id               = t.id;
    k.spec.line        = std::string(LineName(t.line));
    k.spec.width       = t.width;
    k.spec.color       = t.color.ToHex();
    if (t.dashed) k.spec.dash = true;
    if (t.dotted) k.spec.dotted = true;
    if (t.onTop) k.spec.onTop = true;
    if (t.portColor) k.spec.pinColor = true;
    k.spec.arrow      = t.arrow ? "end" : "none";
    k.spec.labelColor = "$muted";
    k.spec.labelFill  = "$background";
    return k;
}

int AlignOf(const std::optional<std::string>& a, int fallback)
{
    if (!a) return fallback;
    return *a == "center" ? 1 : *a == "right" ? 2 : 0;
}

// ── colours ──

struct Hsv {
    float h = 0.f, s = 0.f, v = 0.f;   ///< h in degrees (-1 for none), s and v 0..255
};

Hsv ToHsv(Color c)
{
    const float r = c.r, g = c.g, b = c.b;
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    Hsv out{-1.f, mx > 0.f ? d / mx * 255.f : 0.f, mx};
    if (d > 0.f) {
        float h = mx == r ? (g - b) / d : mx == g ? 2.f + (b - r) / d : 4.f + (r - g) / d;
        h *= 60.f;
        if (h < 0.f) h += 360.f;
        out.h = h;
    }
    return out;
}

Color FromHsv(Hsv x, std::uint8_t a)
{
    auto byte = [](float f) { return static_cast<std::uint8_t>(std::lround(std::clamp(f, 0.f, 255.f))); };
    if (x.h < 0.f || x.s <= 0.f) return {byte(x.v), byte(x.v), byte(x.v), a};
    const float s = x.s / 255.f, v = x.v / 255.f, h = std::fmod(x.h, 360.f) / 60.f;
    const int   i = static_cast<int>(h);
    const float f = h - static_cast<float>(i), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float       r = 0, g = 0, b = 0;
    switch (i) {
    case 0: r = v, g = t, b = p; break;
    case 1: r = q, g = v, b = p; break;
    case 2: r = p, g = v, b = t; break;
    case 3: r = p, g = q, b = v; break;
    case 4: r = t, g = p, b = v; break;
    default: r = v, g = p, b = q; break;
    }
    return {byte(r * 255.f), byte(g * 255.f), byte(b * 255.f), a};
}

/// @brief Qt's QColor::lighter: value scaled by `factor` percent; past white, the saturation gives way.
Color Lighter(Color c, float factor)
{
    if (factor <= 0.f) return c;
    Hsv x = ToHsv(c);
    x.v   = x.v * factor / 100.f;   // under 100 that darkens, as Qt's does
    if (x.v > 255.f) {
        x.s = std::max(0.f, x.s - (x.v - 255.f));
        x.v = 255.f;
    }
    return FromHsv(x, c.a);
}

Color Darker(Color c, float factor)
{
    if (factor <= 0.f) return c;
    Hsv x = ToHsv(c);
    x.v   = x.v * 100.f / factor;
    return FromHsv(x, c.a);
}

class ColorParser {
public:
    ColorParser(std::string_view text, Color band, const Kit& kit, const std::vector<std::string>& tags, int depth)
        : s_(text), band_(band), kit_(kit), tags_(tags), depth_(depth)
    {
    }

    std::optional<Color> Run()
    {
        auto c = Expr();
        Skip();
        if (!c || i_ != s_.size()) return std::nullopt;
        return c;
    }

private:
    void Skip()
    {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    std::string_view Word()
    {
        const std::size_t b = i_;
        while (i_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[i_])) || s_[i_] == '_' || s_[i_] == '-' || s_[i_] == '.')) ++i_;
        return s_.substr(b, i_ - b);
    }
    std::optional<float> Number()
    {
        Skip();
        float      v   = 0.f;
        const auto res = std::from_chars(s_.data() + i_, s_.data() + s_.size(), v);
        if (res.ec != std::errc{}) return std::nullopt;
        i_ = static_cast<std::size_t>(res.ptr - s_.data());
        Skip();
        if (i_ < s_.size() && s_[i_] == '%') ++i_;
        return v;
    }
    bool Eat(char ch)
    {
        Skip();
        if (i_ < s_.size() && s_[i_] == ch) return ++i_, true;
        return false;
    }
    std::optional<Color> Expr()
    {
        Skip();
        if (i_ >= s_.size()) return std::nullopt;
        if (s_[i_] == '#') {
            const std::size_t b = i_++;
            while (i_ < s_.size() && std::isxdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
            return ParseColor(s_.substr(b, i_ - b));
        }
        if (s_[i_] == '$') {
            ++i_;
            const std::string_view name = Word();
            const auto             it   = kit_.tokens.find(name);
            if (it == kit_.tokens.end() || depth_ > 8) return std::nullopt;
            return ColorParser(it->second, band_, kit_, tags_, depth_ + 1).Run();
        }
        const std::string_view word = Word();
        if (word == "band") return band_;
        if (word == "none") return Color{0, 0, 0, 0};
        if (word == "palette") return Palette();
        if (word != "lighter" && word != "darker" && word != "alpha") return std::nullopt;
        if (!Eat('(')) return std::nullopt;
        const auto c = Expr();
        if (!c || !Eat(',')) return std::nullopt;
        const auto n = Number();
        if (!n || !Eat(')')) return std::nullopt;
        if (word == "lighter") return Lighter(*c, *n);
        if (word == "darker") return Darker(*c, *n);
        Color a = *c;
        a.a     = static_cast<std::uint8_t>(std::lround(std::clamp(*n <= 1.f ? *n * 255.f : *n, 0.f, 255.f)));
        return a;
    }
    // `palette(name, tag:key)`: the entry the node's `key:<value>` tag picks; with no such tag, the band.
    std::optional<Color> Palette()
    {
        if (!Eat('(')) return std::nullopt;
        Skip();
        const std::string_view name = Word();
        if (name.empty() || !Eat(',')) return std::nullopt;
        Skip();
        if (Word() != "tag" || !Eat(':')) return std::nullopt;
        Skip();
        const std::string_view key = Word();
        if (key.empty() || !Eat(')')) return std::nullopt;
        const auto list = kit_.palettes.find(name);
        if (list == kit_.palettes.end() || depth_ > 8) return std::nullopt;
        const std::string* value = nullptr;
        for (const std::string& t : tags_)
            if (t.size() > key.size() && t.starts_with(key) && t[key.size()] == ':') {
                value = &t;
                break;
            }
        if (!value || list->second.empty()) {
            // Still check every entry reads, so a broken palette fails when the kit loads.
            for (const std::string& e : list->second)
                if (!ColorParser(e, band_, kit_, tags_, depth_ + 1).Run()) return std::nullopt;
            return band_;
        }
        const std::string_view v = std::string_view(*value).substr(key.size() + 1);
        std::uint64_t          slot = 0;
        const auto             res  = std::from_chars(v.data(), v.data() + v.size(), slot);
        if (res.ec != std::errc{} || res.ptr != v.data() + v.size()) {
            slot = 14695981039346656037ull;   // FNV-1a over its text: the same value, the same entry, on every machine
            for (const char ch : v) slot = (slot ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
        }
        return ColorParser(list->second[slot % list->second.size()], band_, kit_, tags_, depth_ + 1).Run();
    }

    std::string_view                s_;
    std::size_t                     i_ = 0;
    Color                           band_;
    const Kit&                      kit_;
    const std::vector<std::string>& tags_;
    int                             depth_;
};

Color Faded(Color c, float opacity)
{
    if (opacity >= 1.f) return c;
    c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * std::clamp(opacity, 0.f, 1.f)));
    return c;
}

} // namespace

std::optional<Color> ResolveColor(std::string_view expr, Color band, const Kit& kit, const std::vector<std::string>& tags)
{
    return ColorParser(expr, band, kit, tags, 0).Run();
}

std::optional<Selector> ParseSelector(std::string_view text)
{
    Selector    out;
    std::size_t i    = 0;
    auto        skip = [&] {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    };
    auto word = [&] {
        const std::size_t b = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != '[' && text[i] != ':' && text[i] != '>' &&
               text[i] != ']')
            ++i;
        return std::string(text.substr(b, i - b));
    };
    static constexpr std::pair<std::string_view, std::uint32_t> kStates[] = {
        {"selected", StateSelected}, {"hovered", StateHovered},     {"dimmed", StateDimmed},   {"alias", StateAlias},
        {"collapsed", StateCollapsed}, {"inGroup", StateInGroup},   {"wired", StateWired},     {"unwired", StateUnwired},
        {"lit", StateLit},           {"merged", StateMerged}};
    auto stateOf = [](std::string_view name) -> std::uint32_t {
        for (const auto& [n, bit] : kStates)
            if (n == name) return bit;
        return 0;
    };
    skip();
    // The kind runs to a tag, a part or a space and may hold `:` (`card:Combat`); its trailing `:name`s that name states
    // are states.
    {
        const std::size_t b = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != '[' && text[i] != '>' && text[i] != ']') ++i;
        std::string_view kind = text.substr(b, i - b);
        std::uint32_t    tail = 0;
        for (std::size_t colon; (colon = kind.rfind(':')) != std::string_view::npos;) {
            const std::uint32_t bit = stateOf(kind.substr(colon + 1));
            if (!bit) break;
            tail |= bit;
            kind = kind.substr(0, colon);
        }
        if (!kind.empty() && kind.back() == ':') return std::nullopt;
        out.kind   = std::string(kind);
        out.states = tail;
    }
    if (out.kind == "*") out.kind.clear();
    for (;;) {
        skip();
        if (i >= text.size()) break;
        if (text[i] == '[') {
            const std::size_t close = text.find(']', i);
            if (close == std::string_view::npos || close == i + 1) return std::nullopt;
            out.tags.emplace_back(text.substr(i + 1, close - i - 1));
            i = close + 1;
        } else if (text[i] == ':') {
            ++i;
            const std::uint32_t bit = stateOf(word());
            if (!bit) return std::nullopt;
            out.states |= bit;
        } else if (text[i] == '>') {
            ++i;
            skip();
            // A part is a name, or a kind of pin or row (`pin:<kind>`, `row:<kind>`): the rest of the selector.
            const std::size_t b = i;
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            out.part = std::string(text.substr(b, i - b));
            skip();
            if (out.part.empty() || i != text.size()) return std::nullopt;
            break;
        } else {
            return std::nullopt;
        }
    }
    return out;
}

Kit KitFromSchema(const Schema& schema, const Style& style)
{
    Kit kit;
    kit.tokens = {{"background", style.background.ToHex()}, {"nodeFill", style.nodeFill.ToHex()},     {"noteFill", style.noteFill.ToHex()},
                  {"nodeStroke", style.nodeStroke.ToHex()}, {"selection", style.selection.ToHex()},   {"text", style.text.ToHex()},
                  {"muted", style.muted.ToHex()}};
    kit.metrics = {style.padding, style.titleSize, style.fontSize, std::nullopt, std::nullopt, style.groupHeader};
    kit.nodes.reserve(schema.nodeTypes.size() + 1);
    kit.nodes.push_back(DefaultKind(NodeType{}, style));   // an unknown type
    for (const NodeType& t : schema.nodeTypes) kit.nodes.push_back(DefaultKind(t, style));
    // The outline: muted under the pointer, the selection's colour and twice as wide when selected.
    Rule hovered{Selector{{}, {}, StateHovered}, {}};
    hovered.patch.stroke = "$muted";
    Rule selected{Selector{{}, {}, StateSelected}, {}};
    selected.patch.stroke      = "$selection";
    selected.patch.strokeWidth = 2.f;
    kit.rules = {hovered, selected};
    for (const NodeType& t : schema.nodeTypes) {
        if (t.shape != NodeShape::Group && t.shape != NodeShape::Pill) continue;
        Rule hov{Selector{t.id, {}, StateHovered}, {}};
        hov.patch.stroke = "$text";
        Rule sel{Selector{t.id, {}, StateSelected}, {}};
        sel.patch.stroke      = "$selection";
        sel.patch.strokeWidth = 2.f;
        kit.rules.push_back(std::move(hov));
        kit.rules.push_back(std::move(sel));
    }
    // Edges: each type as it draws; touching the selection, in its colour and a little wider; selected, wider still;
    // standing for several that join a closed group, dashed.
    std::vector<EdgeType> edgeTypes = schema.edgeTypes;
    edgeTypes.insert(edgeTypes.begin(), EdgeType{});   // an unknown type
    for (const EdgeType& t : edgeTypes) {
        kit.edges.push_back(DefaultEdgeKind(t));
        const std::string part = "edge:" + t.id;
        Rule lit{Selector{{}, {}, StateLit, part}, {}};
        lit.edge.color    = "$selection";
        lit.edge.pinColor = false;
        lit.edge.widen    = 0.7f;
        Rule sel{Selector{{}, {}, StateSelected, part}, {}};
        sel.edge.color    = "$selection";
        sel.edge.pinColor = false;
        sel.edge.widen    = 1.f;
        Rule merged{Selector{{}, {}, StateMerged, part}, {}};
        merged.edge.dash = true;
        kit.rules.push_back(std::move(lit));
        kit.rules.push_back(std::move(sel));
        kit.rules.push_back(std::move(merged));
    }
    for (const RowStyle& rs : schema.rowStyles) {
        RowKind k;
        k.id = rs.id;
        if (rs.color) k.spec.color = rs.color->ToHex();
        if (rs.valueColor) k.spec.valueColor = rs.valueColor->ToHex();
        if (!rs.prefix.empty()) k.spec.prefix = rs.prefix;
        if (rs.bold) k.spec.bold = true;
        if (rs.italic) k.spec.italic = true;
        if (rs.chip) k.spec.chip = true;
        kit.rows.push_back(std::move(k));
    }
    kit.revision = schema.revision;
    return kit;
}

Kit MergeKits(const Kit& under, const Kit& over)
{
    Kit out = under;
    // A prefix kind of `over` stands for the exact kinds of `under` it covers: they go, with the rules that select them.
    auto covered = [](const auto& overKinds, std::string_view id) {
        if (!id.empty() && id.back() == '*') return false;
        return std::any_of(overKinds.begin(), overKinds.end(), [&](const auto& k) { return k.id != id && KindCovers(k.id, id); });
    };
    auto dropCovered = [&](auto& into, const auto& from) {
        std::erase_if(into, [&](const auto& k) { return covered(from, k.id); });
    };
    std::erase_if(out.rules, [&](const Rule& r) {
        const std::string_view part = r.select.part;
        if (part.starts_with("edge:")) return covered(over.edges, part.substr(5));
        if (part.starts_with("pin:")) return covered(over.pins, part.substr(4));
        return !r.select.kind.empty() && covered(over.nodes, r.select.kind);
    });
    dropCovered(out.nodes, over.nodes);
    dropCovered(out.pins, over.pins);
    dropCovered(out.edges, over.edges);
    for (const auto& [k, v] : over.palettes) out.palettes[k] = v;
    for (const auto& [k, v] : over.tokens) out.tokens[k] = v;
    for (const auto& [k, v] : over.fonts) out.fonts[k] = v;
    auto set = [](const std::optional<float>& from, std::optional<float>& into) {
        if (from) into = from;
    };
    set(over.metrics.padding, out.metrics.padding);
    set(over.metrics.titleSize, out.metrics.titleSize);
    set(over.metrics.fontSize, out.metrics.fontSize);
    set(over.metrics.header, out.metrics.header);
    set(over.metrics.row, out.metrics.row);
    set(over.metrics.groupHeader, out.metrics.groupHeader);
    for (const NodeKind& k : over.nodes) {
        auto it = std::find_if(out.nodes.begin(), out.nodes.end(), [&](const NodeKind& n) { return n.id == k.id; });
        if (it != out.nodes.end()) *it = k;
        else out.nodes.push_back(k);
    }
    auto merge = [](auto& into, const auto& from) {
        for (const auto& k : from) {
            auto it = std::find_if(into.begin(), into.end(), [&](const auto& n) { return n.id == k.id; });
            if (it != into.end()) *it = k;
            else into.push_back(k);
        }
    };
    merge(out.pins, over.pins);
    merge(out.rows, over.rows);
    merge(out.edges, over.edges);
    out.rules.insert(out.rules.end(), over.rules.begin(), over.rules.end());
    for (const auto& [k, v] : over.reserved) out.reserved[k] = v;
    out.revision = under.revision + over.revision;
    return out;
}

NodeLook ResolveLook(const Kit& kit, const NodeType& type, const std::vector<std::string>& tags, std::uint32_t states, const CardMetrics& base)
{
    static const NodeKind kEmpty;
    const NodeKind* kind = kit.FindNode(type.id);
    if (!kind) kind = kit.FindNode("");
    if (!kind) kind = &kEmpty;
    PartSpec root = kind->card;

    // Rules in order of specificity, file order among equals; each patches the part it selects.
    std::vector<const Rule*> order;
    order.reserve(kit.rules.size());
    for (const Rule& r : kit.rules) order.push_back(&r);
    std::stable_sort(order.begin(), order.end(), [](const Rule* a, const Rule* b) { return Specificity(a->select) < Specificity(b->select); });
    for (const Rule* r : order) {
        if (!Matches(r->select, type.id, tags, states)) continue;
        if (PartSpec* target = r->select.part.empty() ? &root : FindPart(root, r->select.part)) Apply(r->patch, *target);
    }

    // `band` for this node: the type's, or what the card's `band` makes of it (a fixed colour, a palette slot).
    const Color band = root.band ? ResolveColor(*root.band, type.band, kit, tags).value_or(type.band) : type.band;
    auto color = [&](const std::optional<std::string>& expr, Color fallback) {
        if (!expr) return fallback;
        return ResolveColor(*expr, band, kit, tags).value_or(fallback);
    };
    auto shows = [&](const PartSpec& p) {
        if (p.visible && !*p.visible) return false;
        if (p.condition.empty()) return true;
        const auto when = ParseSelector(p.condition);
        return when && Matches(*when, type.id, tags, states);
    };

    NodeLook look;
    CardMetrics& m = look.metrics;
    m              = base;
    for (const MetricsSpec* spec : {&kit.metrics, &kind->metrics}) {
        if (spec->padding) m.padding = *spec->padding;
        if (spec->titleSize) m.titleSize = *spec->titleSize;
        if (spec->fontSize) m.fontSize = *spec->fontSize;
        if (spec->header) m.header = *spec->header;
        if (spec->row) m.row = *spec->row;
        if (spec->groupHeader) m.groupHeader = *spec->groupHeader;
    }
    const float opacity = root.opacity.value_or(1.f);
    look.band        = band;
    look.fill        = Faded(color(root.fill, Color{0, 0, 0, 0}), opacity);
    look.stroke      = Faded(color(root.stroke, Color{0, 0, 0, 0}), opacity);
    look.strokeWidth = root.strokeWidth.value_or(1.f);
    look.corner      = root.corner.value_or(6.f);
    look.dash        = root.dash.value_or(false);
    look.minWidth    = kind->minWidth.value_or(0.f);
    look.maxWidth    = kind->maxWidth.value_or(0.f);
    look.fitWidth    = kind->fitWidth.value_or(false);
    static constexpr std::pair<std::string_view, PortGlyph> kPartGlyphs[] = {
        {"circle", PortGlyph::Circle}, {"square", PortGlyph::Square}, {"diamond", PortGlyph::Diamond}, {"arrow", PortGlyph::Arrow}};
    auto textOf = [&](const PartSpec& c) {
        LookText text;
        text.id          = c.id;
        text.from        = c.from.value_or(std::string());
        text.placeholder = c.placeholder.value_or(std::string());
        const FontSpec* font = nullptr;
        if (c.font)
            if (const auto it = kit.fonts.find(*c.font); it != kit.fonts.end()) font = &it->second;
        text.size    = c.size ? *c.size : font && font->size ? *font->size : m.fontSize;
        text.bold    = c.bold ? *c.bold : font && font->bold ? *font->bold : false;
        text.italic  = c.italic ? *c.italic : font && font->italic ? *font->italic : false;
        text.scale   = c.scale.value_or(1.f);
        text.box     = c.box.value_or(1.3f);
        text.advance = c.advance.value_or(1.35f);
        text.color            = Faded(color(c.color, Color{226, 228, 233}), opacity);
        text.placeholderColor = Faded(color(c.placeholderColor, text.color), opacity);
        const std::string align = c.align.value_or("left");
        text.align              = align == "center" ? 1 : align == "right" ? 2 : 0;
        text.format             = c.format.value_or(std::string());
        return text;
    };
    auto partOf = [&](const auto& self, const PartSpec& p) -> LookPart {
        LookPart part;
        part.kind        = p.kind;
        part.id          = p.id;
        part.fill        = Faded(color(p.fill, Color{0, 0, 0, 0}), opacity);
        part.stroke      = Faded(color(p.stroke, Color{0, 0, 0, 0}), opacity);
        part.strokeWidth = p.strokeWidth.value_or(0.f);
        part.corner      = p.corner.value_or(look.corner);
        part.width       = p.width.value_or(0.f);
        part.top         = p.top.value_or(0.8f);
        part.height      = p.height.value_or(0.f);
        part.dash        = p.dash.value_or(false);
        part.on          = p.on;
        part.color       = Faded(color(p.color, part.color), opacity);
        if (p.glyph)
            for (const auto& [name, g] : kPartGlyphs)
                if (name == *p.glyph) part.glyph = g;
        part.size   = p.size.value_or(0.f);
        part.length = p.length.value_or(0.f);
        part.hollow = p.hollow.value_or(false);
        part.out    = p.direction.value_or("in") == "out";
        part.side   = p.side.value_or("left") == "right" ? 1 : 0;
        part.pin    = p.pin.value_or(std::string());
        // A text or a chip is its own line; a header's and a rim's lines are their text parts.
        if (p.kind == PartKind::Text || p.kind == PartKind::Chip) part.texts.push_back(textOf(p));
        for (const PartSpec& c : p.content) {
            if (!shows(c)) continue;
            if (c.kind == PartKind::Text) part.texts.push_back(textOf(c));
            else part.content.push_back(self(self, c));
        }
        return part;
    };
    for (const PartSpec& p : root.content) {
        if (!shows(p)) continue;
        look.parts.push_back(partOf(partOf, p));
        if (p.kind == PartKind::Pins && p.id == "ends") {
            look.endPins = true;
            look.endPin  = p.pin.value_or(std::string());
        }
    }
    look.shape = root.shape.value_or("box") == "pill" ? 1 : 0;
    // An octagon rim makes the dial that shape: its geometry, which pins, hits and the layout read, says so.
    for (const PartSpec& p : root.content)
        if (p.kind == PartKind::Rim && shows(p) && p.shape.value_or("circle") == "octagon") m.dialChamfer = std::max(0.f, p.chamfer.value_or(0.3f));
    // A header of a stated height is where the rows start: pins, hits and docks read it from the metrics.
    for (const LookPart& p : look.parts)
        if (p.kind == PartKind::Header && p.height > 0.f && m.header <= 0.f) m.header = p.height;
    look.on = root.on;

    // Pin kinds, unwired and wired, each patched by the rules on `pin:<kind>` this node (in that state) matches.
    static constexpr std::pair<std::string_view, PortGlyph> kGlyphs[] = {
        {"circle", PortGlyph::Circle}, {"square", PortGlyph::Square}, {"diamond", PortGlyph::Diamond}, {"arrow", PortGlyph::Arrow}};
    for (const PinKind& pk : kit.pins)
        for (int wired = 0; wired < 2; ++wired) {
            PinSpec            spec  = pk.spec;
            const std::uint32_t with = states | (wired ? StateWired : StateUnwired);
            for (const Rule* r : order)
                if (r->select.part == "pin:" + pk.id && Matches(r->select, type.id, tags, with)) ApplyPin(r->pin, spec);
            PinLook& pin = look.pins[pk.id][static_cast<std::size_t>(wired)];
            if (spec.glyph)
                for (const auto& [name, g] : kGlyphs)
                    if (name == *spec.glyph) pin.glyph = g;
            pin.size  = spec.size.value_or(0.f);
            pin.fill  = !spec.fill || *spec.fill == "wired" ? 0 : *spec.fill == "always" ? 1 : 2;
            if (spec.color) pin.color = ResolveColor(*spec.color, band, kit, tags);
            if (spec.hollow) pin.hollow = ResolveColor(*spec.hollow, band, kit, tags);
            pin.hit   = spec.hit.value_or(4.f);
            pin.label = spec.label.value_or(true);
            pin.on    = spec.on;
        }
    // Row kinds, each patched by the rules on `row:<kind>` this node matches.
    for (const RowKind& rk : kit.rows) {
        RowSpec spec = rk.spec;
        for (const Rule* r : order)
            if (r->select.part == "row:" + rk.id && Matches(r->select, type.id, tags, states)) ApplyRow(r->row, spec);
        RowLook& row = look.rows[rk.id];
        if (spec.color) row.color = ResolveColor(*spec.color, band, kit, tags);
        if (spec.valueColor) row.valueColor = ResolveColor(*spec.valueColor, band, kit, tags);
        row.prefix     = spec.prefix.value_or(std::string());
        row.suffix     = spec.suffix.value_or(std::string());
        row.align      = AlignOf(spec.align, 0);
        row.valueAlign = AlignOf(spec.valueAlign, 2);
        row.bold       = spec.bold.value_or(false);
        row.italic     = spec.italic.value_or(false);
        row.chip       = spec.chip.value_or(false);
        row.editor     = spec.editor.value_or(std::string());
        row.min        = spec.min.value_or(0.f);
        row.max        = spec.max.value_or(0.f);
        row.step       = spec.step.value_or(0.f);
        row.reorder    = spec.reorder.value_or(std::string());
        row.on         = spec.on;
    }
    return look;
}

} // namespace grapher

namespace grapher {

const EdgeKind* Kit::FindEdge(std::string_view id) const { return BestKind(edges, id); }

EdgeLook ResolveEdgeLook(const Kit& kit, const EdgeType& type, const std::vector<std::string>& tags, std::uint32_t states)
{
    const EdgeKind* kind = kit.FindEdge(type.id);
    if (!kind) kind = kit.FindEdge("");
    EdgeSpec        spec = kind ? kind->spec : DefaultEdgeKind(type).spec;
    const std::string part = "edge:" + (kind ? kind->id : type.id);
    std::vector<const Rule*> order;
    for (const Rule& r : kit.rules)
        if (r.select.part == part) order.push_back(&r);
    std::stable_sort(order.begin(), order.end(), [](const Rule* a, const Rule* b) { return Specificity(a->select) < Specificity(b->select); });
    for (const Rule* r : order)
        if (Matches(r->select, type.id, tags, states)) ApplyEdge(r->edge, spec);
    auto color = [&](const std::optional<std::string>& expr, Color fallback) {
        if (!expr) return fallback;
        return ResolveColor(*expr, type.color, kit, tags).value_or(fallback);
    };
    EdgeLook look;
    if (spec.line)
        for (const auto& [name, v] : kEdgeLines)
            if (name == *spec.line) look.line = v;
    look.opacity    = spec.opacity.value_or(1.f);
    look.color      = Faded(color(spec.color, type.color), look.opacity);
    look.pinColor   = spec.pinColor.value_or(false);
    look.width      = spec.width.value_or(1.5f) + spec.widen.value_or(0.f);
    look.arrowSize  = spec.arrowSize.value_or(9.f);
    look.dash       = spec.dash.value_or(false);
    look.dotted     = spec.dotted.value_or(false);
    look.onTop      = spec.onTop.value_or(false);
    const std::string arrow = spec.arrow.value_or("end");
    look.arrow      = arrow == "none" ? 0 : arrow == "start" ? 2 : arrow == "both" ? 3 : 1;
    look.labelColor = Faded(color(spec.labelColor, look.labelColor), look.opacity);
    look.labelFill  = color(spec.labelFill, look.labelFill);
    look.labelScale = spec.labelScale.value_or(0.9f);
    return look;
}

} // namespace grapher
