#include <grapher/Canvas.h>

#include <algorithm>
#include <bit>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <numbers>
#include <cstring>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace grapher {

namespace {

constexpr double kLiveRouteMs = 12.0;   ///< a route slower than this is not rerun on every step of a drag

std::size_t Mix(std::size_t h, std::size_t v) { return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2)); }
std::size_t Bits(float f) { return std::bit_cast<std::uint32_t>(f); }

/// @brief A fast hash of text, eight bytes at a time: the wire cache keys every id and type on every frame.
std::size_t HashText(std::string_view s)
{
    constexpr std::uint64_t k = 0x9fb21c651e98df25ull;
    std::uint64_t h = 0x9e3779b97f4a7c15ull ^ (s.size() * k);
    std::size_t   i = 0;
    for (; i + 8 <= s.size(); i += 8) {
        std::uint64_t w;
        std::memcpy(&w, s.data() + i, 8);
        h = (h ^ (w * k)) * 0xff51afd7ed558ccdull;
        h ^= h >> 29;
    }
    if (i < s.size()) {
        std::uint64_t w = 0;
        std::memcpy(&w, s.data() + i, s.size() - i);
        h = (h ^ (w * k)) * 0xff51afd7ed558ccdull;
        h ^= h >> 29;
    }
    return static_cast<std::size_t>(h ^ (h >> 32));
}

std::string PortKey(std::string_view node, std::string_view port)
{
    std::string k(node);
    k += '\x1f';
    k += port;
    return k;
}

constexpr float kHandleAt = 0.8f;   ///< a dial's handles sit this far out along its radius

PortSide DockSideOf(PortSide s) { return s == PortSide::Auto ? PortSide::Bottom : s; }
int      DockSlotOf(PortSide s) { return s == PortSide::Bottom ? 0 : s == PortSide::Top ? 1 : s == PortSide::Right ? 2 : 3; }

float Overlap(const Rect& a, const Rect& b)
{
    const float w = std::min(a.Right(), b.Right()) - std::max(a.x, b.x);
    const float h = std::min(a.Bottom(), b.Bottom()) - std::max(a.y, b.y);
    return w > 0.f && h > 0.f ? w * h : 0.f;
}

/// @brief A graph with more nodes than this drags by rerouting only the dragged nodes' wires.
constexpr std::size_t kLargeGraph = 2000;

/// @brief The point halfway along a polyline, by length.
Vec2 Midpoint(const std::vector<Vec2>& pts)
{
    float total = 0.f;
    for (std::size_t i = 1; i < pts.size(); ++i) total += Distance(pts[i - 1], pts[i]);
    float left = total * 0.5f;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const float seg = Distance(pts[i - 1], pts[i]);
        if (seg >= left && seg > 0.f) return pts[i - 1] + (pts[i] - pts[i - 1]) * (left / seg);
        left -= seg;
    }
    return pts.back();
}

constexpr float kDragThreshold = 3.f;   ///< screen px a press must travel before it becomes a gesture
constexpr float kEdgeHitPx     = 6.f;
constexpr int   kCurveSegments = 24;

enum Side { Left, Right, Top, Bottom };

struct Anchor {
    Vec2 p;
    Vec2 n;   ///< outward normal
};

Anchor SideAnchor(const Rect& r, Side s)
{
    switch (s) {
    case Left: return {{r.x, r.y + r.h * 0.5f}, {-1, 0}};
    case Right: return {{r.Right(), r.y + r.h * 0.5f}, {1, 0}};
    case Top: return {{r.x + r.w * 0.5f, r.y}, {0, -1}};
    default: return {{r.x + r.w * 0.5f, r.Bottom()}, {0, 1}};
    }
}

/// @brief The sides two rectangles face each other on: across when they sit side by side, else above and below.
std::pair<Side, Side> FacingSides(const Rect& a, const Rect& b)
{
    const Vec2 d = b.Center() - a.Center();
    if (std::abs(d.x) * (a.h + b.h) >= std::abs(d.y) * (a.w + b.w))
        return d.x >= 0 ? std::pair{Right, Left} : std::pair{Left, Right};
    return d.y >= 0 ? std::pair{Bottom, Top} : std::pair{Top, Bottom};
}

std::vector<Vec2> Route(Anchor a, Anchor b, EdgeLine line)
{
    switch (line) {
    case EdgeLine::Straight: return {a.p, b.p};
    case EdgeLine::Elbow:
        if (a.n.x != 0.f) {
            const float mx = (a.p.x + b.p.x) * 0.5f;
            return {a.p, {mx, a.p.y}, {mx, b.p.y}, b.p};
        } else {
            const float my = (a.p.y + b.p.y) * 0.5f;
            return {a.p, {a.p.x, my}, {b.p.x, my}, b.p};
        }
    case EdgeLine::Curve:
    default: {
        const float k  = std::max(30.f, Distance(a.p, b.p) * 0.4f);
        const Vec2  c1 = a.p + a.n * k;
        const Vec2  c2 = b.p + b.n * k;
        std::vector<Vec2> pts;
        pts.reserve(kCurveSegments + 1);
        for (int i = 0; i <= kCurveSegments; ++i) {
            const float t = static_cast<float>(i) / kCurveSegments, u = 1.f - t;
            pts.push_back(a.p * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + b.p * (t * t * t));
        }
        return pts;
    }
    }
}


float SegmentDistance(Vec2 p, Vec2 a, Vec2 b)
{
    const Vec2  ab = b - a;
    const float len2 = ab.x * ab.x + ab.y * ab.y;
    const float t = len2 > 0.f ? std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2, 0.f, 1.f) : 0.f;
    return Distance(p, a + ab * t);
}

float PolylineDistance(Vec2 p, const std::vector<Vec2>& pts)
{
    float best = 1e30f;
    for (std::size_t i = 1; i < pts.size(); ++i) best = std::min(best, SegmentDistance(p, pts[i - 1], pts[i]));
    return best;
}

std::vector<Vec2> ToScreen(const Camera& cam, std::vector<Vec2> pts)
{
    for (Vec2& p : pts) p = cam.ToScreen(p);
    return pts;
}

Polygon Arrowhead(const std::vector<Vec2>& pts, float size, Color color)
{
    const Vec2  tip = pts.back();
    Vec2        d   = tip - pts[pts.size() - 2];
    const float len = Length(d);
    if (len <= 0.001f) return {{}, color};
    d = d / len;
    const Vec2 perp{-d.y, d.x};
    const Vec2 base = tip - d * size;
    return {{tip, base + perp * (size * 0.5f), base - perp * (size * 0.5f)}, color};
}

float HandleRadius(const Style& s, float zoom) { return std::clamp(s.handle * zoom, 4.f, 7.f); }

float PinRadius(float zoom) { return std::clamp(4.5f * zoom, 2.5f, 6.f); }

/// @brief A port's pin at screen point `at`: filled while a wire uses it, hollow (filled with `hollow`) while none does.
/// @param dir the way a wire runs through it (an Arrow points along it)
void DrawPin(std::vector<DrawCmd>& c, Vec2 at, Vec2 dir, PortGlyph glyph, float r, Color color, Color hollow, bool filled)
{
    const Color fill = filled ? color : hollow;
    auto outline = [&](std::vector<Vec2> pts) {
        c.push_back(Polygon{pts, fill});
        pts.push_back(pts.front());
        c.push_back(Polyline{std::move(pts), color, 1.5f});
    };
    switch (glyph) {
    case PortGlyph::Square: c.push_back(FillRect{{at.x - r, at.y - r, 2 * r, 2 * r}, fill, color, 1.5f}); break;
    case PortGlyph::Diamond: {
        const float d = r * 1.3f;
        outline({{at.x, at.y - d}, {at.x + d, at.y}, {at.x, at.y + d}, {at.x - d, at.y}});
        break;
    }
    case PortGlyph::Arrow: {
        const Vec2 perp{-dir.y, dir.x};
        outline({at + dir * (r * 1.2f), at - dir * (r * 0.9f) + perp * r, at - dir * (r * 0.9f) - perp * r});
        break;
    }
    case PortGlyph::Circle:
    default: c.push_back(Circle{at, r, fill, color, 1.5f}); break;
    }
}

std::optional<PortDef> PortOf(const Node& n, const Schema& schema, std::string_view id)
{
    for (PortDef& p : PortsOf(n, schema))
        if (p.id == id) return std::move(p);
    return std::nullopt;
}

Vec2 ConnectHandle(const Rect& screen) { return {screen.Right(), screen.y + screen.h * 0.5f}; }

Rect ResizeGrip(const Rect& screen, const Style& s)
{
    return {screen.Right() - s.resizeGrip, screen.Bottom() - s.resizeGrip, s.resizeGrip, s.resizeGrip};
}

// ── Text lines and pill parts, from a look ──

/// @brief What a text line's source gives: a field, a row's label, the type's label, the on-card fields.
std::string SourceText(std::string_view from, const Node& n, const NodeType& t)
{
    if (from == "type") return t.label;
    if (from.starts_with("field:")) return n.Field(from.substr(6));
    if (from.starts_with("row:")) {
        const Row* row = n.FindRow(from.substr(4));
        return row ? row->label : std::string();
    }
    if (from == "fields" || from == "fields:all") {
        std::string out;
        for (const FieldDef& f : t.fields) {
            const std::string& v = n.Field(f.id);
            if (!f.onCard || v.empty() || (from == "fields" && f.id == t.titleField)) continue;
            if (!out.empty()) out += " · ";
            out += v;
        }
        return out;
    }
    return {};
}

struct LineExtras {
    int              count = 0;    ///< what a group holds
    std::string_view arrow;        ///< a collapsible group's ▾ or ▸ and a space
};

/// @return a text line's words (its source, else its placeholder, through its format) and whether it is the placeholder
std::pair<std::string, bool> LineText(const LookText& line, const Node& n, const NodeType& t, const LineExtras& x = {})
{
    std::string value = SourceText(line.from, n, t);
    const bool  empty = value.empty() && !line.placeholder.empty();
    if (empty) {
        value = line.placeholder;
        if (const auto at = value.find("{type}"); at != std::string::npos) value.replace(at, 6, t.label);
    }
    if (line.format.empty()) return {std::move(value), empty};
    std::string            out;
    const std::string_view f = line.format;
    for (std::size_t i = 0; i < f.size();) {
        if (f[i] == '{')
            if (const auto close = f.find('}', i); close != std::string_view::npos) {
                const std::string_view name = f.substr(i + 1, close - i - 1);
                bool                   known = true;
                if (name == "value") out += value;
                else if (name == "type") out += t.label;
                else if (name == "count") out += std::to_string(x.count);
                else if (name == "arrow") out += x.arrow;
                else if (name == "fields") out += SourceText("fields", n, t);
                else known = false;
                if (known) {
                    i = close + 1;
                    continue;
                }
            }
        out += f[i++];
    }
    return {std::move(out), empty};
}

struct PillItem {
    const LookPart* part = nullptr;
    Rect            box;
    std::string     text;
    bool            empty = false;
};

/// @brief A pill's parts laid out along its line in `r` (screen at zoom `z`, or world at 1): fixed-width parts (a bar, a
///        glyph, a chip) take their width, texts share what is left; a stub stands outside its end.
std::vector<PillItem> PillLayout(const Node& n, const NodeType& t, const NodeLook& look, const Rect& r, float z)
{
    std::vector<PillItem> items;
    const float pad   = look.metrics.padding * z;
    const Rect  inner{r.x + r.h * 0.4f, r.y, std::max(0.f, r.w - r.h * 0.8f), r.h};
    float       fixed = 0.f;
    int         flex = 0, flow = 0;
    for (const LookPart& p : look.parts) {
        PillItem it{&p, {}, {}, false};
        switch (p.kind) {
        case PartKind::Text:
        case PartKind::Chip:
            if (p.texts.empty()) continue;
            std::tie(it.text, it.empty) = LineText(p.texts.front(), n, t);
            if (p.kind == PartKind::Text) {
                ++flex;
            } else {
                const LookText& l = p.texts.front();
                it.box.w          = l.size * z * l.scale * 0.6f * static_cast<float>(it.text.size()) + pad;
                fixed += it.box.w;
            }
            break;
        case PartKind::Glyph:
            it.box.w = (p.size > 0.f ? p.size : 5.f) * 2.f * z;
            fixed += it.box.w;
            break;
        case PartKind::Bar:
            it.box.w = std::max(1.f, (p.width > 0.f ? p.width : 3.f) * z);
            fixed += it.box.w;
            break;
        case PartKind::Stub: {
            const float len = (p.length > 0.f ? p.length : 16.f) * z;
            it.box          = p.side == 1 ? Rect{r.Right(), r.y, len, r.h} : Rect{r.x - len, r.y, len, r.h};
            items.push_back(std::move(it));
            continue;
        }
        default:
            continue;
        }
        ++flow;
        items.push_back(std::move(it));
    }
    const float gap  = pad * 0.5f;
    const float gaps = flow > 1 ? gap * static_cast<float>(flow - 1) : 0.f;
    const float each = flex > 0 ? std::max(0.f, (inner.w - fixed - gaps) / static_cast<float>(flex)) : 0.f;
    float       x    = inner.x;
    if (flex == 0) x += std::max(0.f, (inner.w - fixed - gaps) * 0.5f);   // only fixed parts: centred
    for (PillItem& it : items) {
        const PartKind kind = it.part->kind;
        if (kind == PartKind::Stub) continue;
        const float w = kind == PartKind::Text ? each : it.box.w;
        const float h = kind == PartKind::Bar ? r.h * 0.6f : kind == PartKind::Chip ? r.h * 0.8f : kind == PartKind::Glyph ? w : r.h;
        it.box        = {x, r.y + (r.h - h) * 0.5f, w, h};
        x += w + gap;
    }
    return items;
}

void DrawPillParts(std::vector<DrawCmd>& c, const std::vector<PillItem>& items, const NodeLook& look, const Rect& r, float z, bool text)
{
    for (const PillItem& it : items) {
        const LookPart& p = *it.part;
        switch (p.kind) {
        case PartKind::Text:
            if (text) {
                const LookText& l = p.texts.front();
                c.push_back(Text{it.box, it.text, l.size * z * l.scale, it.empty ? l.placeholderColor : l.color, l.bold, false, false,
                                 static_cast<Align>(l.align), l.italic});
            }
            break;
        case PartKind::Chip: {
            const LookText& l    = p.texts.front();
            const Color     tint = p.fill.a > 0 ? p.fill : Color{l.color.r, l.color.g, l.color.b, 40};
            c.push_back(FillRect{it.box, tint, {}, 0.f, it.box.h * 0.5f});
            if (text)
                c.push_back(Text{it.box, it.text, l.size * z * l.scale, it.empty ? l.placeholderColor : l.color, l.bold, false, false,
                                 Align::Center, l.italic});
            break;
        }
        case PartKind::Glyph:
            DrawPin(c, it.box.Center(), Vec2{1.f, 0.f}, p.glyph.value_or(PortGlyph::Diamond), it.box.w * 0.5f, p.color, look.fill, !p.hollow);
            break;
        case PartKind::Bar:
            c.push_back(FillRect{it.box, p.fill.a > 0 ? p.fill : p.color, {}, 0.f, std::min(it.box.w, it.box.h) * 0.5f});
            break;
        case PartKind::Stub: {
            // A short wire out of the pill's end: its arrow points at the pill (in) or away from it (out).
            const float       cy   = r.y + r.h * 0.5f;
            const Vec2        near = {p.side == 1 ? it.box.x : it.box.Right(), cy};
            const Vec2        far  = {p.side == 1 ? it.box.Right() : it.box.x, cy};
            std::vector<Vec2> path = p.out ? std::vector<Vec2>{near, far} : std::vector<Vec2>{far, near};
            c.push_back(Polyline{path, p.color, std::max(1.f, 1.5f * z)});
            c.push_back(Arrowhead(path, std::clamp(7.f * z, 4.f, 10.f), p.color));
            break;
        }
        default:
            break;
        }
    }
}

} // namespace

// ── Setup ────────────────────────────────────────────────────────────────────

Canvas::Canvas(Graph& graph, const Schema& schema) : graph_(&graph), host_(&graph), schema_(&schema) {}

void Canvas::SetGraph(Graph& graph)
{
    dragKey_    = 0;
    resync_     = true;
    graph_      = &graph;
    host_       = &graph;
    scratch_    = {};
    mode_       = Mode::Idle;
    backendKey_ = 0;   // the backend rebuilds for a new graph; that route doesn't time a drag step
    hovered_.clear();
    ClearSelection();
}

const NodeType& Canvas::TypeOf(const Node& n) const
{
    static const NodeType kFallback{};
    const NodeType* t = schema_->FindNode(n.type);
    return t ? *t : kFallback;
}

float Canvas::Snap(float v) const { return snap > 0.f ? std::round(v / snap) * snap : v; }

std::vector<PortSlot> Canvas::SlotsOf(const Node& n) const { return LayoutPorts(n, *schema_, MetricsFor(n)); }
std::vector<PortSlot> Canvas::SlotsOf(const Node& n, const Rect& frame) const { return LayoutPorts(n, *schema_, MetricsFor(n), frame); }

void Canvas::EnsureKit() const
{
    if (kitValid_ && kitFor_ == kit_ && (!kit_ || kitRev_ == kit_->revision) && kitSchema_ == schema_ && kitSchemaRev_ == schema_->revision &&
        kitStyle_ == style)
        return;
    effective_    = kit_ ? MergeKits(KitFromSchema(*schema_, style), *kit_) : KitFromSchema(*schema_, style);
    kitValid_     = true;
    kitFor_       = kit_;
    kitRev_       = kit_ ? kit_->revision : 0;
    kitSchema_    = schema_;
    kitSchemaRev_ = schema_->revision;
    kitStyle_     = style;
    ++kitGen_;
    looks_.clear();
    edgeLooks_.clear();
    kitCards_.clear();
    kitLines_.clear();
    fitWidth_.clear();
    pinKinds_.clear();
    if (!kit_) return;   // the Schema's own kit: every card has the Style's geometry, none fits its width, no pin kinds
    // By type, since a prefix kind (`card:*`) stands for many: each edge type's route, each node type's geometry, and
    // the pin kind each port type draws as. A kit's exact kind for a type the Schema lacks counts as that type.
    auto exactOnly = [](std::string_view id) { return id.empty() || id.back() != '*'; };
    auto route     = [&](const EdgeType& t) {
        if (const EdgeLine line = ResolveEdgeLook(effective_, t, {}, 0).line; line != t.line) kitLines_.emplace(t.id, line);
    };
    route(EdgeType{});
    for (const EdgeType& t : schema_->edgeTypes) route(t);
    for (const EdgeKind& k : kit_->edges)
        if (exactOnly(k.id) && !k.id.empty() && !schema_->FindEdge(k.id)) route(EdgeType{k.id, {}});
    const CardMetrics base = MetricsOf(style);
    auto              card = [&](const NodeType& t) {
        const NodeLook look = ResolveLook(effective_, t, {}, 0, base);
        if (!(look.metrics == base)) kitCards_.emplace(t.id, look.metrics);
        if (look.fitWidth) fitWidth_.insert(t.id);
    };
    card(NodeType{});
    for (const NodeType& t : schema_->nodeTypes) card(t);
    for (const NodeKind& k : kit_->nodes)
        if (exactOnly(k.id) && !k.id.empty() && !schema_->FindNode(k.id)) card(NodeType{k.id, {}});
    if (!effective_.pins.empty())
        for (const PortType& t : schema_->portTypes)
            if (const PinKind* k = effective_.FindPin(t.pin.empty() ? t.id : t.pin)) pinKinds_.emplace(t.id, k->id);
}

const std::string* Canvas::PinKindOf(std::string_view portType) const
{
    if (const auto it = pinKinds_.find(portType); it != pinKinds_.end()) return &it->second;
    const PinKind* k = effective_.FindPin(portType);   // a type the Schema doesn't list (an untyped port's "")
    return k ? &k->id : nullptr;
}

const NodeLook& Canvas::LookOf(const Node& n, std::uint32_t states) const
{
    EnsureKit();
    std::string& key = lookKey_;
    key.assign(n.type);
    key += '\x1f';
    for (const std::string& tag : n.tags) {
        key += tag;
        key += '\x1e';
    }
    key += '\x1f';
    key.append(reinterpret_cast<const char*>(&states), sizeof states);
    if (const auto it = looks_.find(std::string_view(key)); it != looks_.end()) return it->second;
    ++resolved_;
    return looks_.emplace(key, ResolveLook(effective_, TypeOf(n), n.tags, states, MetricsOf(style))).first->second;
}

const EdgeLook& Canvas::EdgeLookOf(const Edge& e, std::uint32_t states) const
{
    EnsureKit();
    std::string key = e.type;
    key += '\x1f';
    for (const std::string& tag : e.tags) {
        key += tag;
        key += '\x1e';
    }
    key += '\x1f';
    key.append(reinterpret_cast<const char*>(&states), sizeof states);
    if (const auto it = edgeLooks_.find(std::string_view(key)); it != edgeLooks_.end()) return it->second;
    static const EdgeType kPlain{};
    const EdgeType* t = schema_->FindEdge(e.type);
    return edgeLooks_.emplace(std::move(key), ResolveEdgeLook(effective_, t ? *t : kPlain, e.tags, states)).first->second;
}

CardMetrics Canvas::MetricsFor(const Node& n) const
{
    if (!kit_) return MetricsOf(style);   // no kit: the Style's, as ever
    EnsureKit();
    const auto it = kitCards_.find(n.type);
    return it == kitCards_.end() ? MetricsOf(style) : it->second;
}

std::uint32_t Canvas::StatesOf(const Node& n) const
{
    std::uint32_t states = 0;
    if (highlights.dimmed.contains(n.id)) states |= StateDimmed;
    if (!n.aliasOf.empty()) states |= StateAlias;
    if (!n.parent.empty()) states |= StateInGroup;
    return states;
}

std::string Canvas::PartAt(const Node& n, Vec2 world) const
{
    const NodeType& t  = TypeOf(n);
    const float     wy = world.y;
    if (t.shape == NodeShape::Pill) {
        const NodeLook& look = LookOf(n, StatesOf(n));
        for (const PillItem& it : PillLayout(*LiveOf(n), t, look, n.frame, 1.f))
            if (it.part->kind != PartKind::Stub && it.box.Contains(world)) return it.part->id;
        return {};
    }
    if (t.shape != NodeShape::Card && t.shape != NodeShape::Note) return {};
    const NodeLook& look = LookOf(n, StatesOf(n));
    for (const LookPart& part : look.parts)
        if (part.kind == PartKind::Header) {
            const float h = part.height > 0.f ? part.height : look.metrics.Header();
            if (wy < n.frame.y + h) return part.id;
        }
    for (const LookPart& part : look.parts)
        if (part.kind == PartKind::Body) return part.id;
    return {};
}

float Canvas::MeasureText(std::string_view text, float size, bool bold) const
{
    if (measure) return measure(text, size, bold);
    std::size_t points = 0;
    for (const char ch : text)
        if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++points;
    return static_cast<float>(points) * size * (bold ? 0.62f : 0.56f);
}

void Canvas::Before(std::string_view label)
{
    ++edits_;
    if (editMode == EditMode::Direct) {
        if (onBeforeChange) onBeforeChange(label);
        return;
    }
    // A proposal plays out on a copy of the host's graph, built on the last one until the host refreshes.
    if (graph_ == host_) {
        scratch_ = *host_;
        graph_   = &scratch_;
    }
}

void Canvas::Changed()
{
    if (editMode == EditMode::Direct && onChanged) onChanged();
}

void Canvas::Changed(Intent intent)
{
    if (editMode == EditMode::Direct) {
        if (onChanged) onChanged();
    } else if (onIntent) {
        onIntent(intent);
    }
}

void Canvas::SetCollapsed(std::string_view group, bool collapsed)
{
    if (collapsed == collapsed_.contains(group)) return;
    if (collapsed) collapsed_.emplace(group);
    else collapsed_.erase(collapsed_.find(group));
    ++viewRev_;
    if (onCollapseChanged) onCollapseChanged(group, collapsed);
}

void Canvas::SetAllCollapsed(bool collapsed, const std::function<bool(const Node&)>& which)
{
    for (const Node& n : graph_->nodes)
        if (IsMemberGroup(n, *schema_) && !TypeOf(n).fixed && (!which || which(n))) SetCollapsed(n.id, collapsed);
}

void Canvas::Reveal(std::string_view nodeId)
{
    const Node* n = graph_->FindNode(nodeId);
    for (std::size_t steps = 0; n && !n->parent.empty() && steps <= graph_->nodes.size(); ++steps) {
        SetCollapsed(n->parent, false);
        n = graph_->FindNode(n->parent);
    }
}

bool Canvas::IsHidden(std::string_view nodeId) const { return graph_->FindNode(nodeId) && !ShownNode(nodeId); }

Rect Canvas::FrameOf(std::string_view nodeId) const
{
    const Node* n = ShownNode(nodeId);
    return n ? n->frame : Rect{};
}

void Canvas::HoldInPlace(std::string_view nodeId)
{
    const auto at = HeaderScreenPos(nodeId);
    hold_         = at ? std::string(nodeId) : std::string{};
    holdAt_       = at.value_or(Vec2{});
}

void Canvas::Refresh()
{
    dragKey_ = 0;   // the host changed its graph: a drag under way keys and routes it in full again
    resync_  = true;
    // The node held in place keeps its header where it was on screen.
    if (!hold_.empty()) {
        const std::string held = std::exchange(hold_, {});
        if (graph_ != host_) {
            if (mode_ != Mode::Idle) CancelGesture();
            graph_   = host_;
            scratch_ = {};
        }
        if (const auto now = HeaderScreenPos(held)) PanBy(holdAt_ - *now);
    }
    if (graph_ == host_) return;
    if (mode_ != Mode::Idle) CancelGesture();
    graph_   = host_;
    scratch_ = {};
    // What the host didn't take is no longer there to be selected.
    NodeSet kept;
    for (const std::string& id : selNodes_)
        if (graph_->FindNode(id)) kept.insert(id);
    if (kept != selNodes_ || (!selEdge_.empty() && !graph_->FindEdge(selEdge_))) {
        selNodes_ = std::move(kept);
        if (!selEdge_.empty() && !graph_->FindEdge(selEdge_)) selEdge_.clear();
        SelectionChanged();
    }
}

void Canvas::CancelGesture()
{
    const Mode mode = std::exchange(mode_, Mode::Idle);
    lifted_.clear();
    connectTo_.clear();
    connectPort_.clear();
    carried_.clear();
    if ((mode != Mode::Moving && mode != Mode::Resizing) || !started_) return;
    started_ = false;
    if (editMode == EditMode::Propose) {
        graph_   = host_;   // the copy goes, and the host's graph never moved
        scratch_ = {};
        return;
    }
    for (const auto& [id, frame] : origin_)
        if (Node* n = graph_->FindNode(id)) n->frame = frame;
    Changed();   // the host snapshotted when the gesture began; it ends where it started
}

MoveNodes Canvas::MovedFrames() const
{
    MoveNodes out;
    if (graph_ == host_) return out;
    std::unordered_map<std::string_view, const Rect*> before;
    before.reserve(host_->nodes.size());
    for (const Node& n : host_->nodes) before.emplace(n.id, &n.frame);
    std::unordered_map<std::string_view, const std::string*> parentBefore;
    parentBefore.reserve(host_->nodes.size());
    for (const Node& n : host_->nodes) parentBefore.emplace(n.id, &n.parent);
    for (const Node& n : graph_->nodes) {
        if (const auto it = before.find(n.id); it != before.end() && !(*it->second == n.frame)) out.frames.emplace_back(n.id, n.frame);
        if (const auto it = parentBefore.find(n.id); it != parentBefore.end() && *it->second != n.parent) out.parents.emplace_back(n.id, n.parent);
        if (const Node* h = host_->FindNode(n.id); h && !(h->dock == n.dock)) out.docks.emplace_back(n.id, n.dock);
    }
    return out;
}

void Canvas::SelectionChanged()
{
    if (onSelectionChanged) onSelectionChanged();
}

// ── Selection ────────────────────────────────────────────────────────────────

void Canvas::SelectNodes(NodeSet ids)
{
    if (ids == selNodes_ && selEdge_.empty()) return;
    selNodes_ = std::move(ids);
    selEdge_.clear();
    SelectionChanged();
}

void Canvas::SelectEdge(std::string id)
{
    if (id == selEdge_ && selNodes_.empty()) return;
    selNodes_.clear();
    selEdge_ = std::move(id);
    SelectionChanged();
}

void Canvas::ClearSelection()
{
    if (selNodes_.empty() && selEdge_.empty()) return;
    selNodes_.clear();
    selEdge_.clear();
    SelectionChanged();
}

void Canvas::SelectAll()
{
    NodeSet all;
    for (const Node& n : graph_->nodes) all.insert(n.id);
    SelectNodes(std::move(all));
}

// ── Edits ────────────────────────────────────────────────────────────────────

std::string Canvas::AddNode(std::string_view type, Vec2 worldCenter)
{
    const NodeType* t = schema_->FindNode(type);
    if (!t) return {};
    Before("Add " + t->label);
    Node n;
    n.id    = NewId();
    n.type  = t->id;
    const float h = PortRows(n, *schema_) > 0 ? std::max(t->height, CardHeight(n, *schema_, MetricsFor(n))) : t->height;
    n.frame = {Snap(worldCenter.x - t->width * 0.5f), Snap(worldCenter.y - h * 0.5f), t->width, h};
    graph_->nodes.push_back(n);
    Settle({n.id}, {n.id});
    SelectNodes({n.id});
    Changed(Intent{"Add " + t->label, CreateNode{*graph_->FindNode(n.id)}});
    return n.id;
}

bool Canvas::DeleteSelection()
{
    RemoveItems gone;
    if (!selNodes_.empty()) {
        gone.nodes = selNodes_;
        for (const Edge& e : graph_->edges)
            if (selNodes_.contains(e.from) || selNodes_.contains(e.to)) gone.edges.push_back(e.id);
        Before("Delete");
        graph_->RemoveNodes(selNodes_);
    } else if (!selEdge_.empty() && graph_->FindEdge(selEdge_)) {
        gone.edges.push_back(selEdge_);
        Before("Delete");
        graph_->RemoveEdges({selEdge_});
    } else {
        return false;
    }
    ClearSelection();
    Changed(Intent{"Delete", std::move(gone)});
    return true;
}

bool Canvas::PickChoice(std::string_view nodeId, std::string_view rowId, std::string_view value)
{
    const Node* n = graph_->FindNode(nodeId);
    const Row*  r = n ? n->FindRow(rowId) : nullptr;
    if (!r || r->action != RowAction::Choice || r->field.empty()) return false;
    SetValue set{std::string(nodeId), std::string(rowId), r->field, std::string(value)};
    Before("Set " + r->field);
    Node* m = graph_->FindNode(nodeId);
    m->fields[set.field] = set.value;
    for (Row& row : m->rows)
        if (row.id == rowId) row.value = set.value;
    Changed(Intent{"Set " + set.field, std::move(set)});
    return true;
}

std::optional<RowClick> Canvas::TakeRowClick() { return std::exchange(rowClick_, std::nullopt); }

std::optional<EditRequest> Canvas::TakeEditRequest() { return std::exchange(edit_, std::nullopt); }

bool Canvas::CommitEdit(std::string_view nodeId, std::string_view rowId, std::string_view value)
{
    const Node* n = graph_->FindNode(nodeId);
    const Row*  r = n ? n->FindRow(rowId) : nullptr;
    if (!r) return false;
    SetValue          set{std::string(nodeId), std::string(rowId), r->field, std::string(value)};
    const std::string label = "Set " + (r->field.empty() ? r->label : r->field);
    Before(label);
    Node* m = graph_->FindNode(nodeId);   // Before may have swapped in the proposal's copy
    if (!set.field.empty()) m->fields[set.field] = set.value;
    for (Row& row : m->rows)
        if (row.id == rowId) row.value = set.value;
    Changed(Intent{label, std::move(set)});
    return true;
}

bool Canvas::DoubleClick(Vec2 at, Modifiers mods) { return kit_ && Gesture("doubleClick", HitTest(at), at, mods); }
bool Canvas::ContextMenu(Vec2 at, Modifiers mods) { return kit_ && Gesture("contextMenu", HitTest(at), at, mods); }

std::string Canvas::BindingFor(const Hit& hit, std::string_view gesture) const
{
    const Node* n = graph_->FindNode(hit.id);
    if (!kit_ || !n) return {};
    auto in = [&](const std::map<std::string, std::string, std::less<>>& on) {
        const auto it = on.find(gesture);
        return it == on.end() ? std::string() : it->second;
    };
    // The most particular thing under the gesture binds it: its pin, its row, its part, then the card.
    if (!hit.port.empty())
        if (const auto def = PortOf(*n, *schema_, hit.port))
            if (const PinLook* pin = PinLookOf(*n, *def, wiredPorts_.contains(PortKey(n->id, hit.port))))
                if (std::string a = in(pin->on); !a.empty()) return a;
    if (!hit.row.empty())
        if (const Row* r = n->FindRow(hit.row))
            if (std::string a = in(RowLookOf(*n, *r).on); !a.empty()) return a;
    const NodeLook& look = LookOf(*n, StatesOf(*n) | (selNodes_.contains(n->id) ? StateSelected : 0u));
    if (!hit.part.empty())
        for (const LookPart& part : look.parts)
            if (part.id == hit.part)
                if (std::string a = in(part.on); !a.empty()) return a;
    return in(look.on);
}

bool Canvas::Gesture(std::string_view gesture, const Hit& hit, Vec2 at, Modifiers mods)
{
    if (!kit_ || hit.kind == HitKind::None || hit.kind == HitKind::Edge) return false;
    const std::string action = BindingFor(hit, gesture);
    if (action.empty()) return false;
    if (action == "edit") return BeginEdit(hit.id, hit.row);
    if (onIntent) onIntent(Intent{action, InvokeAction{action, hit.id, hit.part, hit.row, hit.port, camera.ToWorld(at), mods}});
    return true;
}

bool Canvas::BeginEdit(std::string_view nodeId, std::string_view rowId)
{
    const Node* n = graph_->FindNode(nodeId);
    const Row*  r = n ? n->FindRow(rowId) : nullptr;
    if (!r) return false;
    const RowLook& look = RowLookOf(*n, *r);
    if (look.editor.empty() || look.editor == "none") return false;
    EditRequest e;
    e.node   = std::string(nodeId);
    e.row    = std::string(rowId);
    e.field  = r->field;
    e.editor = look.editor;
    e.value  = !r->field.empty() && !n->Field(r->field).empty() ? n->Field(r->field) : r->value;
    e.choices = r->choices;
    if ((e.editor == "reference" || (e.editor == "choice" && e.choices.empty())) && choicesFor) e.choices = choicesFor(*n, *r);
    e.min  = look.min;
    e.max  = look.max;
    e.step = look.step;
    if (const auto line = RowScreenRect(nodeId, rowId)) e.at = *line;
    edit_ = std::move(e);
    return true;
}

const RowLook& Canvas::RowLookOf(const Node& n, const Row& row) const
{
    static const RowLook kPlain{};
    const NodeLook& look = LookOf(n, StatesOf(n));
    const auto      it   = look.rows.find(row.style);
    return it == look.rows.end() ? kPlain : it->second;
}

const NodeLook* Canvas::PinTableOf(const Node& n) const
{
    if (!kit_) return nullptr;
    EnsureKit();
    return effective_.pins.empty() ? nullptr : &LookOf(n, StatesOf(n));
}

const PinLook* Canvas::PinLookOf(const Node& n, const PortDef& def, bool wired) const
{
    const NodeLook* look = PinTableOf(n);
    return look ? PinLookIn(*look, n, def, wired) : nullptr;
}

const PinLook* Canvas::PinLookIn(const NodeLook& look, const Node& n, const PortDef& def, bool wired) const
{
    if (look.pins.empty()) return nullptr;
    const std::string* kind = def.typeFrom.empty() ? PinKindOf(def.type) : PinKindOf(PortTypeOf(*graph_, *schema_, n, def.id));
    if (!kind) return nullptr;
    const auto it = look.pins.find(*kind);
    return it == look.pins.end() ? nullptr : &it->second[wired ? 1 : 0];
}

int Canvas::ReorderTarget(const Node& n, std::string_view rowId, Vec2 at) const
{
    int from = -1;
    for (std::size_t k = 0; k < n.rows.size(); ++k)
        if (n.rows[k].id == rowId) from = static_cast<int>(k);
    if (from < 0) return -1;
    // Its list: the run of rows around it whose kind names the same list.
    const std::string list = RowLookOf(n, n.rows[static_cast<std::size_t>(from)]).reorder;
    int lo = from, hi = from;
    while (lo > 0 && RowLookOf(n, n.rows[static_cast<std::size_t>(lo - 1)]).reorder == list) --lo;
    while (hi + 1 < static_cast<int>(n.rows.size()) && RowLookOf(n, n.rows[static_cast<std::size_t>(hi + 1)]).reorder == list) ++hi;
    const Node*       shown = ShownNode(n.id);
    const CardMetrics m     = MetricsFor(n);
    const float       top   = RowsTop(shown ? *shown : n, *schema_, m);
    const int         k     = static_cast<int>(std::floor((camera.ToWorld(at).y - top) / m.Row()));
    return std::clamp(k, lo, hi);
}

namespace {

struct Ends {
    std::string_view from, fromPort, to, toPort;
};

/// @brief The ends in wire order: the Out port's end first. A node-level end follows the port at the other end.
Ends Ordered(std::string_view a, std::string_view pa, const std::optional<PortDef>& da, std::string_view b, std::string_view pb,
             const std::optional<PortDef>& db)
{
    const bool swap = da ? da->dir == PortDir::In : (db && db->dir == PortDir::Out);
    return swap ? Ends{b, pb, a, pa} : Ends{a, pa, b, pb};
}

} // namespace

bool Canvas::CanWire(std::string_view a, std::string_view pa, std::string_view b, std::string_view pb) const
{
    return CanWire(a, pa, b, pb, {});
}

bool Canvas::CanWire(std::string_view a, std::string_view pa, std::string_view b, std::string_view pb, std::string_view ignoring) const
{
    if (a == b) return false;
    if (!wholeNodeWires && (pa.empty() || pb.empty())) return false;   // every wire runs port to port
    const Node* na = graph_->FindNode(a);
    const Node* nb = graph_->FindNode(b);
    if (!na || !nb) return false;
    std::optional<PortDef> da, db;
    if (!pa.empty() && !(da = PortOf(*na, *schema_, pa))) return false;
    if (!pb.empty() && !(db = PortOf(*nb, *schema_, pb))) return false;
    if (da && db) {
        if (da->dir == db->dir) return false;
        const bool outA = da->dir == PortDir::Out;
        const std::string out = PortTypeOf(*graph_, *schema_, outA ? *na : *nb, outA ? pa : pb);
        const std::string in  = PortTypeOf(*graph_, *schema_, outA ? *nb : *na, outA ? pb : pa);
        if (!schema_->CanConnect(out, in)) return false;
    }
    const Ends w = Ordered(a, pa, da, b, pb, db);
    for (const Edge& e : graph_->edges)
        if (e.id != ignoring && e.from == w.from && e.to == w.to && e.fromPort == w.fromPort && e.toPort == w.toPort) return false;
    if (checkWire) {
        Edge proposed;
        proposed.from     = w.from;
        proposed.to       = w.to;
        proposed.fromPort = w.fromPort;
        proposed.toPort   = w.toPort;
        return checkWire(proposed).allowed;
    }
    return true;
}

std::string Canvas::Connect(std::string_view a, std::string_view pa, std::string_view b, std::string_view pb)
{
    return Wire(a, pa, b, pb, {});
}

std::string Canvas::Wire(std::string_view a, std::string_view pa, std::string_view b, std::string_view pb, std::string_view lifted)
{
    // The lifted wire doesn't count as the same wire twice: it is about to go.
    std::optional<Edge> held;
    if (const Edge* old = lifted.empty() ? nullptr : graph_->FindEdge(lifted)) held = *old;
    if (!CanWire(a, pa, b, pb, held ? std::string_view(held->id) : std::string_view{})) return {};
    const Node* na = graph_->FindNode(a);
    const Node* nb = graph_->FindNode(b);
    const auto  da = pa.empty() ? std::nullopt : PortOf(*na, *schema_, pa);
    const auto  db = pb.empty() ? std::nullopt : PortOf(*nb, *schema_, pb);
    const Ends  w  = Ordered(a, pa, da, b, pb, db);
    const bool  swapped = w.from != a;
    const std::optional<PortDef>& outDef = swapped ? db : da;
    const std::optional<PortDef>& inDef  = swapped ? da : db;

    Edge e;
    e.id       = NewId();
    e.from     = w.from;
    e.to       = w.to;
    e.fromPort = w.fromPort;
    e.toPort   = w.toPort;
    const Node* fromNode = graph_->FindNode(e.from);
    const Node* toNode   = graph_->FindNode(e.to);
    const PortType* pt   = outDef ? schema_->FindPortType(PortTypeOf(*graph_, *schema_, *fromNode, e.fromPort)) : nullptr;
    e.type = pt && !pt->edge.empty() && schema_->FindEdge(pt->edge) ? pt->edge : schema_->EdgeFor(fromNode->type, toNode->type);
    if (checkWire) {
        // The host may mark the wire (a converting one): its own edge type, a label.
        const WireCheck verdict = checkWire(e);
        if (!verdict.type.empty() && schema_->FindEdge(verdict.type)) e.type = verdict.type;
        if (!verdict.label.empty()) e.label = verdict.label;
    }

    Before(held ? "Move wire" : "Connect");
    std::vector<std::string> replaced;
    if (held) {
        std::erase_if(graph_->edges, [&](const Edge& x) { return x.id == held->id; });
        replaced.push_back(held->id);
    }
    // A port at its limit gives up its oldest wire to the new one.
    auto makeRoom = [&](const std::string& node, const std::string& port, const std::optional<PortDef>& def, bool source) {
        if (!def) return;
        const int max = MaxWires(*def);
        if (max <= 0) return;
        auto on = [&](const Edge& x) { return source ? x.from == node && x.fromPort == port : x.to == node && x.toPort == port; };
        int count = static_cast<int>(std::count_if(graph_->edges.begin(), graph_->edges.end(), on));
        for (auto it = graph_->edges.begin(); it != graph_->edges.end() && count >= max;) {
            if (!on(*it)) {
                ++it;
                continue;
            }
            if (it->id == selEdge_) selEdge_.clear();
            replaced.push_back(it->id);
            it = graph_->edges.erase(it);
            --count;
        }
    };
    makeRoom(e.from, e.fromPort, outDef, true);
    makeRoom(e.to, e.toPort, inDef, false);
    graph_->edges.push_back(e);
    Changed(Intent{held ? "Move wire" : "Connect", ConnectWire{e, std::move(replaced)}});
    return e.id;
}

std::string Canvas::LandingPort(const Node& from, std::string_view port, const Node& to) const
{
    std::string first;
    for (const PortDef& p : PortsOf(to, *schema_)) {
        if (!CanWire(from.id, port, to.id, p.id)) continue;
        const int max = MaxWires(p);
        const auto used = std::count_if(graph_->edges.begin(), graph_->edges.end(), [&](const Edge& e) {
            return p.dir == PortDir::In ? e.to == to.id && e.toPort == p.id : e.from == to.id && e.fromPort == p.id;
        });
        if (max == 0 || used < max) return p.id;
        if (first.empty()) first = p.id;
    }
    return first;
}

std::optional<Vec2> Canvas::PortScreenPos(std::string_view nodeId, std::string_view port) const
{
    const Node* n = ShownNode(nodeId);
    if (!n) return std::nullopt;
    const auto slots = SlotsOf(*n);
    const PortSlot* s = FindSlot(slots, port);
    if (!s) return std::nullopt;
    return camera.ToScreen(s->at);
}

// ── Camera ───────────────────────────────────────────────────────────────────

void Canvas::Frame() { Frame({}); }

void Canvas::Frame(const NodeSet& ids, float margin, float zoomCap)
{
    std::optional<Rect> b;
    for (const Node& n : (Sync(), Shown().nodes))
        if (ids.empty() || ids.contains(n.id)) b = b ? Rect::Union(*b, n.frame) : n.frame;
    if (!b) {
        if (ids.empty()) camera = {{viewport_.x * 0.5f, viewport_.y * 0.5f}, 1.f};
        return;
    }
    const float zx = (viewport_.x - 2 * margin) / std::max(b->w, 1.f);
    const float zy = (viewport_.y - 2 * margin) / std::max(b->h, 1.f);
    // A fit wins over the zoom-out limit: it shows the whole of what it frames however small that makes it.
    camera.zoom    = std::max(std::min({zx, zy, zoomCap, maxZoom}), 1e-4f);
    fitZoom_       = camera.zoom;
    const Vec2 c   = b->Center();
    camera.pan     = {viewport_.x * 0.5f - c.x * camera.zoom, viewport_.y * 0.5f - c.y * camera.zoom};
}

void Canvas::CenterOn(std::string_view nodeId, std::optional<float> zoom)
{
    const Node* n = ShownNode(nodeId);
    if (!n) return;
    if (zoom) camera.zoom = std::clamp(*zoom, ZoomFloor(), maxZoom);
    const Vec2 c = n->frame.Center();
    camera.pan   = {viewport_.x * 0.5f - c.x * camera.zoom, viewport_.y * 0.5f - c.y * camera.zoom};
}

void Canvas::EnsureVisible(std::string_view nodeId, float margin)
{
    const Node* n = ShownNode(nodeId);
    if (!n) return;
    const Rect r = camera.ToScreen(n->frame);
    // Per axis: as little as brings it in; when it can't fit, its top-left goes in first.
    auto shift = [](float lo, float hi, float size, float m) {
        if (hi - lo > size - 2 * m) return m - lo;
        if (lo < m) return m - lo;
        if (hi > size - m) return size - m - hi;
        return 0.f;
    };
    PanBy({shift(r.x, r.Right(), viewport_.x, margin), shift(r.y, r.Bottom(), viewport_.y, margin)});
}

void Canvas::PanBy(Vec2 pixels) { camera.pan = camera.pan + pixels; }

bool Canvas::ZoomAbout(float factor, Vec2 anchor)
{
    const float before = camera.zoom;
    const float after  = std::clamp(before * factor, std::min(ZoomFloor(), before), maxZoom);
    if (after == before) return false;
    const Vec2 world = camera.ToWorld(anchor);
    camera.zoom      = after;
    camera.pan       = anchor - world * after;
    return true;
}

bool Canvas::Wheel(Vec2 at, float steps) { return ZoomAbout(std::pow(1.15f, steps), at); }

std::optional<Vec2> Canvas::HeaderScreenPos(std::string_view nodeId) const
{
    const Node* n = ShownNode(nodeId);
    if (!n) return std::nullopt;
    const CardMetrics m = MetricsFor(*n);
    const float       y = IsGroup(*n, *schema_) ? std::min(n->frame.h, m.groupHeader) * 0.5f : m.Top() + m.titleSize * 0.65f;
    return camera.ToScreen(Vec2{n->frame.Center().x, n->frame.y + std::min(y, n->frame.h)});
}

std::optional<Rect> Canvas::RowScreenRect(std::string_view nodeId, std::string_view row) const
{
    const Node* n = ShownNode(nodeId);
    if (!n) return std::nullopt;
    for (std::size_t k = 0; k < n->rows.size(); ++k)
        if (n->rows[k].id == row) return camera.ToScreen(RowRect(*n, *schema_, MetricsFor(*n), k));
    return std::nullopt;
}

std::string Canvas::TooltipAt(Vec2 screen) const
{
    const Hit hit = HitTest(screen);
    switch (hit.kind) {
    case HitKind::Row:
        if (const Node* n = ShownNode(hit.id)) {
            const Row* r = n->FindRow(hit.row);
            return r && !r->tooltip.empty() ? r->tooltip : n->tooltip;
        }
        return {};
    case HitKind::Port:
        if (const Node* n = ShownNode(hit.id))
            for (const PortDef& p : PortsOf(*n, *schema_))
                if (p.id == hit.port) {
                    if (!p.tooltip.empty()) return p.tooltip;
                    const std::string type = PortTypeOf(*graph_, *schema_, *n, p.id);
                    const PortType*   pt   = schema_->FindPortType(type);
                    const std::string name = p.label.empty() ? p.id : p.label;
                    return type.empty() ? name : name + " (" + (pt ? pt->label : type) + ")";
                }
        return {};
    case HitKind::Node:
    case HitKind::Resize:
        if (const Node* n = ShownNode(hit.id)) return n->tooltip;
        return {};
    case HitKind::Edge:
        if (const Edge* e = graph_->FindEdge(hit.id)) return e->label;
        return {};
    default: return {};
    }
}

Rect Canvas::ScreenRectOf(std::string_view nodeId) const
{
    const Node* n = ShownNode(nodeId);
    return n ? camera.ToScreen(n->frame) : Rect{};
}

// ── Hit testing ──────────────────────────────────────────────────────────────

Hit Canvas::HitTest(Vec2 screen) const { return HitAt(screen, false); }

Hit Canvas::HitAt(Vec2 screen, bool groupInterior) const
{
    SyncForHit();
    const float handle = HandleRadius(style, camera.zoom) + 4.f;
    const float pin    = PinRadius(camera.zoom) + 4.f;
    const float reach  = std::max(handle, pin);

    // Only the nodes near the point are candidates. Topmost first: selected nodes draw last, so they are tested first.
    // Groups draw under everything, inner over outer.
    std::vector<const Node*> order, groups;
    for (std::size_t i = Shown().nodes.size(); i-- > 0;) {
        const Node& n = Shown().nodes[i];
        if (!camera.ToScreen(n.frame).Inflated(reach).Contains(screen) || !Visible(n)) continue;
        if (const Rect r = camera.ToScreen(n.frame); TypeOf(n).shape == NodeShape::Dot &&
                                                     Distance(screen, r.Center()) <= std::max(std::min(r.w, r.h) * 0.3f, 4.f))
            return {HitKind::Node, n.id};   // its pins cover its rim; its middle grabs it
        if (grouped_[i]) {
            if (!TypeOf(n).fixed) groups.push_back(&n);   // a fixed group is canvas: a press on it is a press on nothing
        } else {
            order.push_back(&n);
        }
    }
    std::stable_partition(order.begin(), order.end(), [&](const Node* n) { return selNodes_.contains(n->id); });
    std::stable_sort(groups.begin(), groups.end(), [](const Node* a, const Node* b) { return a->frame.Area() < b->frame.Area(); });

    // A port's pin, or a port-less node's connect handle, sits on the outline; both reach a little past it.
    auto onOutline = [&](const Node& n, const Rect& r) -> Hit {
        const auto      slots = SlotsOf(n);
        const NodeLook* table = slots.empty() ? nullptr : PinTableOf(n);
        for (const PortSlot& s : slots) {
            if (s.def.id == kGroupIn || s.def.id == kGroupOut) continue;   // a closed group's end pins: shown, not edited
            float reachOf = pin;
            if (const PinLook* look = table ? PinLookIn(*table, n, s.def, wiredPorts_.contains(PortKey(n.id, s.def.id))) : nullptr)
                reachOf = (look->size > 0.f ? look->size * camera.zoom : PinRadius(camera.zoom)) + look->hit;
            if (Distance(screen, camera.ToScreen(s.at)) <= reachOf) return {HitKind::Port, n.id, s.def.id};
        }
        if (wholeNodeWires && slots.empty() && Distance(screen, ConnectHandle(r)) <= handle) return {HitKind::Connect, n.id};
        return {};
    };
    for (const Node* n : order) {
        const Rect r = camera.ToScreen(n->frame);
        if (!r.Inflated(reach).Contains(screen)) continue;
        if (Hit h = onOutline(*n, r); h.kind != HitKind::None) return h;
        if (r.Contains(screen)) {
            if (TypeOf(*n).shape == NodeShape::Dial) {
                // A dial's handles sit inside its rim, one per row on it; an octagon is its outline, its listed rows
                // under its header.
                const DialShape shape = DialOf(*n, MetricsFor(*n), n->frame);
                const Ring&     ring  = shape.rim;
                const DialAxis  axis  = AxisOf(*n, ring.OpenReach());
                const Vec2      world = camera.ToWorld(screen);
                for (const Row& row : n->rows)
                    if (OnRim(*n, row) && Distance(screen, camera.ToScreen(ring.At(axis.Angle(RowNumber(row)), kHandleAt))) <= pin)
                        return {HitKind::Handle, n->id, {}, row.id};
                if (!shape.Holds(world)) continue;
                if (shape.Octagon() && world.y >= shape.rowsTop && world.y < shape.rowsBottom) {
                    const Node* live = LiveOf(*n);
                    auto        k    = static_cast<std::size_t>((world.y - shape.rowsTop) / MetricsFor(*n).Row());
                    for (const Row& row : live->rows)
                        if (!OnRim(*live, row) && k-- == 0) return {HitKind::Row, n->id, {}, row.id, kit_ ? PartAt(*n, world) : std::string()};
                }
                return {HitKind::Node, n->id};
            }
            if (TypeOf(*n).shape != NodeShape::Dot && ResizeGrip(r, style).Contains(screen)) return {HitKind::Resize, n->id};
            if (const Node* live = LiveOf(*n); !live->rows.empty() && TypeOf(*n).shape == NodeShape::Card) {
                const CardMetrics m   = MetricsFor(*n);
                const float       top = RowsTop(*n, *schema_, m);
                const float       wy  = camera.ToWorld(screen).y;
                if (wy >= top) {
                    const auto k = static_cast<std::size_t>((wy - top) / m.Row());
                    if (k < live->rows.size()) return {HitKind::Row, n->id, {}, live->rows[k].id, kit_ ? PartAt(*n, camera.ToWorld(screen)) : std::string()};
                }
            }
            return {HitKind::Node, n->id, {}, {}, kit_ ? PartAt(*n, camera.ToWorld(screen)) : std::string()};
        }
    }
    for (std::size_t i = Shown().edges.size(); i-- > 0;) {
        if (!paths_[i] || !camera.ToScreen(boxes_[i]).Inflated(kEdgeHitPx).Contains(screen)) continue;
        if (PolylineDistance(screen, ToScreen(camera, *paths_[i])) <= kEdgeHitPx) return {HitKind::Edge, Shown().edges[i].id};
    }
    for (const Node* n : groups) {
        const Rect r = camera.ToScreen(n->frame);
        if (!r.Inflated(reach).Contains(screen)) continue;
        if (Hit h = onOutline(*n, r); h.kind != HitKind::None) return h;
        if (!r.Contains(screen)) continue;
        if (ResizeGrip(r, style).Contains(screen)) return {HitKind::Resize, n->id};
        if (groupInterior || screen.y <= r.y + style.groupHeader * camera.zoom) return {HitKind::Node, n->id};
    }
    return {};
}

void Canvas::Settle(const NodeSet& roots, const NodeSet& moving)
{
    const float pad = style.padding;
    for (const std::string& id : roots) {
        Node* n = graph_->FindNode(id);
        if (!n) continue;
        if (!n->parent.empty()) continue;   // a member group's member is its alone
        const bool  group  = IsGroup(*n, *schema_);
        const Vec2  centre = n->frame.Center();
        const Node* target = nullptr;
        for (const Node& g : graph_->nodes) {
            if (g.id == id || moving.contains(g.id) || !IsGroup(g, *schema_) || IsMemberGroup(g, *schema_) || !g.frame.Contains(centre)) continue;
            if (group && g.frame.Area() <= n->frame.Area()) continue;
            if (!target || g.frame.Area() < target->frame.Area()) target = &g;
        }
        if (!target || target->frame.Holds(n->frame)) continue;
        const Rect& g = target->frame;
        const Rect  inner{g.x + pad, g.y + style.groupHeader, g.w - 2 * pad, g.h - style.groupHeader - pad};
        if (n->frame.w > inner.w || n->frame.h > inner.h) continue;   // too big to go in: it stays where it was dropped
        const Vec2 d{std::clamp(n->frame.x, inner.x, inner.Right() - n->frame.w) - n->frame.x,
                     std::clamp(n->frame.y, inner.y, inner.Bottom() - n->frame.h) - n->frame.y};
        std::vector<std::string> along{id};
        if (group) {
            const auto held = GroupMembers(*graph_, *schema_, id);
            along.insert(along.end(), held.begin(), held.end());
        }
        for (const std::string& m : along)
            if (Node* node = graph_->FindNode(m)) {
                node->frame.x += d.x;
                node->frame.y += d.y;
            }
    }
}

const WirePaths& Canvas::Wires() const
{
    Sync();
    return wires_;
}

const Node* Canvas::ShownNode(std::string_view id) const
{
    Sync();
    return NodeById(id);
}

const Node* Canvas::NodeById(std::string_view id) const
{
    const Graph& shown = Shown();
    if (synced_ == graph_ && index_.size() == shown.nodes.size())
        if (const auto it = index_.find(id); it != index_.end() && it->second < shown.nodes.size() && shown.nodes[it->second].id == id)
            return &shown.nodes[it->second];
    return shown.FindNode(id);
}

void Canvas::SyncForHit() const
{
    if (!wiresValid_ || synced_ != graph_ || syncedEdits_ != edits_ || syncedNodes_ != graph_->nodes.size() ||
        syncedEdges_ != graph_->edges.size() || grouped_.size() != Shown().nodes.size() || paths_.size() != Shown().edges.size())
        Sync();
}

void Canvas::BuildView() const
{
    view_ = *graph_;
    graphAt_.clear();
    graphAt_.reserve(graph_->nodes.size());
    for (std::size_t i = 0; i < graph_->nodes.size(); ++i) graphAt_.emplace(graph_->nodes[i].id, i);
    {
        // A card whose type fits its height grows to show its ports, rows and fields.
        for (Node& node : view_.nodes)
            if (const NodeType* t = schema_->FindNode(node.type); t && t->fitHeight && t->shape == NodeShape::Card)
                node.frame.h = std::max(node.frame.h, CardHeight(node, *schema_, MetricsFor(node)));
        // One whose kind fits its width takes its widest line, between the kind's bounds.
        if (!fitWidth_.empty())
            for (Node& node : view_.nodes) {
                if (!fitWidth_.contains(node.type)) continue;
                const NodeLook&    look = LookOf(node, 0);
                const CardMetrics& m    = look.metrics;
                float              inset = 0.f, widest = 0.f;
                for (const LookPart& part : look.parts) {
                    if (part.kind == PartKind::Accent) inset += part.width;
                    for (const LookText& text : part.texts) {
                        auto [line, placeholder] = LineText(text, node, TypeOf(node));
                        if (placeholder) line.clear();
                        widest = std::max(widest, MeasureText(line, text.size * text.scale, text.bold));
                    }
                }
                for (const Row& row : node.rows) {
                    const float value = row.value.empty() ? 0.f : MeasureText(row.value, m.fontSize, false) + m.padding;
                    widest            = std::max(widest, MeasureText(row.label, m.fontSize, false) + value);
                }
                float w = widest + 2 * m.padding + inset;
                if (look.minWidth > 0.f) w = std::max(w, look.minWidth);
                if (look.maxWidth > 0.f) w = std::min(w, look.maxWidth);
                node.frame.w = w;
            }
    }
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    const std::size_t     n     = view_.nodes.size();
    std::unordered_map<std::string_view, const NodeType*> types;
    for (const NodeType& t : schema_->nodeTypes) types.emplace(t.id, &t);
    // The ids as owned strings: the nodes move out of view_ below, and the index must outlive that.
    std::vector<std::string> ids;
    ids.reserve(n);
    for (const Node& node : view_.nodes) ids.push_back(node.id);
    std::unordered_map<std::string_view, std::size_t> at;
    at.reserve(n);
    for (std::size_t i = 0; i < n; ++i) at.emplace(ids[i], i);
    std::vector<char> member(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const auto t = types.find(view_.nodes[i].type);
        member[i]    = t != types.end() && t->second->shape == NodeShape::Group && t->second->groupMode == GroupMode::Members;
    }
    // Each node's member group, a cycle in the parents cut where it closes.
    std::vector<std::size_t> up(n, kNone), depth(n, 0);
    for (std::size_t i = 0; i < n; ++i)
        if (const std::string& p = view_.nodes[i].parent; !p.empty())
            if (const auto it = at.find(p); it != at.end() && member[it->second] && it->second != i) up[i] = it->second;
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t steps = 0;
        for (std::size_t a = up[i]; a != kNone; a = up[a])
            if (++steps > n) {
                up[i] = kNone;
                steps = 0;
                break;
            }
        depth[i] = steps;
    }
    // What a closed group holds is shown as that group: the outermost closed one around it.
    std::vector<std::size_t> rep(n, kNone);
    std::vector<int>         count(n, 0);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t a = up[i]; a != kNone; a = up[a]) {
            if (collapsed_.contains(view_.nodes[a].id)) rep[i] = a;
            if (!member[i]) ++count[a];
        }
    // Frames: deepest groups first, each around what it holds (a closed one keeps only its header).
    std::vector<std::vector<std::size_t>> kids(n);
    std::vector<std::size_t>              groups;
    for (std::size_t i = 0; i < n; ++i) {
        if (up[i] != kNone) kids[up[i]].push_back(i);
        if (member[i]) groups.push_back(i);
    }
    std::ranges::stable_sort(groups, std::greater<>{}, [&](std::size_t g) { return depth[g]; });
    hasMembers_ = !groups.empty();
    const auto fit = [&] {
        for (const std::size_t g : groups) {
            Rect& f = view_.nodes[g].frame;
            if (!kids[g].empty()) {
                Rect u = view_.nodes[kids[g].front()].frame;
                for (const std::size_t c : kids[g]) u = Rect::Union(u, view_.nodes[c].frame);
                f = {u.x - style.groupPadding, u.y - style.groupHeader, u.w + 2 * style.groupPadding, u.h + style.groupHeader + style.groupPadding};
            }
            if (collapsed_.contains(view_.nodes[g].id)) f.h = style.groupHeader;
        }
    };
    fit();
    // A closed group whose look shows its ends takes its merged wires on two pins: in at its left end, out at its right.
    std::vector<char> ends(n, 0);
    if (kit_)
        for (std::size_t i = 0; i < n; ++i) {
            if (!member[i] || !collapsed_.contains(view_.nodes[i].id)) continue;
            const NodeLook& look = LookOf(view_.nodes[i], StatesOf(view_.nodes[i]) | StateCollapsed);
            if (!look.endPins) continue;
            ends[i] = 1;
            PortDef in{std::string(kGroupIn), {}, look.endPin, PortDir::In, PortSide::Left, 0};
            PortDef out{std::string(kGroupOut), {}, look.endPin, PortDir::Out, PortSide::Right, 0};
            in.onHeader = out.onHeader = true;
            view_.nodes[i].ports.push_back(std::move(in));
            view_.nodes[i].ports.push_back(std::move(out));
        }
    // Wires into a closed group end on it (on its end pins when it shows them); several that then join the same two ends
    // draw as one, dashed.
    std::vector<Edge>               edges;
    std::vector<char>               dashed;
    std::unordered_map<std::string, std::string> seen;   // merged ends -> the edge drawn for them
    edgeShownAs_.clear();
    shownAs_.clear();
    edges.reserve(view_.edges.size());
    for (const Edge& e : view_.edges) {
        const auto a = at.find(e.from), b = at.find(e.to);
        if (a == at.end() || b == at.end()) {
            edges.push_back(e);
            dashed.push_back(0);
            continue;
        }
        const std::size_t ra = rep[a->second] != kNone ? rep[a->second] : a->second;
        const std::size_t rb = rep[b->second] != kNone ? rep[b->second] : b->second;
        if (ra == rb) continue;
        Edge x       = e;
        bool remapped = false;
        if (ra != a->second) x.from = view_.nodes[ra].id, x.fromPort = ends[ra] ? std::string(kGroupOut) : std::string(), remapped = true;
        if (rb != b->second) x.to = view_.nodes[rb].id, x.toPort = ends[rb] ? std::string(kGroupIn) : std::string(), remapped = true;
        if (remapped) {
            const auto [it, fresh] = seen.emplace(x.from + '\x1f' + x.fromPort + '\x1f' + x.to + '\x1f' + x.toPort, x.id);
            if (!fresh) {
                edgeShownAs_.emplace(e.id, it->second);   // its light goes to the wire drawn for it
                continue;
            }
        }
        edges.push_back(std::move(x));
        dashed.push_back(remapped ? 1 : 0);
    }
    // Leave out what closed groups hold.
    std::vector<Node> shown;
    count_.clear();
    shown.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        if (rep[i] == kNone) {
            shown.push_back(std::move(view_.nodes[i]));
            count_.push_back(count[i]);
        } else {
            shownAs_.emplace(ids[i], ids[rep[i]]);
        }
    view_.nodes = std::move(shown);
    view_.edges = std::move(edges);
    dashed_     = std::move(dashed);

    // For FitView: the open groups of the view, deepest first, and what each shows.
    std::unordered_map<std::string_view, std::size_t> shownAt;
    for (std::size_t i = 0; i < view_.nodes.size(); ++i) shownAt.emplace(view_.nodes[i].id, i);
    viewKids_.assign(view_.nodes.size(), {});
    viewUp_.assign(view_.nodes.size(), kNone);
    fitOrder_.clear();
    std::vector<std::size_t> shownDepth(view_.nodes.size(), 0);
    for (std::size_t i = 0; i < view_.nodes.size(); ++i) {
        const std::size_t full = at.at(view_.nodes[i].id);
        shownDepth[i]          = depth[full];
        if (up[full] != kNone) {
            viewUp_[i] = shownAt.at(ids[up[full]]);
            viewKids_[viewUp_[i]].push_back(i);
        }
        if (member[full] && !collapsed_.contains(view_.nodes[i].id)) fitOrder_.push_back(i);
    }
    std::ranges::stable_sort(fitOrder_, std::greater<>{}, [&](std::size_t g) { return shownDepth[g]; });

    // Docked nodes take their places beside their parents, carrying what docked groups hold; the groups fit again.
    IndexDocks();
    if (hasDocks_) {
        std::vector<char> dragged(view_.nodes.size(), 0);
        if (mode_ == Mode::Moving && started_)
            for (const auto& [id, frame] : origin_)
                if (const auto it = shownAt.find(id); it != shownAt.end()) dragged[it->second] = 1;
        PlaceDocks(dragged, {}, nullptr);
        FitView();
    }
}

const Node* Canvas::LiveOf(const Node& shown) const
{
    if (!useView_) return &shown;
    const auto it = graphAt_.find(shown.id);
    if (it == graphAt_.end() || it->second >= graph_->nodes.size() || graph_->nodes[it->second].id != shown.id) return &shown;
    return &graph_->nodes[it->second];
}

bool Canvas::Visible(const Node& n) const
{
    if (n.showWith.empty() || selNodes_.contains(n.id)) return true;
    for (const std::string& id : n.showWith)
        if (selNodes_.contains(id)) return true;
    return false;
}

void Canvas::IndexDocks() const
{
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    const std::size_t     n     = view_.nodes.size();
    hasDocks_ = false;
    dockParent_.assign(n, kNone);
    dockSide_.clear();
    dockBeside_.clear();
    std::unordered_map<std::string_view, std::size_t> at;
    at.reserve(n);
    for (std::size_t i = 0; i < n; ++i) at.emplace(view_.nodes[i].id, i);
    for (std::size_t i = 0; i < n; ++i) {
        const Dock& d = view_.nodes[i].dock;
        if (d.parent.empty()) continue;
        const auto p = at.find(d.parent);
        if (p == at.end() || p->second == i) continue;
        const PortSide side = DockSideOf(d.side);
        if (!d.row.empty() && (side == PortSide::Left || side == PortSide::Right)) dockBeside_[p->second].push_back(i);
        else dockSide_[p->second][static_cast<std::size_t>(DockSlotOf(side))].push_back(i);
        dockParent_[i] = p->second;
        hasDocks_      = true;
    }
    auto byOrder = [&](std::vector<std::size_t>& v) { std::ranges::stable_sort(v, {}, [&](std::size_t i) { return view_.nodes[i].dock.order; }); };
    for (auto& [parent, sides] : dockSide_)
        for (auto& list : sides) byOrder(list);
}

void Canvas::PlaceDocks(const std::vector<char>& dragged, const std::vector<std::size_t>& roots, std::vector<std::size_t>* moved) const
{
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    constexpr std::array  kSides{PortSide::Bottom, PortSide::Top, PortSide::Right, PortSide::Left};
    const std::size_t     n = view_.nodes.size();
    offset_.resize(n);
    visiting_.assign(n, 0);
    auto out = [&](std::size_t c) { return c < dragged.size() && dragged[c]; };   // a dragged node is out of every list

    // Each node's cluster (itself and what is docked to it, at any depth) relative to its own corner.
    std::function<Rect(std::size_t)> cluster = [&](std::size_t i) -> Rect {
        visiting_[i] = 1;
        const Rect& f = view_.nodes[i].frame;
        const Rect  own{0, 0, f.w, f.h};
        Rect        span = own;
        if (const auto it = dockSide_.find(i); it != dockSide_.end())
            for (const PortSide side : kSides) {
                float along = side == PortSide::Bottom ? own.Bottom() + style.dockGap : side == PortSide::Top ? -style.dockGap : 0.f;
                for (const std::size_t c : it->second[static_cast<std::size_t>(DockSlotOf(side))]) {
                    if (visiting_[c] || out(c)) continue;   // a dock cycle ends here; the others close up behind a dragged one
                    const Rect r = cluster(c);
                    Vec2       off;
                    switch (side) {
                    case PortSide::Bottom:
                        if (view_.nodes[c].dock.flush) along -= style.dockGap;
                        off = {0, along - r.y};
                        along += r.h + style.dockGap;
                        break;
                    case PortSide::Top:
                        off = {0, along - r.Bottom()};
                        along -= r.h + style.dockGap;
                        break;
                    case PortSide::Right:
                        off = {own.Right() + style.dockSideGap - r.x, along - r.y};
                        along += r.h + style.dockGap;
                        break;
                    default:
                        off = {own.x - style.dockSideGap - r.Right(), along - r.y};
                        along += r.h + style.dockGap;
                        break;
                    }
                    offset_[c] = off;
                    span       = Rect::Union(span, Rect{r.x + off.x, r.y + off.y, r.w, r.h});
                }
            }
        // Beside a row: level with its line, further out for each one already there.
        if (const auto it = dockBeside_.find(i); it != dockBeside_.end()) {
            float right = own.Right() + style.dockSideGap, left = own.x - style.dockSideGap;
            for (const std::size_t c : it->second) {
                if (visiting_[c] || out(c)) continue;
                const Rect  r      = cluster(c);
                const Dock& d      = view_.nodes[c].dock;
                float       middle = own.h * 0.5f;
                for (std::size_t k = 0; k < view_.nodes[i].rows.size(); ++k)
                    if (view_.nodes[i].rows[k].id == d.row) {
                        const Rect line = RowRect(view_.nodes[i], *schema_, MetricsFor(view_.nodes[i]), k);
                        middle          = line.y - f.y + line.h * 0.5f;
                    }
                const float y = middle - view_.nodes[c].frame.h * 0.5f;
                Vec2        off;
                if (DockSideOf(d.side) == PortSide::Right) {
                    off = {right - r.x, y};
                    right += r.w + style.dockSideGap;
                } else {
                    off = {left - r.Right(), y};
                    left -= r.w + style.dockSideGap;
                }
                offset_[c] = off;
                span       = Rect::Union(span, Rect{r.x + off.x, r.y + off.y, r.w, r.h});
            }
        }
        visiting_[i] = 0;
        return span;
    };

    // What a docked member group holds moves with it.
    std::function<void(std::size_t, Vec2)> shift = [&](std::size_t i, Vec2 d) {
        if (d.x == 0.f && d.y == 0.f) return;
        view_.nodes[i].frame.x += d.x;
        view_.nodes[i].frame.y += d.y;
        if (moved) moved->push_back(i);
        if (i < viewKids_.size())
            for (const std::size_t k : viewKids_[i]) shift(k, d);
    };
    std::function<void(std::size_t)> place = [&](std::size_t i) {
        visiting_[i] = 1;
        const Vec2 corner{view_.nodes[i].frame.x, view_.nodes[i].frame.y};
        auto put = [&](std::size_t c) {
            if (visiting_[c] || out(c)) return;
            shift(c, corner + offset_[c] - Vec2{view_.nodes[c].frame.x, view_.nodes[c].frame.y});
            place(c);
        };
        if (const auto it = dockSide_.find(i); it != dockSide_.end())
            for (const auto& list : it->second)
                for (const std::size_t c : list) put(c);
        if (const auto it = dockBeside_.find(i); it != dockBeside_.end())
            for (const std::size_t c : it->second) put(c);
        visiting_[i] = 0;
    };
    auto layOut = [&](std::size_t r) {
        cluster(r);
        place(r);
    };
    if (!roots.empty()) {
        for (const std::size_t r : roots) layOut(r);
        return;
    }
    // Every cluster: from each node with something docked that is docked to nothing (or is being dragged).
    for (std::size_t i = 0; i < n; ++i)
        if ((dockSide_.contains(i) || dockBeside_.contains(i)) && (dockParent_[i] == kNone || out(i))) layOut(i);
}

std::optional<DockSlot> Canvas::FindDockSlot(std::string_view nodeId, const Rect& frame) const
{
    if (!docking) return std::nullopt;
    const Node* child = graph_->FindNode(nodeId);
    if (!child || frame.w <= 0.f || frame.h <= 0.f) return std::nullopt;
    // A node docked (at any depth) under the child is no parent for it.
    auto under = [&](const Node& p) {
        const Node* at = &p;
        for (std::size_t steps = 0; at && steps <= graph_->nodes.size(); ++steps) {
            if (at->dock.parent.empty()) return false;
            if (at->dock.parent == child->id) return true;
            at = graph_->FindNode(at->dock.parent);
        }
        return at != nullptr;
    };
    std::vector<const Node*> parents;
    if (canDock) {
        for (const Node& p : graph_->nodes)
            if (p.id != child->id && canDock(*child, p) && !under(p)) parents.push_back(&p);
    } else {
        for (const Edge& e : graph_->edges)
            if (e.to == child->id && e.from != child->id)
                if (const Node* p = graph_->FindNode(e.from); p && !under(*p) && std::ranges::find(parents, p) == parents.end()) parents.push_back(p);
    }

    std::optional<DockSlot> best;
    float                   bestShare = 0.25f;   // the slot must take at least a quarter of it
    for (const Node* p : parents) {
        const Node* shown = ShownNode(p->id);
        if (!shown) continue;
        const Rect P = shown->frame;
        if (!P.Inflated(std::max(frame.w, frame.h) * 2.f).Intersects(frame)) continue;
        for (const PortSide side : {PortSide::Bottom, PortSide::Top, PortSide::Right, PortSide::Left}) {
            std::vector<const Node*> list;
            for (const Node& k : graph_->nodes)
                if (k.id != child->id && k.dock.parent == p->id && k.dock.row.empty() && DockSideOf(k.dock.side) == side) list.push_back(&k);
            std::ranges::stable_sort(list, {}, [](const Node* k) { return k->dock.order; });
            float along = 0.f;
            for (std::size_t k = 0; k <= list.size(); ++k) {
                Rect slot;
                switch (side) {
                case PortSide::Bottom: slot = {P.x, P.Bottom() + style.dockGap + along, frame.w, frame.h}; break;
                case PortSide::Top: slot = {P.x, P.y - style.dockGap - along - frame.h, frame.w, frame.h}; break;
                case PortSide::Right: slot = {P.Right() + style.dockSideGap, P.y + along, frame.w, frame.h}; break;
                default: slot = {P.x - style.dockSideGap - frame.w, P.y + along, frame.w, frame.h}; break;
                }
                if (const float share = Overlap(slot, frame) / (frame.w * frame.h); share > bestShare) {
                    bestShare = share;
                    best      = DockSlot{p->id, side, static_cast<int>(k), slot};
                }
                if (k < list.size()) {
                    const Node* ks = ShownNode(list[k]->id);
                    along += (ks ? ks->frame.h : list[k]->frame.h) + style.dockGap;
                }
            }
        }
    }
    return best;
}

void Canvas::DrawDial(std::vector<DrawCmd>& c, const Node& n, const Rect& frame, const NodeType& t, const Camera& cam, bool sel, bool hov) const
{
    constexpr double kPi = std::numbers::pi;
    if (const DialShape shape = DialOf(n, MetricsFor(n), frame); shape.Octagon())
        return DrawOctagonDial(c, n, shape, t, cam, sel, hov);
    const Style&     s   = style;
    const float      z   = cam.zoom;
    const Ring       ring = RingOf(frame);
    const DialAxis   axis = AxisOf(n);
    const Vec2       o    = cam.ToScreen(ring.center);
    const float      R    = ring.radius * z;
    const float      fs   = s.fontSize * z;
    auto dir = [](double a) { return Vec2{static_cast<float>(std::sin(a)), static_cast<float>(-std::cos(a))}; };

    const Rect r = cam.ToScreen(frame);
    // Its look: the card under it, and the rim's parts (the needle, the handles, the labels and the reading).
    const NodeLook& look = LookOf(n, (sel ? StateSelected : 0u) | (hov ? StateHovered : 0u) | StatesOf(n));
    const LookPart* rim  = nullptr;
    for (const LookPart& part : look.parts)
        if (part.kind == PartKind::Rim) rim = &part;
    const LookPart* needle = nullptr;
    const LookPart* handle = nullptr;
    const LookText* label = nullptr;
    const LookText* reading = nullptr;
    if (rim) {
        for (const LookPart& part : rim->content) {
            if (part.id == "needle") needle = &part;
            if (part.id == "handle") handle = &part;
        }
        for (const LookText& line : rim->texts) {
            if (line.id == "label") label = &line;
            if (line.id == "reading") reading = &line;
        }
    }
    const Color needleColor  = needle ? needle->color : s.text;
    const Color handleColor  = handle ? handle->color : Color{220, 180, 60};
    const Color labelColor   = label ? label->color : s.text;
    const Color readingColor = reading ? reading->color : s.muted;
    c.push_back(FillRect{r, look.fill, look.stroke, look.strokeWidth, look.corner * z, look.dash});
    // The ring: whole when cyclic, else a 270° arc open at the bottom.
    const double      a0 = axis.cyclic ? 0.0 : -0.75 * kPi, a1 = axis.cyclic ? 2 * kPi : 0.75 * kPi;
    std::vector<Vec2> arc;
    for (int k = 0; k <= 64; ++k) arc.push_back(o + dir(a0 + (a1 - a0) * k / 64.0) * R);
    c.push_back(Polyline{std::move(arc), rim ? rim->stroke : t.band, std::max(1.5f, (rim ? rim->width : 3.f) * z)});
    const std::string& named = t.titleField.empty() ? std::string{} : n.Field(t.titleField);
    c.push_back(Text{{r.x + s.padding * z, r.y + s.padding * 0.3f * z, r.w - 2 * s.padding * z, s.titleSize * z * 1.3f},
                     named.empty() ? t.label : named, s.titleSize * z, s.text, true, false, false, Align::Center});
    // The needle, and what it reads.
    const Vec2 tip = o + dir(axis.Angle(n.dial.value)) * (R * 0.7f);
    c.push_back(Polyline{{o, tip}, needleColor, needle && needle->width > 0.f ? needle->width : 2.f});
    c.push_back(Circle{o, std::max(2.f, 3.f * z), needleColor, needleColor, 0.f});
    char       buf[32];
    const auto end = std::to_chars(buf, buf + sizeof buf, n.dial.value).ptr;
    c.push_back(Text{{o.x - R, o.y + R * 0.2f, 2 * R, fs * 1.3f}, n.dial.label + "  " + std::string(buf, end),
                     reading ? reading->size * z * reading->scale : fs, readingColor, false, false, false, Align::Center});
    // Each row on the ring: a handle inside it (following a drag), its label outside.
    const float handleSize = handle && handle->size > 0.f ? handle->size : 5.f;
    for (const Row& row : n.rows) {
        if (!OnRim(n, row)) continue;
        const bool  dragging = mode_ == Mode::DialDrag && active_ == n.id && pressRow_ == row.id;
        const float v        = dragging ? dialValue_ : RowNumber(row);
        const auto  a        = axis.Angle(v);
        const Vec2  h        = o + dir(a) * (R * kHandleAt);
        const float d        = std::clamp(handleSize * z, handleSize * 3.f / 5.f, handleSize * 7.f / 5.f);
        if (!handle || (handle->glyph.value_or(PortGlyph::Diamond) == PortGlyph::Diamond && !handle->hollow))
            c.push_back(Polygon{{{h.x, h.y - d}, {h.x + d, h.y}, {h.x, h.y + d}, {h.x - d, h.y}}, handleColor});
        else
            DrawPin(c, h, dir(a), *handle->glyph, d, handleColor, look.fill, !handle->hollow);
        const Vec2  l = o + dir(a) * (R * 1.32f);
        const float w = 110.f * z;
        std::string text = row.label;
        if (dragging) {
            const auto e = std::to_chars(buf, buf + sizeof buf, v).ptr;
            text += " " + std::string(buf, e);
        } else if (!row.value.empty()) {
            text += " " + row.value;
        }
        c.push_back(Text{{l.x - w * 0.5f, l.y - fs * 0.65f, w, fs * 1.3f}, text, label ? label->size * z * label->scale : fs * 0.9f,
                         labelColor, label && label->bold, false, false, Align::Center, label && label->italic});
    }
}

void Canvas::DrawOctagonDial(std::vector<DrawCmd>& c, const Node& n, const DialShape& shape, const NodeType& t, const Camera& cam, bool sel,
                             bool hov) const
{
    constexpr double   kPi   = std::numbers::pi;
    const Style&       s     = style;
    const float        z     = cam.zoom;
    const NodeLook&    look  = LookOf(n, (sel ? StateSelected : 0u) | (hov ? StateHovered : 0u) | StatesOf(n));
    const CardMetrics& km    = look.metrics;
    const Ring&        ring  = shape.rim;
    const DialAxis     axis  = AxisOf(n, ring.OpenReach());
    const Rect&        frame = shape.frame;
    const float        fs    = km.fontSize * z;
    const float        pad   = km.padding * z;
    const LookPart*    head  = nullptr;
    const LookPart*    rim   = nullptr;
    for (const LookPart& part : look.parts) {
        if (part.kind == PartKind::Header) head = &part;
        if (part.kind == PartKind::Rim) rim = &part;
    }
    const LookPart* needle  = nullptr;
    const LookPart* handle  = nullptr;
    const LookText* label   = nullptr;
    const LookText* reading = nullptr;
    if (rim) {
        for (const LookPart& part : rim->content) {
            if (part.id == "needle") needle = &part;
            if (part.id == "handle") handle = &part;
        }
        for (const LookText& line : rim->texts) {
            if (line.id == "label") label = &line;
            if (line.id == "reading") reading = &line;
        }
    }
    const Color needleColor  = needle ? needle->color : s.text;
    const Color handleColor  = handle ? handle->color : Color{220, 180, 60};
    const Color labelColor   = label ? label->color : s.text;
    const Color readingColor = reading ? reading->color : s.muted;

    // The body: the octagon, its header band clipped to it, its outline over both.
    const std::vector<Vec2> body = ToScreen(cam, shape.body);
    c.push_back(Polygon{body, look.fill});
    if (head && head->fill.a > 0) c.push_back(Polygon{ToScreen(cam, ClipAbove(shape.body, frame.y + shape.header)), head->fill});
    std::vector<Vec2> outline = body;
    outline.push_back(body.front());
    c.push_back(Polyline{std::move(outline), look.stroke, look.strokeWidth, look.dash});
    const bool text = fs >= 4.f;

    // The header's lines, between its edges at each line (the cut corners narrow the top).
    auto across = [&](float worldY) {
        const float l = cam.ToScreen(Vec2{shape.EdgeX(worldY, false), worldY}).x + pad;
        const float r = cam.ToScreen(Vec2{shape.EdgeX(worldY, true), worldY}).x - pad;
        return std::pair{l, std::max(l, r)};
    };
    const Rect r = cam.ToScreen(frame);
    if (text && head) {
        float y = r.y + pad * head->top;
        for (const LookText& line : head->texts) {
            const float ts       = line.size * z;
            const auto [l, rr]   = across(frame.y + (y - r.y + ts * line.box * 0.5f) / z);
            const auto [str, em] = LineText(line, n, t);
            c.push_back(Text{{l, y, rr - l, ts * line.box}, str, ts * line.scale, em ? line.placeholderColor : line.color, line.bold, false, false,
                             static_cast<Align>(line.align), line.italic});
            y += ts * line.advance;
        }
    } else if (text) {
        const std::string& named = t.titleField.empty() ? std::string{} : n.Field(t.titleField);
        const auto [l, rr]       = across(frame.y + shape.header * 0.5f);
        c.push_back(Text{{l, r.y + pad * 0.3f, rr - l, s.titleSize * z * 1.3f}, named.empty() ? t.label : named, s.titleSize * z, s.text, true,
                         false, false, Align::Center});
    }

    // Its listed rows, as a card's: a label on the left, a value on the right.
    float y = cam.ToScreen(Vec2{0.f, shape.rowsTop}).y;
    const float rowH = km.Row() * z;
    for (const Row& row : n.rows) {
        if (OnRim(n, row)) continue;
        if (text) {
            static const RowLook kPlainRow{};
            const auto           kind = look.rows.find(row.style);
            const RowLook&       rl   = kind == look.rows.end() ? kPlainRow : kind->second;
            const auto [l, rr]        = across(frame.y + (y - r.y + rowH * 0.5f) / z);
            std::string right         = row.value + rl.suffix;
            const float rw            = right.empty() ? 0.f : std::min((rr - l) * 0.5f, fs * 0.6f * static_cast<float>(right.size()) + pad);
            c.push_back(Text{{l, y, rr - l - rw, rowH}, rl.prefix + row.label, fs, rl.color.value_or(s.text), rl.bold, false, false,
                             static_cast<Align>(rl.align), rl.italic});
            if (!right.empty())
                c.push_back(Text{{rr - rw, y, rw, rowH}, right, fs, rl.valueColor.value_or(s.muted), false, false, false,
                                 static_cast<Align>(rl.valueAlign), rl.italic});
        }
        y += rowH;
    }

    // The rim: whole when cyclic, else open along its flat bottom edge.
    const double reach = axis.reach * kPi;
    const double a0 = axis.cyclic ? 0.0 : -reach, a1 = axis.cyclic ? 2 * kPi : reach;
    std::vector<Vec2> edge = ToScreen(cam, ring.Outline(a0, a1));
    if (axis.cyclic) edge.push_back(edge.front());
    c.push_back(Polyline{std::move(edge), rim ? rim->stroke : t.band, std::max(1.f, (rim ? rim->width : 3.f) * z)});
    // The needle, and what it reads: an open dial says it on the gap, its flat bottom edge; a cyclic one under the hub.
    const Vec2 o   = cam.ToScreen(ring.center);
    const Vec2 tip = cam.ToScreen(ring.At(axis.Angle(n.dial.value), 0.7f));
    c.push_back(Polyline{{o, tip}, needleColor, needle && needle->width > 0.f ? needle->width : 2.f});
    c.push_back(Circle{o, std::max(2.f, 3.f * z), needleColor, needleColor, 0.f});
    char       buf[32];
    const auto end   = std::to_chars(buf, buf + sizeof buf, n.dial.value).ptr;
    const float rs   = reading ? reading->size * z * reading->scale : fs;
    const Vec2  gap  = cam.ToScreen(Vec2{ring.center.x, ring.center.y + ring.radius});
    const float gapW = 2.f * ring.radius * (1.f - std::min(ring.chamfer, 1.f)) * z + 2.f * pad;
    if (text)
        c.push_back(Text{axis.cyclic ? Rect{o.x - ring.radius * z, o.y + ring.radius * z * 0.35f, 2 * ring.radius * z, rs * 1.3f}
                                     : Rect{gap.x - gapW * 0.5f, gap.y - rs * 0.65f, gapW, rs * 1.3f},
                         n.dial.label + "  " + std::string(buf, end), rs, readingColor, false, false, false, Align::Center});
    // Each row on the rim: a handle inside it, following a drag along the edges; its label outside, square to its edge.
    const float handleSize = handle && handle->size > 0.f ? handle->size : 5.f;
    for (const Row& row : n.rows) {
        if (!OnRim(n, row)) continue;
        const bool   dragging = mode_ == Mode::DialDrag && active_ == n.id && pressRow_ == row.id;
        const float  v        = dragging ? dialValue_ : RowNumber(row);
        const double a        = axis.Angle(v);
        const Vec2   h        = cam.ToScreen(ring.At(a, kHandleAt));
        const Vec2   normal   = ring.Normal(a);
        const float  d        = std::clamp(handleSize * z, handleSize * 3.f / 5.f, handleSize * 7.f / 5.f);
        if (!handle || (handle->glyph.value_or(PortGlyph::Diamond) == PortGlyph::Diamond && !handle->hollow))
            c.push_back(Polygon{{{h.x, h.y - d}, {h.x + d, h.y}, {h.x, h.y + d}, {h.x - d, h.y}}, handleColor});
        else
            DrawPin(c, h, normal, *handle->glyph, d, handleColor, look.fill, !handle->hollow);
        if (!text) continue;
        std::string what = row.label;
        if (dragging) {
            const auto e = std::to_chars(buf, buf + sizeof buf, v).ptr;
            what += " " + std::string(buf, e);
        } else if (!row.value.empty()) {
            what += " " + row.value;
        }
        const float ls = label ? label->size * z * label->scale : fs * 0.9f;
        // As wide as it reads (by code point, about 0.62 of its size each), so a long name keeps its value.
        std::size_t points = 0;
        for (const char ch : what) points += (static_cast<unsigned char>(ch) & 0xC0) != 0x80 ? 1 : 0;
        const float w  = std::max(110.f * z, ls * 0.62f * static_cast<float>(points) + 8.f * z), lh = ls * 1.3f;
        const Vec2  at = cam.ToScreen(ring.At(a)) + normal * (10.f * z);
        Rect        box{at.x - w * 0.5f, at.y - lh * 0.5f, w, lh};
        Align       align = Align::Center;
        if (normal.x > 0.5f) box.x = at.x, align = Align::Left;
        if (normal.x < -0.5f) box.x = at.x - w, align = Align::Right;
        if (normal.y < -0.5f) box.y = at.y - lh;
        if (normal.y > 0.5f) box.y = at.y;
        c.push_back(Text{box, what, ls, labelColor, label && label->bold, false, false, align, label && label->italic});
    }
}

void Canvas::FitView() const
{
    for (const std::size_t g : fitOrder_) {
        if (viewKids_[g].empty()) continue;
        Rect u = view_.nodes[viewKids_[g].front()].frame;
        for (const std::size_t c : viewKids_[g]) u = Rect::Union(u, view_.nodes[c].frame);
        view_.nodes[g].frame = {u.x - style.groupPadding, u.y - style.groupHeader, u.w + 2 * style.groupPadding,
                                u.h + style.groupHeader + style.groupPadding};
    }
}

void Canvas::Sync() const
{
    const bool     dragging = (mode_ == Mode::Moving || mode_ == Mode::Resizing) && started_;
    // With a backend, a slow live reroute leaves the settled wires alone and patches only the dragged nodes' wires.
    const bool     patch = router && dragging && backendMs_ > kLiveRouteMs;
    // A large graph without one does the same with the built-in router: only the dragged nodes' wires move.
    // A host change since the last frame rebuilds everything once, even mid-drag.
    const bool     local = !router && dragging && graph_->nodes.size() > kLargeGraph && wiresValid_ && synced_ == graph_ && !resync_;
    resync_              = false;
    const bool     plain = (!router || patch) && dragging && routeMs_ > kLiveRouteMs && !local;
    RoutingOptions o     = routing;
    if (plain) o.avoidNodes = false;
    EnsureKit();
    o.card  = MetricsOf(style);
    o.cards = kitCards_;
    o.lines = kitLines_;

    std::size_t optionsKey = Mix(reinterpret_cast<std::uintptr_t>(router.get()),
                                 (o.avoidNodes ? 1u : 0u) | (o.spreadPorts ? 2u : 0u));
    for (float f : {o.clearance, o.bendCost, o.shareCost, o.crossCost, o.laneSpacing, o.cornerRadius, o.curveRadius,
                    o.card.padding, o.card.titleSize, o.card.fontSize, o.card.groupHeader, style.groupPadding})
        optionsKey = Mix(optionsKey, Bits(f));
    optionsKey = Mix(optionsKey, static_cast<std::size_t>(kitGen_));

    const auto  hs  = HashText;
    // A drag takes the dragged nodes out of their docks while it lasts, so whether one is under way is part of the key.
    std::size_t key = Mix(reinterpret_cast<std::uintptr_t>(schema_),
                          (plain ? 1u : 0u) | (patch ? 2u : 0u) | (local ? 4u : 0u) | (dragging && mode_ == Mode::Moving && hasDocks_ ? 8u : 0u));
    key = Mix(key, reinterpret_cast<std::uintptr_t>(graph_));
    key = Mix(Mix(Mix(key, optionsKey), static_cast<std::size_t>(schema_->revision)), viewRev_);
    // During a drag only the canvas changes the graph, and only the dragged frames: after its first step the key is the
    // one the drag began with, mixed with those, instead of every node and edge hashed again.
    const bool shortcut = local && dragKey_ != 0;
    if (!shortcut) {
        for (const Node& n : graph_->nodes) {
            key = Mix(Mix(Mix(key, hs(n.id)), hs(n.type)), n.parent.empty() ? 0 : hs(n.parent));
            if (!n.dock.parent.empty())
                key = Mix(Mix(Mix(key, hs(n.dock.parent)), hs(n.dock.row)),
                          static_cast<std::size_t>(n.dock.order) * 16 + static_cast<std::size_t>(n.dock.side) * 2 + (n.dock.flush ? 1 : 0));
            for (float f : {n.frame.x, n.frame.y, n.frame.w, n.frame.h}) key = Mix(key, Bits(f));
            for (const PortDef& p : n.ports)
                key = Mix(Mix(key, hs(p.id)), static_cast<std::size_t>(p.side) * 8 + static_cast<std::size_t>(p.dir) * 2 + (p.onHeader ? 1 : 0));
            // Rows move pins (a row claims a port; a dial places it by the row's value) and grow a fitted card.
            key = Mix(key, n.rows.size());
            for (std::size_t k = 0; k < n.rows.size(); ++k)
                if (!n.rows[k].port.empty()) key = Mix(Mix(Mix(key, k), hs(n.rows[k].port)), hs(n.rows[k].value));
        }
        for (const Edge& e : graph_->edges) {
            for (const std::string* s : {&e.id, &e.from, &e.to, &e.type, &e.fromPort, &e.toPort}) key = Mix(key, hs(*s));
            key = Mix(key, static_cast<std::size_t>(o.LineFor(e.type, *schema_)) + 1);
        }
    }
    if (local) {
        if (!shortcut) dragKey_ = key;
        key = dragKey_;
        for (const auto& [id, frame] : origin_)
            if (const Node* n = graph_->FindNode(id)) key = Mix(Mix(key, Bits(n->frame.x)), Bits(n->frame.y));
    } else {
        dragKey_ = 0;
    }
    if (wiresValid_ && key == wiresKey_) {
        synced_      = graph_;
        syncedEdits_ = edits_;
        return;
    }

    std::unordered_map<std::string_view, const NodeType*> types;
    types.reserve(schema_->nodeTypes.size());
    for (const NodeType& t : schema_->nodeTypes) types.emplace(t.id, &t);

    const auto start   = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
    if (local) {
        // Only the wires of the dragged nodes (and of the open groups around them, which grow with them) reroute, among
        // the nodes they join; the rest keep their settled paths.
        std::unordered_set<std::string_view> moving;
        for (const auto& [id, frame] : origin_) moving.insert(id);
        bool rebuilt = false;
        if (useView_) {
            for (const auto& [id, frame] : origin_) {
                const auto it = index_.find(id);
                const Node* real = graph_->FindNode(id);
                if (it == index_.end() || !real) rebuilt = true;
                else view_.nodes[it->second].frame = real->frame;
            }
            if (rebuilt) {
                BuildView();   // a closed group moved with what it holds: start over
            } else {
                if (hasDocks_) {
                    // Docked nodes close up behind a dragged one and follow a dragged parent: only the clusters the
                    // drag touches are placed again, and the nodes that moved reroute too.
                    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
                    std::vector<char>        dragged(view_.nodes.size(), 0);
                    std::vector<std::size_t> roots, shifted;
                    for (const auto& [id, frame] : origin_)
                        if (const auto it = index_.find(id); it != index_.end()) dragged[it->second] = 1;
                    for (const auto& [id, frame] : origin_)
                        if (const auto it = index_.find(id); it != index_.end()) {
                            roots.push_back(it->second);
                            std::size_t top = dockParent_[it->second];
                            for (std::size_t steps = 0; top != kNone && dockParent_[top] != kNone && !dragged[top] && steps < dockParent_.size(); ++steps)
                                top = dockParent_[top];
                            if (top != kNone) roots.push_back(top);
                        }
                    PlaceDocks(dragged, roots, &shifted);
                    for (const std::size_t i : shifted) moving.insert(view_.nodes[i].id);
                }
                FitView();
            }
            if (!rebuilt)
                for (const auto& [id, frame] : origin_)
                    if (const auto it = index_.find(id); it != index_.end())
                        for (std::size_t a = viewUp_[it->second]; a != static_cast<std::size_t>(-1); a = viewUp_[a]) moving.insert(view_.nodes[a].id);
        }
        // The touched wires come from each moving node's own list, not a pass over every edge.
        const Graph&             shown = Shown();
        Graph                    sub;
        std::vector<std::size_t> touched, ends;
        for (const std::string_view id : moving)
            if (const auto it = index_.find(id); it != index_.end() && it->second < incident_.size())
                touched.insert(touched.end(), incident_[it->second].begin(), incident_[it->second].end());
        std::ranges::sort(touched);
        touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
        for (const std::size_t i : touched) {
            const Edge& e = shown.edges[i];
            sub.edges.push_back(e);
            for (const std::string* end : {&e.from, &e.to})
                if (const auto it = index_.find(*end); it != index_.end()) ends.push_back(it->second);
        }
        std::ranges::sort(ends);
        ends.erase(std::unique(ends.begin(), ends.end()), ends.end());
        for (const std::size_t i : ends) sub.nodes.push_back(shown.nodes[i]);
        for (auto& [id, path] : RouteWires(sub, *schema_, o, &sides_)) wires_[id] = std::move(path);
        if (!rebuilt && paths_.size() == shown.edges.size()) {
            // A drag changes no node or edge, only frames: the tables keep their shape, and only the touched wires'
            // paths and bounds change.
            for (const std::size_t i : touched) SetPath(i);
            wiresKey_    = key;
            synced_      = graph_;
            syncedEdits_ = edits_;
            return;
        }
    } else {
        useView_ = false;
        for (const Node& n : graph_->nodes)
            if (const auto t = types.find(n.type);
                !n.dock.parent.empty() ||
                (t != types.end() && ((t->second->groupMode == GroupMode::Members && t->second->shape == NodeShape::Group) || t->second->fitHeight)) ||
                fitWidth_.contains(n.type)) {
                useView_ = true;
                break;
            }
        if (useView_) BuildView();
        else view_ = {};
        const Graph& shown = Shown();
        if (router && !patch) {
            // A backend's first route with a router or options builds everything; only the reroutes after it time a drag step.
            const bool incremental = backendKey_ == optionsKey && !routed_.empty();
            routed_     = router->Route(shown, *schema_, o);
            backendMs_  = incremental ? elapsed() : -1.0;
            backendKey_ = optionsKey;
            wires_      = routed_;
        } else if (patch) {
            std::unordered_set<std::string_view> moving;
            for (const auto& [id, frame] : origin_) moving.insert(id);
            const WirePaths quick = RouteWires(shown, *schema_, o);
            if (!plain) routeMs_ = elapsed();
            wires_.clear();
            for (const Edge& e : shown.edges) {
                const bool touched = moving.contains(e.from) || moving.contains(e.to);
                const auto settled = routed_.find(e.id);
                const auto live    = quick.find(e.id);
                if (!touched && settled != routed_.end()) wires_.emplace(e.id, settled->second);
                else if (live != quick.end()) wires_.emplace(e.id, live->second);
            }
        } else {
            wires_ = RouteWires(shown, *schema_, o, &sides_);
            if (!plain) routeMs_ = elapsed();
        }
    }
    wiresKey_    = key;
    wiresValid_  = true;
    synced_      = graph_;
    syncedEdits_ = edits_;
    syncedNodes_ = graph_->nodes.size();
    syncedEdges_ = graph_->edges.size();

    // Per shown node and edge, so a frame needs no lookups by id.
    const Graph&      shown = Shown();
    const std::size_t nodes = shown.nodes.size();
    grouped_.assign(nodes, 0);
    index_.clear();
    index_.reserve(nodes);
    for (std::size_t i = 0; i < nodes; ++i) {
        const Node& n = shown.nodes[i];
        const auto  t = types.find(n.type);
        grouped_[i]   = t != types.end() && t->second->shape == NodeShape::Group;
        index_.emplace(n.id, i);
    }
    if (!useView_) {
        dashed_.assign(shown.edges.size(), 0);
        count_.assign(nodes, 0);
    }
    paths_.assign(shown.edges.size(), nullptr);
    boxes_.assign(shown.edges.size(), Rect{});
    wiredPorts_.clear();
    incident_.assign(nodes, {});
    for (std::size_t i = 0; i < shown.edges.size(); ++i) {
        const Edge& e = shown.edges[i];
        if (!e.fromPort.empty()) wiredPorts_.insert(PortKey(e.from, e.fromPort));
        if (!e.toPort.empty()) wiredPorts_.insert(PortKey(e.to, e.toPort));
        for (const std::string* end : {&e.from, &e.to})
            if (const auto it = index_.find(*end); it != index_.end()) incident_[it->second].push_back(static_cast<std::uint32_t>(i));
        SetPath(i);
    }
}

void Canvas::SetPath(std::size_t i) const
{
    paths_[i] = nullptr;
    const auto w = wires_.find(Shown().edges[i].id);
    if (w == wires_.end() || w->second.size() < 2) return;
    paths_[i] = &w->second;
    Vec2 lo = w->second.front(), hi = lo;
    for (const Vec2& p : w->second) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
    }
    boxes_[i] = Rect::FromPoints(lo, hi);
}

// ── Gestures ─────────────────────────────────────────────────────────────────

bool Canvas::PointerDown(Vec2 at, Button button, Modifiers mods)
{
    downAt_ = pointer_ = at;
    started_ = false;
    origin_.clear();
    dragKey_ = 0;   // a new gesture: the next drag keys from scratch

    if (button != Button::Left) {
        mode_      = panWithButtons ? Mode::Panning : Mode::Idle;
        panAtDown_ = camera.pan;
        return false;
    }

    const Hit hit = HitTest(at);
    downHit_      = hit;
    downMods_     = mods;
    switch (hit.kind) {
    case HitKind::Handle:
        if (!selNodes_.contains(hit.id)) SelectNodes({hit.id});
        mode_     = Mode::DialDrag;
        active_   = hit.id;
        pressRow_ = hit.row;
        if (const Node* n = ShownNode(hit.id))
            if (const Row* r = n->FindRow(hit.row)) dialValue_ = RowNumber(*r);
        return true;
    case HitKind::Connect:
    case HitKind::Port:
        mode_       = Mode::Connecting;
        active_     = hit.id;
        activePort_ = hit.port;
        connectTo_.clear();
        connectPort_.clear();
        lifted_.clear();
        if (liftWires && !hit.port.empty())
            // A full one-wire input gives its wire up to the hand: the drag carries it from its source.
            if (const Node* n = graph_->FindNode(hit.id))
                if (const auto def = PortOf(*n, *schema_, hit.port); def && def->dir == PortDir::In && MaxWires(*def) == 1)
                    for (const Edge& e : graph_->edges)
                        if (e.to == hit.id && e.toPort == hit.port) {
                            lifted_     = e.id;
                            active_     = e.from;
                            activePort_ = e.fromPort;
                            break;
                        }
        return true;
    case HitKind::Resize:
        if (!selNodes_.contains(hit.id)) SelectNodes({hit.id});
        mode_   = Mode::Resizing;
        active_ = hit.id;
        origin_.emplace_back(hit.id, graph_->FindNode(hit.id)->frame);
        return true;
    case HitKind::Row:
    case HitKind::Node: {
        pressed_ = hit.id;
        // The start of a member group's header is its arrow: a press there opens or closes it.
        if (const Node* g = ShownNode(hit.id); g && hit.kind == HitKind::Node && IsMemberGroup(*g, *schema_)) {
            const Rect  r    = camera.ToScreen(g->frame);
            const float head = style.groupHeader * camera.zoom;
            if (at.y <= r.y + head && at.x <= r.x + head) {
                SetCollapsed(hit.id, !IsCollapsed(hit.id));
                mode_ = Mode::Idle;
                return true;
            }
        }
        if (mods.ctrl || mods.shift) {
            NodeSet next = selNodes_;
            if (selectKeys == SelectKeys::Toggle) {
                if (!next.erase(hit.id)) next.insert(hit.id);
            } else if (mods.ctrl) {
                next.erase(hit.id);
            } else {
                next.insert(hit.id);
            }
            SelectNodes(std::move(next));
            mode_ = Mode::Idle;
            return true;
        }
        if (!selNodes_.contains(hit.id)) SelectNodes({hit.id});
        // A press on a row that acts is a click on it unless it drags, when it moves the node like any other press.
        const Node* n = graph_->FindNode(hit.id);
        const Row*  r = hit.kind == HitKind::Row && n ? n->FindRow(hit.row) : nullptr;
        // A row of a list a kit lets reorder drags among its list instead.
        if (r && kit_ && !RowLookOf(*n, *r).reorder.empty()) {
            mode_      = Mode::RowReorder;
            active_    = hit.id;
            pressRow_  = hit.row;
            reorderTo_ = -1;
            return true;
        }
        if (r && r->action != RowAction::None) {
            mode_     = Mode::RowPress;
            active_   = hit.id;
            pressRow_ = hit.row;
            return true;
        }
        StartMove();
        return true;
    }
    case HitKind::Edge:
        SelectEdge(hit.id);
        mode_ = Mode::Idle;
        return true;
    case HitKind::None:
    default:
        subtract_ = selectKeys == SelectKeys::ExtendSubtract && mods.ctrl;
        additive_ = !subtract_ && (mods.ctrl || mods.shift);
        if (!additive_ && !subtract_) ClearSelection();
        mode_ = Mode::Marquee;
        return true;
    }
}

void Canvas::StartMove()
{
    mode_ = Mode::Moving;
    origin_.clear();
    // A selected group takes everything it holds along.
    carried_.clear();
    for (const Node& n : graph_->nodes)
        if (selNodes_.contains(n.id) && IsGroup(n, *schema_))
            for (std::string& m : GroupMembers(*graph_, *schema_, n.id))
                if (!selNodes_.contains(m)) carried_.insert(std::move(m));
    for (const Node& n : graph_->nodes)
        if (selNodes_.contains(n.id) || carried_.contains(n.id)) {
            // A docked node moves from where it is drawn, not from the frame it had before it docked.
            const Node* shown = n.dock.parent.empty() ? nullptr : ShownNode(n.id);
            origin_.emplace_back(n.id, shown ? shown->frame : n.frame);
        }
    // The member groups a drop may join, as they are now: a group grows with a member dragged inside it.
    Sync();
    dropTargets_.clear();
    for (const Node& n : Shown().nodes)
        if (IsMemberGroup(n, *schema_) && !selNodes_.contains(n.id) && !carried_.contains(n.id)) dropTargets_.emplace_back(n.id, n.frame);
}

bool Canvas::PointerMove(Vec2 at, Modifiers mods)
{
    pointer_  = at;
    snapDial_ = mods.shift;
    if (mode_ != Mode::Idle && !started_) {
        if (Distance(at, downAt_) < kDragThreshold) return false;
        started_ = true;
        if (mode_ == Mode::RowPress) StartMove();   // a row dragged moves its node
        if (mode_ == Mode::Moving) Before("Move");
        if (mode_ == Mode::Resizing) Before("Resize");
    }
    const Vec2 delta = (at - downAt_) / camera.zoom;

    switch (mode_) {
    case Mode::Idle: {
        const Hit   hit  = HitTest(at);
        std::string over = hit.kind == HitKind::Edge ? std::string{} : hit.id;
        if (over == hovered_) return false;
        hovered_ = std::move(over);
        if (onHoverChanged) onHoverChanged(hovered_);
        return true;
    }
    case Mode::Moving:
        for (const auto& [id, frame] : origin_)
            if (Node* n = graph_->FindNode(id)) {
                n->frame.x = Snap(frame.x + delta.x);
                n->frame.y = Snap(frame.y + delta.y);
            }
        return true;
    case Mode::Resizing:
        if (Node* n = graph_->FindNode(active_)) {
            const Rect& o = origin_.front().second;
            n->frame.w    = std::max(style.minNode, Snap(o.x + o.w + delta.x) - o.x);
            n->frame.h    = std::max(style.minNode * 0.5f, Snap(o.y + o.h + delta.y) - o.y);
        }
        return true;
    case Mode::Connecting: {
        const Hit hit = HitAt(at, true);   // a group's interior is a target too
        connectTo_.clear();
        connectPort_.clear();
        if (const Edge* held = lifted_.empty() ? nullptr : graph_->FindEdge(lifted_); held && hit.id == held->to && hit.port == held->toPort) {
            connectTo_   = held->to;   // back where it was lifted from
            connectPort_ = held->toPort;
            return true;
        }
        const Node* from = graph_->FindNode(active_);
        if (!from || hit.kind == HitKind::None || hit.kind == HitKind::Edge || hit.id == active_) return true;
        if (hit.kind == HitKind::Port) {
            if (CanWire(active_, activePort_, hit.id, hit.port)) {
                connectTo_   = hit.id;
                connectPort_ = hit.port;
            }
            return true;
        }
        // Dropped on a node as a whole: a wire from a port lands on the first of its ports that takes it.
        const Node* to = graph_->FindNode(hit.id);
        if (!to) return true;
        if (activePort_.empty() || PortsOf(*to, *schema_).empty()) {
            if (CanWire(active_, activePort_, hit.id, {})) connectTo_ = hit.id;
        } else if (std::string p = LandingPort(*from, activePort_, *to); !p.empty()) {
            connectTo_   = hit.id;
            connectPort_ = std::move(p);
        }
        return true;
    }
    case Mode::DialDrag:
        if (const Node* n = ShownNode(active_)) {
            const Ring     ring = DialOf(*n, MetricsFor(*n), n->frame).rim;
            const DialAxis axis = AxisOf(*n, ring.OpenReach());
            const Vec2     d    = camera.ToWorld(at) - ring.center;
            const float    v    = axis.Value(std::atan2(d.x, -d.y));
            dialValue_          = snapDial_ ? axis.Snap(v) : v;
        }
        return true;
    case Mode::Panning:
        camera.pan = panAtDown_ + (at - downAt_);
        return true;
    case Mode::RowReorder:
        if (const Node* n = graph_->FindNode(active_)) reorderTo_ = ReorderTarget(*n, pressRow_, at);
        return true;
    case Mode::Marquee:
    default:
        return true;
    }
}

bool Canvas::PointerUp(Vec2 at, Button)
{
    PointerMove(at, Modifiers{snapDial_, false, false});   // the last move keeps the snap the drag had
    dragKey_ = 0;   // the drop may settle, re-parent or dock: key it in full
    const Mode mode = mode_;
    mode_           = Mode::Idle;

    switch (mode) {
    case Mode::DialDrag: {
        if (!started_) return false;
        const Node* n = graph_->FindNode(active_);
        const Row*  r = n ? n->FindRow(pressRow_) : nullptr;
        if (!r) return true;
        char       buf[32];
        const auto end = std::to_chars(buf, buf + sizeof buf, dialValue_).ptr;
        SetValue   set{active_, pressRow_, r->field, std::string(buf, end)};
        Before("Set " + (r->field.empty() ? r->label : r->field));
        Node* m = graph_->FindNode(active_);
        if (!set.field.empty()) m->fields[set.field] = set.value;
        for (Row& row : m->rows)
            if (row.id == pressRow_) row.value = set.value;
        Changed(Intent{"Set " + (set.field.empty() ? set.row : set.field), std::move(set)});
        return true;
    }
    case Mode::RowPress:
        if (const Node* n = graph_->FindNode(active_))
            if (const Row* r = n->FindRow(pressRow_)) rowClick_ = RowClick{active_, pressRow_, r->action, at};
        Gesture("click", downHit_, at, downMods_);
        return true;
    case Mode::RowReorder: {
        if (!started_) {
            Gesture("click", downHit_, at, downMods_);
            return true;
        }
        const int   to = std::exchange(reorderTo_, -1);
        const Node* n  = graph_->FindNode(active_);
        int         from = -1;
        for (std::size_t k = 0; n && k < n->rows.size(); ++k)
            if (n->rows[k].id == pressRow_) from = static_cast<int>(k);
        if (from < 0 || to < 0 || to == from) return true;
        Before("Reorder");
        Node* m   = graph_->FindNode(active_);
        Row   row = std::move(m->rows[static_cast<std::size_t>(from)]);
        m->rows.erase(m->rows.begin() + from);
        m->rows.insert(m->rows.begin() + to, std::move(row));
        Changed(Intent{"Reorder", ReorderRow{active_, pressRow_, to}});
        return true;
    }
    case Mode::Moving:
        if (started_) {
            // Dropped with its centre in a group: it goes in. Members carried by a moved group stay where they are in it.
            NodeSet roots, moving;
            for (const auto& [id, frame] : origin_) {
                moving.insert(id);
                if (!carried_.contains(id)) roots.insert(id);
            }
            for (const auto& [id, frame] : origin_)
                if (const Node* n = graph_->FindNode(id); n && IsGroup(*n, *schema_))
                    for (const std::string& m : GroupMembers(*graph_, *schema_, id)) roots.erase(m);
            Settle(roots, moving);
            if (docking && roots.size() == 1) {
                // One node let go over a slot docks there, the slots after it moving out; let go anywhere else, a
                // docked node undocks where it is.
                Node* n = graph_->FindNode(*roots.begin());
                if (const auto slot = n ? FindDockSlot(n->id, n->frame) : std::nullopt) {
                    // The side's list, renumbered from nearest, with the node in its slot.
                    std::vector<Node*> list;
                    for (Node& s : graph_->nodes)
                        if (s.id != n->id && s.dock.parent == slot->parent && s.dock.row.empty() && DockSideOf(s.dock.side) == slot->side)
                            list.push_back(&s);
                    std::ranges::stable_sort(list, {}, [](const Node* s) { return s->dock.order; });
                    n->dock = Dock{slot->parent, slot->side, slot->index};
                    list.insert(list.begin() + std::min<std::ptrdiff_t>(slot->index, std::ssize(list)), n);
                    for (std::size_t k = 0; k < list.size(); ++k) list[k]->dock.order = static_cast<int>(k);
                    n->frame.x = slot->at.x;
                    n->frame.y = slot->at.y;
                } else if (n && !n->dock.parent.empty()) {
                    n->dock = {};
                }
            }
            if (hasMembers_) {
                // Dropped on a member group, a node joins the innermost one under the pointer; dropped on none, it
                // leaves its own.
                const Vec2                                 w      = camera.ToWorld(at);
                const std::pair<std::string, Rect>*        target = nullptr;
                for (const auto& t : dropTargets_)
                    if (t.second.Contains(w) && (!target || t.second.Area() < target->second.Area())) target = &t;
                const std::string parent = target ? target->first : std::string{};
                for (const std::string& id : roots)
                    if (Node* n = graph_->FindNode(id); n && n->parent != parent && n->id != parent) n->parent = parent;
            }
            Changed(Intent{"Move", MovedFrames()});
        } else if (Gesture("click", downHit_, at, downMods_)) {
            // a click the kit binds: its action runs
        } else if (followAliases) {
            // A click on an alias goes to the original.
            if (const Node* n = graph_->FindNode(pressed_); n && !n->aliasOf.empty() && graph_->FindNode(n->aliasOf)) {
                const std::string original = n->aliasOf;
                SelectNodes({original});
                Reveal(original);
                EnsureVisible(original);
            }
        }
        carried_.clear();
        return true;
    case Mode::Resizing:
        if (started_) Changed(Intent{"Resize", MovedFrames()});
        return true;
    case Mode::Connecting: {
        const std::string to     = std::exchange(connectTo_, {});
        const std::string toPort = std::exchange(connectPort_, {});
        const std::string held   = std::exchange(lifted_, {});
        if (!started_ && Gesture("click", downHit_, at, downMods_)) return true;   // a click on a pin the kit binds
        const Edge*       lifted = held.empty() ? nullptr : graph_->FindEdge(held);
        if (lifted && to == lifted->to && toPort == lifted->toPort) return true;   // put back where it was
        if (lifted && to.empty()) {
            // A lifted wire let go on nothing comes off.
            Before("Disconnect");
            std::erase_if(graph_->edges, [&](const Edge& x) { return x.id == held; });
            if (selEdge_ == held) selEdge_.clear();
            Changed(Intent{"Disconnect", RemoveItems{{}, {held}}});
            return true;
        }
        if (!to.empty()) Wire(active_, activePort_, to, toPort, held);   // one edge per pair of ends (CanWire)
        else if (started_ && onIntent && HitTest(at).kind == HitKind::None) {
            // Let go on nothing: the pin's binding (a menu of what may take it), else the host hears of it.
            const Node*       from = graph_->FindNode(active_);
            const std::string type = from && !activePort_.empty() ? PortTypeOf(*graph_, *schema_, *from, activePort_) : std::string();
            if (!Gesture("dragToNothing", Hit{HitKind::Port, active_, activePort_}, at, downMods_))
                onIntent(Intent{"Connect", ConnectToNothing{active_, activePort_, camera.ToWorld(at), type}});
        }
        return true;
    }
    case Mode::Marquee: {
        if (!started_) return true;
        const Rect box  = camera.ToWorld(Rect::FromPoints(downAt_, at));
        NodeSet    next = additive_ || subtract_ ? selNodes_ : NodeSet{};
        // A group is picked only when the box takes in all of it, so a marquee started inside one selects what it holds.
        for (const Node& n : graph_->nodes)
            if (IsGroup(n, *schema_) ? box.Holds(n.frame) : box.Intersects(n.frame)) {
                if (subtract_) next.erase(n.id);
                else next.insert(n.id);
            }
        SelectNodes(std::move(next));
        return true;
    }
    default:
        return started_;
    }
}

// ── Drawing ──────────────────────────────────────────────────────────────────

DrawList Canvas::Draw() const { return Render(camera, viewport_, true); }

std::pair<Vec2, float> Canvas::MinimapFrame(Vec2 size) const
{
    constexpr float kReadable = 0.02f;   ///< below this the minimap stops shrinking and follows the view instead
    Sync();
    const Graph& shown = Shown();
    if (shown.nodes.empty() || size.x <= 0.f || size.y <= 0.f) return {{}, 1.f};
    const Rect  b     = shown.Bounds().Inflated(40.f);
    const float fit   = std::min(size.x / std::max(b.w, 1.f), size.y / std::max(b.h, 1.f));
    if (fit >= kReadable) return {Vec2{b.x - (size.x / fit - b.w) * 0.5f, b.y - (size.y / fit - b.h) * 0.5f}, fit};
    // Too big to read whole: a readable scale, centred on the view, kept inside the graph.
    const Vec2 middle = camera.ToWorld(viewport_ * 0.5f);
    const Vec2 span   = size / kReadable;
    const Vec2 origin{std::clamp(middle.x - span.x * 0.5f, b.x, std::max(b.x, b.Right() - span.x)),
                      std::clamp(middle.y - span.y * 0.5f, b.y, std::max(b.y, b.Bottom() - span.y))};
    return {origin, kReadable};
}

DrawList Canvas::DrawMinimap(Vec2 size) const
{
    DrawList out;
    out.size              = size;
    const auto [o, k]     = MinimapFrame(size);
    const Style& s        = style;
    auto         toMap    = [&](const Rect& r) { return Rect{(r.x - o.x) * k, (r.y - o.y) * k, std::max(1.5f, r.w * k), std::max(1.5f, r.h * k)}; };
    out.cmds.push_back(FillRect{{0, 0, size.x, size.y}, Color{s.background.r, s.background.g, s.background.b, 235}, s.nodeStroke, 1.f, 4.f});
    const Graph& shown = Shown();
    for (std::size_t i = 0; i < shown.nodes.size(); ++i)
        if (grouped_[i]) {
            const NodeType* t = schema_->FindNode(shown.nodes[i].type);
            // A kit's band (a palette slot, a fixed colour) is the group's colour here too.
            const Color c = kit_ ? LookOf(*LiveOf(shown.nodes[i]), StatesOf(shown.nodes[i])).band : t ? t->band : s.muted;
            out.cmds.push_back(FillRect{toMap(shown.nodes[i].frame), Color{c.r, c.g, c.b, 40}, Color{c.r, c.g, c.b, 160}, 1.f});
        }
    for (std::size_t i = 0; i < shown.nodes.size(); ++i) {
        const Node& n = shown.nodes[i];
        if (grouped_[i] || !Visible(n)) continue;
        Color c = s.muted;
        if (const auto ring = highlights.rings.find(n.id); ring != highlights.rings.end()) c = ring->second;
        if (selNodes_.contains(n.id)) c = s.selection;
        out.cmds.push_back(FillRect{toMap(n.frame), c});
    }
    const Vec2 a = camera.ToWorld(Vec2{0, 0}), b = camera.ToWorld(viewport_);
    out.cmds.push_back(FillRect{toMap(Rect::FromPoints(a, b)), Color{s.text.r, s.text.g, s.text.b, 20}, s.text, 1.f});
    return out;
}

Vec2 Canvas::MinimapToWorld(Vec2 at, Vec2 size) const
{
    const auto [o, k] = MinimapFrame(size);
    return o + at / k;
}

void Canvas::CenterOnWorld(Vec2 world) { camera.pan = viewport_ * 0.5f - world * camera.zoom; }

void Canvas::DrawArrow(std::vector<DrawCmd>& c, const Arrow& a, const Camera& cam) const
{
    auto shownAs = [&](const std::string& id) -> const Node* {
        if (const Node* n = NodeById(id)) return n;
        const auto as = shownAs_.find(id);
        return as == shownAs_.end() ? nullptr : NodeById(as->second);
    };
    const Node* from = shownAs(a.from);
    const Node* to   = shownAs(a.to);
    if (!from || !to) return;
    const float z = cam.zoom;
    const Rect  fa = cam.ToScreen(from->frame), fb = cam.ToScreen(to->frame);
    const Vec2  p0{fa.x, fa.y + fa.h * 0.5f}, p1{fb.x, fb.y + fb.h * 0.5f};
    std::vector<Vec2> pts;
    if (from == to) {
        // A loop out of its left side.
        const float r = 14.f * std::max(z, 0.4f);
        const Vec2  o{p0.x - r, p0.y};
        for (int k = 0; k <= 24; ++k) {
            const double t = 0.35 * 3.14159265358979 + (1.3 * 3.14159265358979) * k / 24.0;
            pts.push_back({o.x + static_cast<float>(std::cos(t)) * r, o.y - static_cast<float>(std::sin(t)) * r});
        }
    } else {
        // An arc bowing out to the left of both, from one's left side to the other's.
        const float bow  = 40.f * z + Distance(p0, p1) * 0.25f;
        const Vec2  ctrl{std::min(p0.x, p1.x) - bow, (p0.y + p1.y) * 0.5f};
        for (int k = 0; k <= 24; ++k) {
            const float t = static_cast<float>(k) / 24.f, u = 1.f - t;
            pts.push_back(p0 * (u * u) + ctrl * (2 * u * t) + p1 * (t * t));
        }
    }
    const Color col = style.glow;
    c.push_back(Polyline{pts, col, 2.f, true});
    // The head at the middle fills as the arrow's progress runs; a flash rings it.
    const Vec2  mid = pts[pts.size() / 2];
    Vec2        d   = pts[pts.size() / 2 + 1] - pts[pts.size() / 2 - 1];
    const float len = Length(d);
    if (len > 0.001f) d = d / len;
    const Vec2  perp{-d.y, d.x};
    const float h = std::clamp(8.f * z, 5.f, 10.f);
    std::vector<Vec2> head{mid + d * h, mid - d * (h * 0.6f) + perp * (h * 0.7f), mid - d * (h * 0.6f) - perp * (h * 0.7f)};
    c.push_back(Polygon{head, Color{col.r, col.g, col.b, static_cast<std::uint8_t>(60 + 195 * std::clamp(a.progress, 0.f, 1.f))}});
    head.push_back(head.front());
    c.push_back(Polyline{head, col, 1.5f});
    if (a.flash > 0.f)
        c.push_back(Circle{mid, h * 1.6f, Color{255, 255, 255, static_cast<std::uint8_t>(200 * std::clamp(a.flash, 0.f, 1.f))}, {}, 0.f});
}

DrawList Canvas::DrawAll(float zoom, float margin) const
{
    const Rect b = (Sync(), Shown().Bounds());
    Camera     cam;
    cam.zoom = zoom;
    cam.pan  = {margin - b.x * zoom, margin - b.y * zoom};
    return Render(cam, {b.w * zoom + 2 * margin, b.h * zoom + 2 * margin}, false);
}

DrawList Canvas::Render(const Camera& cam, Vec2 viewport, bool chrome) const
{
    DrawList out;
    out.size = viewport;
    auto& c  = out.cmds;
    const Style& s = style;
    const float  z = cam.zoom;

    c.push_back(FillRect{{0, 0, viewport.x, viewport.y}, s.background});

    // Grid: minor lines only while they are far enough apart to read as a grid.
    if (chrome) {
        const float step = s.gridSpacing * z;
        if (step >= 6.f) {
            const Vec2 w0 = cam.ToWorld(Vec2{0, 0});
            const int  i0 = static_cast<int>(std::floor(w0.x / s.gridSpacing));
            const int  j0 = static_cast<int>(std::floor(w0.y / s.gridSpacing));
            for (int i = i0; cam.ToScreen(Vec2{i * s.gridSpacing, 0}).x <= viewport.x; ++i) {
                const float x = cam.ToScreen(Vec2{i * s.gridSpacing, 0}).x;
                c.push_back(Polyline{{{x, 0}, {x, viewport.y}}, i % s.majorEvery == 0 ? s.gridMajor : s.grid, 1.f});
            }
            for (int j = j0; cam.ToScreen(Vec2{0, j * s.gridSpacing}).y <= viewport.y; ++j) {
                const float y = cam.ToScreen(Vec2{0, j * s.gridSpacing}).y;
                c.push_back(Polyline{{{0, y}, {viewport.x, y}}, j % s.majorEvery == 0 ? s.gridMajor : s.grid, 1.f});
            }
        }
    }

    Sync();
    // What the host lights, resolved to what is drawn: a hidden node's glow to its closed group, a merged wire's to the
    // wire drawn for it, the brightest winning.
    std::unordered_map<std::string_view, float> nodeGlow, wireGlow;
    for (const auto& [id, v] : highlights.glow) {
        const auto as = shownAs_.find(id);
        float&     g  = nodeGlow[as == shownAs_.end() ? std::string_view(id) : std::string_view(as->second)];
        g             = std::max(g, v);
    }
    for (const auto& [id, v] : highlights.edgeGlow) {
        const auto as = edgeShownAs_.find(id);
        float&     g  = wireGlow[as == edgeShownAs_.end() ? std::string_view(id) : std::string_view(as->second)];
        g             = std::max(g, v);
    }
    auto glowOf = [](const std::unordered_map<std::string_view, float>& m, std::string_view id) {
        const auto it = m.find(id);
        return it == m.end() ? 0.f : std::clamp(it->second, 0.f, 1.f);
    };
    auto drawGlow = [&](const Rect& r, float v) {
        if (v <= 0.f) return;
        const float spread = 3.f + 7.f * v;
        c.push_back(FillRect{r.Inflated(spread), Color{s.glow.r, s.glow.g, s.glow.b, static_cast<std::uint8_t>(50 + 130 * v)}, {}, 0.f,
                             s.corner * z + spread});
    };
    // Which ports have a wire: their pins draw filled.
    auto wired = [&](std::string_view node, std::string_view port) { return wiredPorts_.contains(PortKey(node, port)); };
    const bool  offering = chrome && mode_ == Mode::Connecting && started_;   // a wire is being dragged: ring where it may land
    const float pinR     = PinRadius(z);
    auto drawPins = [&](const Node& n, const Rect& frame, const Camera& cm) {
        const auto      slots = SlotsOf(n, frame);
        const NodeLook* table = slots.empty() ? nullptr : PinTableOf(n);
        for (const PortSlot& slot : slots) {
            const PortType* pt    = schema_->FindPortType(slot.def.typeFrom.empty() ? slot.def.type
                                                                                    : PortTypeOf(*graph_, *schema_, n, slot.def.id));
            const Color     color = pt ? pt->color : s.muted;
            const Vec2      at    = cm.ToScreen(slot.at);
            const Vec2      dir   = slot.def.dir == PortDir::Out ? slot.normal : slot.normal * -1.f;
            const bool      on    = wired(n.id, slot.def.id);
            if (const PinLook* pl = table ? PinLookIn(*table, n, slot.def, on) : nullptr)
                DrawPin(c, at, dir, pl->glyph.value_or(pt ? pt->glyph : PortGlyph::Circle), pl->size > 0.f ? pl->size * cm.zoom : pinR,
                        pl->color.value_or(color), pl->hollow.value_or(s.nodeFill), pl->fill == 1 || (pl->fill == 0 && on));
            else
                DrawPin(c, at, dir, pt ? pt->glyph : PortGlyph::Circle, pinR, color, s.nodeFill, on);
            if (!offering || n.id == active_) continue;
            const bool target = n.id == connectTo_ && slot.def.id == connectPort_;
            if (target || (!activePort_.empty() && CanWire(active_, activePort_, n.id, slot.def.id)))
                c.push_back(Circle{at, pinR + (target ? 4.f : 3.f), Color{0, 0, 0, 0}, s.selection, target ? 2.f : 1.25f});
        }
    };

    // Groups, under everything else: outer ones first, so a nested group sits on its parent.
    std::vector<const Node*> groups;
    for (std::size_t i = 0; i < Shown().nodes.size(); ++i)
        if (grouped_[i]) groups.push_back(&Shown().nodes[i]);
    std::stable_sort(groups.begin(), groups.end(), [](const Node* a, const Node* b) { return a->frame.Area() > b->frame.Area(); });
    for (const Node* g : groups) {
        const NodeType& t   = TypeOf(*g);
        const bool      sel = chrome && selNodes_.contains(g->id);
        const bool      hov = chrome && (g->id == hovered_ || g->id == connectTo_);
        const Rect      r   = cam.ToScreen(g->frame);
        if (!r.Intersects(Rect{0, 0, viewport.x, viewport.y})) continue;
        // Its look: the body's tint and outline, the title bar, the title and the detail line; closed, maybe a pill.
        const Node*     live   = LiveOf(*g);   // the host's own node: the view's copy keeps only its place
        const bool      closed = IsCollapsed(g->id);
        const NodeLook& look   = LookOf(*live, StatesOf(*live) | (sel ? StateSelected : 0u) | (hov ? StateHovered : 0u) |
                                                 (closed ? StateCollapsed : 0u));
        const bool      pill   = closed && look.shape == 1;
        const float     corner = pill ? r.h * 0.5f : look.corner * z;
        const LookPart* bar    = nullptr;
        for (const LookPart& part : look.parts)
            if (part.kind == PartKind::Header) bar = &part;
        drawGlow(r, glowOf(nodeGlow, g->id));   // a closed group glows with the brightest thing it holds
        c.push_back(FillRect{r, look.fill, look.stroke, look.strokeWidth, corner, look.dash});
        if (const auto ring = highlights.rings.find(g->id); ring != highlights.rings.end())
            c.push_back(FillRect{r.Inflated(3.f), Color{0, 0, 0, 0}, ring->second, 2.f, corner + 3.f});
        const float hh = std::min(s.groupHeader * z, r.h);
        if (bar) c.push_back(FillRect{{r.x + 1.f, r.y + 1.f, std::max(0.f, r.w - 2.f), std::max(0.f, hh - 1.f)}, bar->fill, {}, 0.f, corner});
        drawPins(*g, g->frame, cam);

        const float fs = s.fontSize * z;
        if (fs < 4.f || !bar) continue;
        const LookText* titleLine = nullptr;
        const LookText* detailLine = nullptr;
        for (const LookText& line : bar->texts) {
            if (line.id == "detail") detailLine = &line;
            else if (!titleLine) titleLine = &line;
        }
        const bool       folds = t.groupMode == GroupMode::Members && !t.fixed;
        const LineExtras extras{count_[static_cast<std::size_t>(g - Shown().nodes.data())], folds ? (closed ? "▸ " : "▾ ") : ""};
        std::string title, detail;
        bool        unnamed = false;
        if (titleLine) std::tie(title, unnamed) = LineText(*titleLine, *live, t, extras);
        if (detailLine) detail = LineText(*detailLine, *live, t, extras).first;
        const float pad = s.padding * z + (pill ? r.h * 0.25f : 0.f);
        const float w   = std::max(0.f, r.w - 2 * pad);
        const float ts  = titleLine ? titleLine->size * z * titleLine->scale : fs;
        const float tw  = detail.empty() ? w : std::min(w, ts * 0.62f * static_cast<float>(title.size()) + pad);
        if (titleLine)
            c.push_back(Text{{r.x + pad, r.y, tw, hh}, title, ts, unnamed ? titleLine->placeholderColor : titleLine->color, titleLine->bold,
                             false, false, static_cast<Align>(titleLine->align), titleLine->italic});
        if (!detail.empty() && w - tw > 20.f)
            c.push_back(Text{{r.x + pad + tw + pad * 0.5f, r.y, w - tw - pad * 0.5f, hh}, detail, detailLine->size * z * detailLine->scale,
                             detailLine->color, detailLine->bold, false, false, static_cast<Align>(detailLine->align), detailLine->italic});
    }

    // Edges, under the nodes; their labels go on top of everything, so a short edge between two cards still reads.
    std::vector<DrawCmd> labels, topWires;
    const Rect screenArea{-20.f, -20.f, viewport.x + 40.f, viewport.y + 40.f};
    for (std::size_t i = 0; i < Shown().edges.size(); ++i) {
        const Edge& e = Shown().edges[i];
        if (!paths_[i] || !cam.ToScreen(boxes_[i]).Intersects(screenArea)) continue;   // off screen
        if (mode_ == Mode::Connecting && !lifted_.empty() && Shown().edges[i].id == lifted_) continue;   // in the hand
        if (const Node* a = NodeById(Shown().edges[i].from), *b = NodeById(Shown().edges[i].to); (a && !Visible(*a)) || (b && !Visible(*b)))
            continue;   // an end shows only with what it belongs to
        const std::vector<Vec2>& wire = *paths_[i];
        const bool      sel   = chrome && e.id == selEdge_;
        const bool      near  = chrome && s.highlightSelectedWires && (selNodes_.contains(e.from) || selNodes_.contains(e.to));
        // Its look: its kind's, patched by the rules its tags and state match (selected, lit, merged, dimmed).
        const EdgeLook& look  = EdgeLookOf(e, (sel ? StateSelected : 0u) | (near ? StateLit : 0u) | (dashed_[i] ? StateMerged : 0u) |
                                                  (highlights.dimmed.contains(e.id) ? StateDimmed : 0u));
        Color color = look.color;
        if (look.pinColor && !e.fromPort.empty())
            if (const Node* from = NodeById(e.from)) {
                // Its pin kind's colour, as the source node shows it wired, when the kit gives one; else the type's.
                const std::string     type = PortTypeOf(*graph_, *schema_, *from, e.fromPort);
                std::optional<Color> pin;
                if (const NodeLook* table = PinTableOf(*from))
                    if (const std::string* kind = PinKindOf(type))
                        if (const auto it = table->pins.find(*kind); it != table->pins.end()) pin = it->second[1].color;
                if (!pin)
                    if (const PortType* pt = schema_->FindPortType(type)) pin = pt->color;
                if (pin) {
                    color = *pin;
                    if (look.opacity < 1.f) color.a = static_cast<std::uint8_t>(std::lround(color.a * std::clamp(look.opacity, 0.f, 1.f)));
                }
            }
        const auto      path  = ToScreen(cam, wire);
        std::vector<DrawCmd>& into = look.onTop ? topWires : c;   // a wire on top draws over the nodes
        if (const float v = glowOf(wireGlow, e.id); v > 0.f)
            into.push_back(Polyline{path, Color{s.glow.r, s.glow.g, s.glow.b, static_cast<std::uint8_t>(60 + 120 * v)},
                                    EdgeLookOf(e).width + 2.f + 6.f * v});
        into.push_back(Polyline{path, color, look.width, !look.dotted && look.dash, look.dotted});
        if (look.arrow != 0) {
            const float size  = look.arrowSize;
            const float arrow = std::clamp(size * z, size * 5.f / 9.f, size * 12.f / 9.f);
            if (look.arrow & 1) into.push_back(Arrowhead(path, arrow, color));
            if (look.arrow & 2) into.push_back(Arrowhead(std::vector<Vec2>(path.rbegin(), path.rend()), arrow, color));
        }
        if (!e.label.empty() && s.fontSize * z >= 6.f) {
            const Vec2  mid = Midpoint(path);
            const float fs  = s.fontSize * z * look.labelScale;
            const float w   = std::max(40.f, fs * 0.55f * static_cast<float>(e.label.size()) + 8.f);
            labels.push_back(FillRect{{mid.x - w * 0.5f, mid.y - fs * 0.8f, w, fs * 1.6f}, look.labelFill, {}, 0.f, 3.f});
            labels.push_back(Text{{mid.x - w * 0.5f, mid.y - fs * 0.8f, w, fs * 1.6f}, e.label, fs, look.labelColor, false, false, false,
                                  Align::Center});
        }
    }

    // A connection being drawn.
    if (chrome && mode_ == Mode::Connecting)
        if (const Node* from = Shown().FindNode(active_)) {
            const Node* to = connectTo_.empty() ? nullptr : Shown().FindNode(connectTo_);
            // Each end is its port's pin when it has one, else the side of the node facing the other end.
            auto pinOf = [&](const Node& n, const std::string& port) -> std::optional<Anchor> {
                if (port.empty()) return std::nullopt;
                const auto      slots = SlotsOf(n);
                const PortSlot* sl    = FindSlot(slots, port);
                return sl ? std::optional<Anchor>{Anchor{sl->at, sl->normal}} : std::nullopt;
            };
            const Vec2 cursor = cam.ToWorld(pointer_);
            const Rect toRect = to ? to->frame : Rect{cursor.x, cursor.y, 0, 0};
            const auto a      = pinOf(*from, activePort_).value_or(SideAnchor(from->frame, FacingSides(from->frame, toRect).first));
            Anchor     b{cursor, {0, 0}};
            if (to) b = pinOf(*to, connectPort_).value_or(SideAnchor(to->frame, FacingSides(from->frame, to->frame).second));
            c.push_back(Polyline{ToScreen(cam, Route(a, b, EdgeLine::Curve)), s.selection, 2.f, true});
        }

    auto drawHandles = [&](const Node& n, const Rect& r, bool sel) {
        const float hr = HandleRadius(s, z);
        if (wholeNodeWires && PortsOf(n, *schema_).empty()) c.push_back(Circle{ConnectHandle(r), hr, s.background, sel ? s.selection : s.muted, 1.5f});
        if (TypeOf(n).shape == NodeShape::Dot) return;   // a reroute dot is no size to resize
        const Rect g = ResizeGrip(r, s);
        c.push_back(Polygon{{{g.Right() - 2, g.y + 4}, {g.Right() - 2, g.Bottom() - 2}, {g.x + 4, g.Bottom() - 2}},
                            sel ? s.selection : s.muted});
    };

    // Nodes: unselected, then selected on top. With a view, a node draws with the host's live content at the view's place.
    std::function<void(const Node&, const Rect&)> drawNode;
    const Rect screenView{0, 0, viewport.x, viewport.y};
    auto drawLive = [&](const Node& shownNode) {
        if (!cam.ToScreen(shownNode.frame).Intersects(screenView)) return;   // off screen: no lookup either
        drawNode(*LiveOf(shownNode), shownNode.frame);                        // no copy: content and place apart
    };
    drawNode = [&](const Node& n, const Rect& frame) {
        const NodeType& t    = TypeOf(n);
        const bool      sel  = chrome && selNodes_.contains(n.id);
        const bool      hov  = chrome && (n.id == hovered_ || n.id == connectTo_);
        const bool      note = t.shape == NodeShape::Note;
        const Rect      r    = cam.ToScreen(frame);
        if (!r.Intersects(Rect{0, 0, viewport.x, viewport.y})) return;

        if (!Visible(n)) return;
        drawGlow(r, glowOf(nodeGlow, n.id));
        if (t.shape == NodeShape::Dial) {
            DrawDial(c, n, frame, t, cam, sel, hov);
        } else if (t.shape == NodeShape::Dot) {
            c.push_back(Circle{r.Center(), std::min(r.w, r.h) * 0.5f, t.band, sel ? s.selection : (hov ? s.text : t.band), sel ? 2.f : 1.f});
        } else if (t.shape == NodeShape::Pill) {
            // A capsule, and its look's parts along its line: glyphs, bars, texts, chips, stubs.
            const NodeLook& look = LookOf(n, (sel ? StateSelected : 0u) | (hov ? StateHovered : 0u) | StatesOf(n));
            c.push_back(FillRect{r, look.fill, look.stroke, look.strokeWidth, r.h * 0.5f, look.dash});
            DrawPillParts(c, PillLayout(n, t, look, r, z), look, r, z, s.fontSize * z >= 4.f);
        } else {
        // A card or a note, drawn from its look: the kit's, or the one its Schema and Style make.
        const NodeLook&    look = LookOf(n, (sel ? StateSelected : 0u) | (hov ? StateHovered : 0u) | StatesOf(n));
        const CardMetrics& km   = look.metrics;
        const float        rad  = look.corner * z;
        const LookPart*    head = nullptr;
        bool               filled = false;   // a part paints over the card: its outline goes on top
        for (const LookPart& part : look.parts) {
            if (part.kind == PartKind::Header) head = &part;
            filled = filled || ((part.kind == PartKind::Header || part.kind == PartKind::Body) && part.fill.a > 0);
        }
        c.push_back(FillRect{r, look.fill, filled ? Color{0, 0, 0, 0} : look.stroke, filled ? 0.f : look.strokeWidth, rad, look.dash});
        float inset = 0.f;   // an accent strip moves the text right
        for (const LookPart& part : look.parts) {
            if (part.kind == PartKind::Accent) {
                c.push_back(FillRect{{r.x + 1.f, r.y + rad, std::max(2.f, part.width * z), std::max(0.f, r.h - 2 * rad)}, part.fill});
                inset += part.width;
            } else if (part.kind == PartKind::Header && part.fill.a > 0) {
                const float hh = std::min(r.h, (part.height > 0.f ? part.height : km.Header()) * z);
                c.push_back(FillRect{{r.x, r.y, r.w, hh}, part.fill, {}, 0.f, rad});
                if (hh > rad) c.push_back(FillRect{{r.x, r.y + rad, r.w, hh - rad}, part.fill});
            } else if (part.kind == PartKind::Body && part.fill.a > 0 && head) {
                const float top = std::min(r.h, (head->height > 0.f ? head->height : km.Header()) * z);
                c.push_back(FillRect{{r.x, r.y + top, r.w, r.h - top}, part.fill, {}, 0.f, rad});
                c.push_back(FillRect{{r.x, r.y + top, r.w, std::min(rad, r.h - top)}, part.fill});
            }
        }
        if (filled) c.push_back(FillRect{r, Color{0, 0, 0, 0}, look.stroke, look.strokeWidth, rad, look.dash});

        const float pad = km.padding * z;
        const float x   = r.x + pad + inset * z;
        const float w   = r.Right() - pad - x;
        float       y   = r.y + pad * 0.8f;
        const float fs  = km.fontSize * z;

        if (fs >= 4.f && w > 4.f) {
            if (note) {
                const std::string& body = n.body;
                c.push_back(Text{{x, y, w, r.Bottom() - pad - y}, body.empty() ? std::string{"Double-click to write"} : body,
                                 fs, body.empty() ? s.muted : s.text, false, true, true});
            } else {
                // The header's lines: the title (a drillable card leaves room for its mark), then what the look adds.
                if (head) {
                    y                 = r.y + pad * head->top;
                    const float iconW = t.drillable ? 18.f * z : 0.f;
                    for (std::size_t k = 0; k < head->texts.size(); ++k) {
                        const LookText& line          = head->texts[k];
                        const auto [text, empty]      = LineText(line, n, t);
                        const float ts = line.size * z;
                        c.push_back(Text{{x, y, w - (k == 0 ? iconW : 0.f), ts * line.box}, text, ts * line.scale,
                                         empty ? line.placeholderColor : line.color, line.bold, false, false, static_cast<Align>(line.align),
                                         line.italic});
                        y += ts * line.advance;
                    }
                    if (head->height > 0.f) y = r.y + head->height * z;
                }
                if (km.header > 0.f) y = r.y + km.header * z;

                // Port rows: a left port's label after its pin, a right port's before it; a row holding both splits.
                const auto      slots    = SlotsOf(n, frame);
                const NodeLook* pinTable = slots.empty() ? nullptr : PinTableOf(n);
                std::vector<std::string_view> shown;   // fields a port's row already shows
                std::map<float, int> rowSides;          // row top -> 1 left, 2 right
                for (const PortSlot& sl : slots)
                    if (sl.row.h > 0.f) rowSides[sl.row.y] |= sl.side == PortSide::Left ? 1 : 2;
                for (const PortSlot& sl : slots) {
                    if (!sl.def.field.empty()) shown.push_back(sl.def.field);
                    if (sl.row.h <= 0.f || RowOfPort(n, sl.def.id) >= 0) continue;   // a Row draws its own line
                    const Rect  row  = cam.ToScreen(sl.row);
                    const bool  both = rowSides[sl.row.y] == 3;
                    const bool  left = sl.side == PortSide::Left;
                    const float half = std::max(0.f, both ? w * 0.5f : w);
                    const Rect  box{left ? x : r.Right() - pad - half, row.y, half, row.h};
                    std::string text = sl.def.label;
                    if (const PinLook* pl = pinTable ? PinLookIn(*pinTable, n, sl.def, wired(n.id, sl.def.id)) : nullptr; pl && !pl->label)
                        text.clear();
                    if (const std::string& v = n.Field(sl.def.field); !sl.def.field.empty() && !v.empty() &&
                                                                   !wired(n.id, sl.def.id))
                        text = text.empty() ? v : text + "  " + v;   // a literal or default shows while nothing is wired
                    if (!text.empty())
                        c.push_back(Text{box, text, fs, s.text, false, false, false, left ? Align::Left : Align::Right});
                }
                y += static_cast<float>(PortRows(n, *schema_)) * km.Row() * z;

                // The picture sits at the foot of the card; text above stops short of it.
                const bool  pictured = t.imageHeight > 0.f && !n.image.empty();
                const float foot     = r.Bottom() - pad * 0.5f - (pictured ? (t.imageHeight + s.padding * 0.5f) * z : 0.f);
                if (pictured)
                    c.push_back(Image{{x, foot + s.padding * 0.25f * z, w, t.imageHeight * z}, n.image, s.corner * z * 0.5f});

                // The node's own rows: a label on the left, a value (or a button, or a choice) on the right.
                const float rowH = km.Row() * z;
                for (const Row& row : n.rows) {
                    if (y + rowH > foot + 0.5f) break;
                    if (row.separator)
                        c.push_back(Polyline{{{x, y}, {r.Right() - pad, y}}, s.nodeStroke, 1.f});
                    // Its kind's look: the kit's row kind, or the one its RowStyle makes.
                    static const RowLook kPlainRow{};
                    const auto      kind  = look.rows.find(row.style);
                    const RowLook&  rl    = kind == look.rows.end() ? kPlainRow : kind->second;
                    const Color     label = rl.color.value_or(s.text);
                    const Color     value = rl.valueColor.value_or(s.muted);
                    const bool      bold  = rl.bold, italic = rl.italic;
                    std::string     right = row.value;
                    if (row.action == RowAction::Choice) right += right.empty() ? "\u25be" : " \u25be";
                    if (!rl.suffix.empty()) right += rl.suffix;
                    const float rw = right.empty() ? 0.f : std::min(w * 0.5f, fs * 0.6f * static_cast<float>(right.size()) + pad);
                    const std::string text = rl.prefix + row.label;
                    if (rl.chip) {
                        const float cw = std::min(w - rw, fs * 0.6f * static_cast<float>(text.size()) + pad);
                        c.push_back(FillRect{{x - pad * 0.3f, y + rowH * 0.1f, cw, rowH * 0.8f}, Color{label.r, label.g, label.b, 40}, {}, 0.f, rowH * 0.4f});
                    }
                    c.push_back(Text{{x, y, w - rw, rowH}, text, fs, label, bold, false, false, static_cast<Align>(rl.align), italic});
                    if (row.action == RowAction::Button) {
                        const Rect b{r.Right() - pad - rw, y + rowH * 0.1f, rw, rowH * 0.8f};
                        c.push_back(FillRect{b, s.background, s.muted, 1.f, rowH * 0.3f});
                        c.push_back(Text{b, right, fs, s.text, false, false, false, Align::Center});
                    } else if (!right.empty()) {
                        c.push_back(Text{{r.Right() - pad - rw, y, rw, rowH}, right, fs, value, false, false, false, static_cast<Align>(rl.valueAlign), italic});
                    }
                    y += rowH;
                }

                if (z >= s.detailZoom) {
                    const float line = fs * 1.4f;
                    for (const FieldDef& f : t.fields) {
                        if (!f.onCard || f.id == t.titleField) continue;
                        if (std::find(shown.begin(), shown.end(), f.id) != shown.end()) continue;
                        const std::string& v = n.Field(f.id);
                        if (v.empty()) continue;
                        if (y + line > foot) break;
                        const float lw = std::min(w * 0.38f, 90.f * z);
                        c.push_back(Text{{x, y, lw, line}, f.label, fs, s.muted});
                        c.push_back(Text{{x + lw, y, w - lw, line}, v, fs, s.text});
                        y += line;
                    }
                    if (!n.body.empty() && y + line <= foot) {
                        y += fs * 0.4f;
                        c.push_back(Text{{x, y, w, r.Bottom() - pad * 0.6f - y}, n.body, fs, s.muted, false, true});
                    }
                }

                // A drillable node's "layers" mark: filled once it holds a graph.
                if (t.drillable) {
                    const float u  = 5.f * z;
                    const float ix = r.Right() - pad - 2.4f * u;
                    const float iy = r.y + pad * 0.8f + 0.2f * u;
                    const Color on = n.link.empty() ? s.background : look.band;
                    c.push_back(FillRect{{ix + 0.6f * u, iy, 1.8f * u, 1.4f * u}, on, s.muted, 1.f, 1.f});
                    c.push_back(FillRect{{ix, iy + 0.6f * u, 1.8f * u, 1.4f * u}, on, s.muted, 1.f, 1.f});
                }
            }
        }
        }   // card / note

        if (const auto ring = highlights.rings.find(n.id); ring != highlights.rings.end())
            c.push_back(FillRect{r.Inflated(3.f), Color{0, 0, 0, 0}, ring->second, 2.f, s.corner * z + 3.f});
        if (highlights.dimmed.contains(n.id))
            c.push_back(FillRect{r, Color{s.background.r, s.background.g, s.background.b, 150}, {}, 0.f, s.corner * z});
        // An alias draws dimmed, with a dashed outline.
        if (!n.aliasOf.empty())
            c.push_back(FillRect{r, Color{s.background.r, s.background.g, s.background.b, 120}, s.muted, 1.f, s.corner * z, true});
        drawPins(n, frame, cam);
        if (sel || hov) drawHandles(n, r, sel);
    };
    for (std::size_t i = 0; i < Shown().nodes.size(); ++i)
        if (!grouped_[i] && (!chrome || !selNodes_.contains(Shown().nodes[i].id))) drawLive(Shown().nodes[i]);
    if (chrome)
        for (const std::string& id : selNodes_)
            if (const auto it = index_.find(id); it != index_.end() && !grouped_[it->second]) drawLive(Shown().nodes[it->second]);
    // A group's handles go over what it holds, or its grip could hide under a member.
    if (chrome)
        for (const Node* g : groups) {
            const bool sel = selNodes_.contains(g->id);
            if (sel || g->id == hovered_ || g->id == connectTo_) drawHandles(*g, cam.ToScreen(g->frame), sel);
        }

    c.insert(c.end(), topWires.begin(), topWires.end());
    for (const Arrow& a : highlights.arrows) DrawArrow(c, a, cam);

    // A row being reordered: a line where it would go.
    if (chrome && mode_ == Mode::RowReorder && started_ && reorderTo_ >= 0)
        if (const Node* n = ShownNode(active_)) {
            int from = -1;
            for (std::size_t k = 0; k < n->rows.size(); ++k)
                if (n->rows[k].id == pressRow_) from = static_cast<int>(k);
            if (from >= 0 && reorderTo_ != from) {
                const Rect  line = cam.ToScreen(RowRect(*n, *schema_, MetricsFor(*n), static_cast<std::size_t>(reorderTo_)));
                const float y    = reorderTo_ < from ? line.y : line.Bottom();
                c.push_back(Polyline{{{line.x + 2.f, y}, {line.Right() - 2.f, y}}, s.selection, 2.f});
            }
        }
    if (chrome && docking && mode_ == Mode::Moving && started_ && origin_.size() == 1)
        if (const Node* moving = graph_->FindNode(origin_.front().first))
            if (const auto slot = FindDockSlot(moving->id, moving->frame)) {
                const Color wash{s.selection.r, s.selection.g, s.selection.b, 40};
                c.push_back(FillRect{cam.ToScreen(slot->at), wash, s.selection, 1.f, s.corner * z, true});
            }
    if (chrome && mode_ == Mode::Idle && !hovered_.empty())
        if (const Node* h = NodeById(hovered_); h && !h->dock.parent.empty()) {
            // A docked card hovered: the row it hangs on washes, and what its wires enter is ringed.
            const Color wash{s.selection.r, s.selection.g, s.selection.b, 40};
            if (!h->dock.row.empty())
                if (const auto line = RowScreenRect(h->dock.parent, h->dock.row)) c.push_back(FillRect{*line, wash});
            for (const Edge& e : Shown().edges)
                if (e.from == h->id)
                    if (const Node* to = NodeById(e.to))
                        c.push_back(FillRect{cam.ToScreen(to->frame).Inflated(3.f), Color{0, 0, 0, 0}, s.selection, 1.5f, s.corner * z});
        }
    c.insert(c.end(), labels.begin(), labels.end());
    // Zoomed far out, text is noise: only the shapes draw.
    if (z < s.textZoom) std::erase_if(c, [](const DrawCmd& d) { return std::holds_alternative<Text>(d); });

    if (chrome && mode_ == Mode::Marquee && started_)
        c.push_back(FillRect{Rect::FromPoints(downAt_, pointer_), s.marquee, s.selection, 1.f});

    return out;
}

} // namespace grapher
