#include <grapher/Canvas.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
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

} // namespace

// ── Setup ────────────────────────────────────────────────────────────────────

Canvas::Canvas(Graph& graph, const Schema& schema) : graph_(&graph), host_(&graph), schema_(&schema) {}

void Canvas::SetGraph(Graph& graph)
{
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

std::vector<PortSlot> Canvas::SlotsOf(const Node& n) const { return LayoutPorts(n, *schema_, MetricsOf(style)); }

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

void Canvas::Refresh()
{
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
    for (const Node& n : graph_->nodes)
        if (const auto it = before.find(n.id); it != before.end() && !(*it->second == n.frame)) out.frames.emplace_back(n.id, n.frame);
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
    const float h = PortRows(n, *schema_) > 0 ? std::max(t->height, CardHeight(n, *schema_, MetricsOf(style))) : t->height;
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
    if (a == b) return false;
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
        if (e.from == w.from && e.to == w.to && e.fromPort == w.fromPort && e.toPort == w.toPort) return false;
    return true;
}

std::string Canvas::Connect(std::string_view a, std::string_view pa, std::string_view b, std::string_view pb)
{
    if (!CanWire(a, pa, b, pb)) return {};
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

    Before("Connect");
    // A port at its limit gives up its oldest wire to the new one.
    std::vector<std::string> replaced;
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
    Changed(Intent{"Connect", ConnectWire{e, std::move(replaced)}});
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
    const Node* n = graph_->FindNode(nodeId);
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
    for (const Node& n : graph_->nodes)
        if (ids.empty() || ids.contains(n.id)) b = b ? Rect::Union(*b, n.frame) : n.frame;
    if (!b) {
        if (ids.empty()) camera = {{viewport_.x * 0.5f, viewport_.y * 0.5f}, 1.f};
        return;
    }
    const float zx = (viewport_.x - 2 * margin) / std::max(b->w, 1.f);
    const float zy = (viewport_.y - 2 * margin) / std::max(b->h, 1.f);
    camera.zoom    = std::clamp(std::min({zx, zy, zoomCap}), minZoom, maxZoom);
    const Vec2 c   = b->Center();
    camera.pan     = {viewport_.x * 0.5f - c.x * camera.zoom, viewport_.y * 0.5f - c.y * camera.zoom};
}

void Canvas::CenterOn(std::string_view nodeId, std::optional<float> zoom)
{
    const Node* n = graph_->FindNode(nodeId);
    if (!n) return;
    if (zoom) camera.zoom = std::clamp(*zoom, minZoom, maxZoom);
    const Vec2 c = n->frame.Center();
    camera.pan   = {viewport_.x * 0.5f - c.x * camera.zoom, viewport_.y * 0.5f - c.y * camera.zoom};
}

void Canvas::EnsureVisible(std::string_view nodeId, float margin)
{
    const Node* n = graph_->FindNode(nodeId);
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
    const float after  = std::clamp(before * factor, minZoom, maxZoom);
    if (after == before) return false;
    const Vec2 world = camera.ToWorld(anchor);
    camera.zoom      = after;
    camera.pan       = anchor - world * after;
    return true;
}

bool Canvas::Wheel(Vec2 at, float steps) { return ZoomAbout(std::pow(1.15f, steps), at); }

std::optional<Vec2> Canvas::HeaderScreenPos(std::string_view nodeId) const
{
    const Node* n = graph_->FindNode(nodeId);
    if (!n) return std::nullopt;
    const CardMetrics m = MetricsOf(style);
    const float       y = IsGroup(*n, *schema_) ? std::min(n->frame.h, m.groupHeader) * 0.5f : m.Top() + m.titleSize * 0.65f;
    return camera.ToScreen(Vec2{n->frame.Center().x, n->frame.y + std::min(y, n->frame.h)});
}

std::optional<Rect> Canvas::RowScreenRect(std::string_view nodeId, std::string_view row) const
{
    const Node* n = graph_->FindNode(nodeId);
    if (!n) return std::nullopt;
    for (std::size_t k = 0; k < n->rows.size(); ++k)
        if (n->rows[k].id == row) return camera.ToScreen(RowRect(*n, *schema_, MetricsOf(style), k));
    return std::nullopt;
}

std::string Canvas::TooltipAt(Vec2 screen) const
{
    const Hit hit = HitTest(screen);
    switch (hit.kind) {
    case HitKind::Row:
        if (const Node* n = graph_->FindNode(hit.id)) {
            const Row* r = n->FindRow(hit.row);
            return r && !r->tooltip.empty() ? r->tooltip : n->tooltip;
        }
        return {};
    case HitKind::Port:
        if (const Node* n = graph_->FindNode(hit.id))
            for (const PortDef& p : PortsOf(*n, *schema_))
                if (p.id == hit.port) {
                    const std::string type = PortTypeOf(*graph_, *schema_, *n, p.id);
                    const PortType*   pt   = schema_->FindPortType(type);
                    const std::string name = p.label.empty() ? p.id : p.label;
                    return type.empty() ? name : name + " (" + (pt ? pt->label : type) + ")";
                }
        return {};
    case HitKind::Node:
    case HitKind::Resize:
        if (const Node* n = graph_->FindNode(hit.id)) return n->tooltip;
        return {};
    case HitKind::Edge:
        if (const Edge* e = graph_->FindEdge(hit.id)) return e->label;
        return {};
    default: return {};
    }
}

Rect Canvas::ScreenRectOf(std::string_view nodeId) const
{
    const Node* n = graph_->FindNode(nodeId);
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
    for (std::size_t i = graph_->nodes.size(); i-- > 0;) {
        const Node& n = graph_->nodes[i];
        if (!camera.ToScreen(n.frame).Inflated(reach).Contains(screen)) continue;
        if (grouped_[i]) groups.push_back(&n);
        else order.push_back(&n);
    }
    std::stable_partition(order.begin(), order.end(), [&](const Node* n) { return selNodes_.contains(n->id); });
    std::stable_sort(groups.begin(), groups.end(), [](const Node* a, const Node* b) { return a->frame.Area() < b->frame.Area(); });

    // A port's pin, or a port-less node's connect handle, sits on the outline; both reach a little past it.
    auto onOutline = [&](const Node& n, const Rect& r) -> Hit {
        const auto slots = SlotsOf(n);
        for (const PortSlot& s : slots)
            if (Distance(screen, camera.ToScreen(s.at)) <= pin) return {HitKind::Port, n.id, s.def.id};
        if (slots.empty() && Distance(screen, ConnectHandle(r)) <= handle) return {HitKind::Connect, n.id};
        return {};
    };
    for (const Node* n : order) {
        const Rect r = camera.ToScreen(n->frame);
        if (!r.Inflated(reach).Contains(screen)) continue;
        if (Hit h = onOutline(*n, r); h.kind != HitKind::None) return h;
        if (r.Contains(screen)) {
            if (ResizeGrip(r, style).Contains(screen)) return {HitKind::Resize, n->id};
            if (!n->rows.empty() && TypeOf(*n).shape == NodeShape::Card) {
                const CardMetrics m   = MetricsOf(style);
                const float       top = RowsTop(*n, *schema_, m);
                const float       wy  = camera.ToWorld(screen).y;
                if (wy >= top) {
                    const auto k = static_cast<std::size_t>((wy - top) / m.Row());
                    if (k < n->rows.size()) return {HitKind::Row, n->id, {}, n->rows[k].id};
                }
            }
            return {HitKind::Node, n->id};
        }
    }
    for (std::size_t i = graph_->edges.size(); i-- > 0;) {
        if (!paths_[i] || !camera.ToScreen(boxes_[i]).Inflated(kEdgeHitPx).Contains(screen)) continue;
        if (PolylineDistance(screen, ToScreen(camera, *paths_[i])) <= kEdgeHitPx) return {HitKind::Edge, graph_->edges[i].id};
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
        const bool  group  = IsGroup(*n, *schema_);
        const Vec2  centre = n->frame.Center();
        const Node* target = nullptr;
        for (const Node& g : graph_->nodes) {
            if (g.id == id || moving.contains(g.id) || !IsGroup(g, *schema_) || !g.frame.Contains(centre)) continue;
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

const Node* Canvas::NodeById(std::string_view id) const
{
    if (synced_ == graph_ && index_.size() == graph_->nodes.size())
        if (const auto it = index_.find(id); it != index_.end() && it->second < graph_->nodes.size() && graph_->nodes[it->second].id == id)
            return &graph_->nodes[it->second];
    return graph_->FindNode(id);
}

void Canvas::SyncForHit() const
{
    if (!wiresValid_ || synced_ != graph_ || syncedEdits_ != edits_ || grouped_.size() != graph_->nodes.size() ||
        paths_.size() != graph_->edges.size())
        Sync();
}

void Canvas::Sync() const
{
    const bool     dragging = (mode_ == Mode::Moving || mode_ == Mode::Resizing) && started_;
    // With a backend, a slow live reroute leaves the settled wires alone and patches only the dragged nodes' wires.
    const bool     patch = router && dragging && backendMs_ > kLiveRouteMs;
    // A large graph without one does the same with the built-in router: only the dragged nodes' wires move.
    const bool     local = !router && dragging && graph_->nodes.size() > kLargeGraph && wiresValid_;
    const bool     plain = (!router || patch) && dragging && routeMs_ > kLiveRouteMs && !local;
    RoutingOptions o     = routing;
    if (plain) o.avoidNodes = false;
    o.card = MetricsOf(style);

    std::size_t optionsKey = Mix(reinterpret_cast<std::uintptr_t>(router.get()),
                                 (o.avoidNodes ? 1u : 0u) | (o.spreadPorts ? 2u : 0u));
    for (float f : {o.clearance, o.bendCost, o.shareCost, o.crossCost, o.laneSpacing, o.cornerRadius, o.curveRadius,
                    o.card.padding, o.card.titleSize, o.card.fontSize, o.card.groupHeader})
        optionsKey = Mix(optionsKey, Bits(f));

    const auto  hs  = HashText;
    std::size_t key = Mix(reinterpret_cast<std::uintptr_t>(schema_), (plain ? 1u : 0u) | (patch ? 2u : 0u) | (local ? 4u : 0u));
    key = Mix(key, reinterpret_cast<std::uintptr_t>(graph_));
    key = Mix(Mix(key, optionsKey), static_cast<std::size_t>(schema_->revision));
    for (const Node& n : graph_->nodes) {
        key = Mix(Mix(key, hs(n.id)), hs(n.type));
        for (float f : {n.frame.x, n.frame.y, n.frame.w, n.frame.h}) key = Mix(key, Bits(f));
        for (const PortDef& p : n.ports)
            key = Mix(Mix(key, hs(p.id)), static_cast<std::size_t>(p.side) * 8 + static_cast<std::size_t>(p.dir) * 2 + (p.onHeader ? 1 : 0));
    }
    for (const Edge& e : graph_->edges) {
        for (const std::string* s : {&e.id, &e.from, &e.to, &e.type, &e.fromPort, &e.toPort}) key = Mix(key, hs(*s));
        const EdgeType* t = schema_->FindEdge(e.type);
        key = Mix(key, t ? static_cast<std::size_t>(t->line) + 1 : 0);
    }
    if (wiresValid_ && key == wiresKey_) {
        synced_      = graph_;
        syncedEdits_ = edits_;
        return;
    }

    const auto start   = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
    if (router && !patch) {
        // A backend's first route with a router or options builds everything; only the reroutes after it time a drag step.
        const bool incremental = backendKey_ == optionsKey && !routed_.empty();
        routed_     = router->Route(*graph_, *schema_, o);
        backendMs_  = incremental ? elapsed() : -1.0;
        backendKey_ = optionsKey;
        wires_      = routed_;
    } else if (patch) {
        std::unordered_set<std::string_view> moving;
        for (const auto& [id, frame] : origin_) moving.insert(id);
        const WirePaths quick = RouteWires(*graph_, *schema_, o);
        if (!plain) routeMs_ = elapsed();
        wires_.clear();
        for (const Edge& e : graph_->edges) {
            const bool touched = moving.contains(e.from) || moving.contains(e.to);
            const auto settled = routed_.find(e.id);
            const auto live    = quick.find(e.id);
            if (!touched && settled != routed_.end()) wires_.emplace(e.id, settled->second);
            else if (live != quick.end()) wires_.emplace(e.id, live->second);
        }
    } else if (local) {
        // Only the wires of the dragged nodes reroute, among the nodes they join; the rest keep their settled paths.
        std::unordered_set<std::string_view> moving, joined;
        for (const auto& [id, frame] : origin_) moving.insert(id);
        Graph sub;
        for (const Edge& e : graph_->edges)
            if (moving.contains(e.from) || moving.contains(e.to)) {
                sub.edges.push_back(e);
                joined.insert(e.from);
                joined.insert(e.to);
            }
        for (const Node& n : graph_->nodes)
            if (joined.contains(n.id)) sub.nodes.push_back(n);
        for (auto& [id, path] : RouteWires(sub, *schema_, o)) wires_[id] = std::move(path);
    } else {
        wires_ = RouteWires(*graph_, *schema_, o);
        if (!plain) routeMs_ = elapsed();
    }
    wiresKey_   = key;
    wiresValid_ = true;
    synced_      = graph_;
    syncedEdits_ = edits_;

    // Per node and per edge, so a frame needs no lookups by id.
    std::unordered_map<std::string_view, const NodeType*> types;
    types.reserve(schema_->nodeTypes.size());
    for (const NodeType& t : schema_->nodeTypes) types.emplace(t.id, &t);
    const std::size_t nodes = graph_->nodes.size();
    grouped_.assign(nodes, 0);
    index_.clear();
    index_.reserve(nodes);
    for (std::size_t i = 0; i < nodes; ++i) {
        const Node& n = graph_->nodes[i];
        const auto  t = types.find(n.type);
        grouped_[i]   = t != types.end() && t->second->shape == NodeShape::Group;
        index_.emplace(n.id, i);
    }
    paths_.assign(graph_->edges.size(), nullptr);
    boxes_.assign(graph_->edges.size(), Rect{});
    wiredPorts_.clear();
    for (std::size_t i = 0; i < graph_->edges.size(); ++i) {
        const Edge& e = graph_->edges[i];
        if (!e.fromPort.empty()) wiredPorts_.insert(PortKey(e.from, e.fromPort));
        if (!e.toPort.empty()) wiredPorts_.insert(PortKey(e.to, e.toPort));
        const auto w = wires_.find(e.id);
        if (w == wires_.end() || w->second.size() < 2) continue;
        paths_[i] = &w->second;
        Vec2 lo = w->second.front(), hi = lo;
        for (const Vec2& p : w->second) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
        }
        boxes_[i] = Rect::FromPoints(lo, hi);
    }
}

// ── Gestures ─────────────────────────────────────────────────────────────────

bool Canvas::PointerDown(Vec2 at, Button button, Modifiers mods)
{
    downAt_ = pointer_ = at;
    started_ = false;
    origin_.clear();

    if (button != Button::Left) {
        mode_      = panWithButtons ? Mode::Panning : Mode::Idle;
        panAtDown_ = camera.pan;
        return false;
    }

    const Hit hit = HitTest(at);
    switch (hit.kind) {
    case HitKind::Connect:
    case HitKind::Port:
        mode_       = Mode::Connecting;
        active_     = hit.id;
        activePort_ = hit.port;
        connectTo_.clear();
        connectPort_.clear();
        return true;
    case HitKind::Resize:
        if (!selNodes_.contains(hit.id)) SelectNodes({hit.id});
        mode_   = Mode::Resizing;
        active_ = hit.id;
        origin_.emplace_back(hit.id, graph_->FindNode(hit.id)->frame);
        return true;
    case HitKind::Row:
    case HitKind::Node: {
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
        if (selNodes_.contains(n.id) || carried_.contains(n.id)) origin_.emplace_back(n.id, n.frame);
}

bool Canvas::PointerMove(Vec2 at)
{
    pointer_ = at;
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
    case Mode::Panning:
        camera.pan = panAtDown_ + (at - downAt_);
        return true;
    case Mode::Marquee:
    default:
        return true;
    }
}

bool Canvas::PointerUp(Vec2 at, Button)
{
    PointerMove(at);
    const Mode mode = mode_;
    mode_           = Mode::Idle;

    switch (mode) {
    case Mode::RowPress:
        if (const Node* n = graph_->FindNode(active_))
            if (const Row* r = n->FindRow(pressRow_)) rowClick_ = RowClick{active_, pressRow_, r->action, at};
        return true;
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
            Changed(Intent{"Move", MovedFrames()});
        }
        carried_.clear();
        return true;
    case Mode::Resizing:
        if (started_) Changed(Intent{"Resize", MovedFrames()});
        return true;
    case Mode::Connecting: {
        const std::string to     = std::exchange(connectTo_, {});
        const std::string toPort = std::exchange(connectPort_, {});
        if (!to.empty()) Connect(active_, activePort_, to, toPort);   // one edge per pair of ends (CanWire)
        else if (started_ && onIntent && HitTest(at).kind == HitKind::None)
            onIntent(Intent{"Connect", ConnectToNothing{active_, activePort_, camera.ToWorld(at)}});
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

DrawList Canvas::DrawAll(float zoom, float margin) const
{
    const Rect b = graph_->Bounds();
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
    // Which ports have a wire: their pins draw filled.
    auto wired = [&](std::string_view node, std::string_view port) { return wiredPorts_.contains(PortKey(node, port)); };
    const bool  offering = chrome && mode_ == Mode::Connecting && started_;   // a wire is being dragged: ring where it may land
    const float pinR     = PinRadius(z);
    auto drawPins = [&](const Node& n, const Camera& cm) {
        for (const PortSlot& slot : SlotsOf(n)) {
            const PortType* pt    = schema_->FindPortType(slot.def.typeFrom.empty() ? slot.def.type
                                                                                    : PortTypeOf(*graph_, *schema_, n, slot.def.id));
            const Color     color = pt ? pt->color : s.muted;
            const Vec2      at    = cm.ToScreen(slot.at);
            const Vec2      dir   = slot.def.dir == PortDir::Out ? slot.normal : slot.normal * -1.f;
            DrawPin(c, at, dir, pt ? pt->glyph : PortGlyph::Circle, pinR, color, s.nodeFill, wired(n.id, slot.def.id));
            if (!offering || n.id == active_) continue;
            const bool target = n.id == connectTo_ && slot.def.id == connectPort_;
            if (target || (!activePort_.empty() && CanWire(active_, activePort_, n.id, slot.def.id)))
                c.push_back(Circle{at, pinR + (target ? 4.f : 3.f), Color{0, 0, 0, 0}, s.selection, target ? 2.f : 1.25f});
        }
    };

    // Groups, under everything else: outer ones first, so a nested group sits on its parent.
    std::vector<const Node*> groups;
    for (std::size_t i = 0; i < graph_->nodes.size(); ++i)
        if (grouped_[i]) groups.push_back(&graph_->nodes[i]);
    std::stable_sort(groups.begin(), groups.end(), [](const Node* a, const Node* b) { return a->frame.Area() > b->frame.Area(); });
    for (const Node* g : groups) {
        const NodeType& t   = TypeOf(*g);
        const bool      sel = chrome && selNodes_.contains(g->id);
        const bool      hov = chrome && (g->id == hovered_ || g->id == connectTo_);
        const Rect      r   = cam.ToScreen(g->frame);
        if (!r.Intersects(Rect{0, 0, viewport.x, viewport.y})) continue;
        Color fill = t.band, head = t.band;
        fill.a     = s.groupFill;
        head.a     = static_cast<std::uint8_t>(std::min(255, s.groupFill * 2));
        c.push_back(FillRect{r, fill, sel ? s.selection : (hov ? s.text : t.band), sel ? 2.f : 1.25f, s.corner * z});
        const float hh = std::min(s.groupHeader * z, r.h);
        c.push_back(FillRect{{r.x + 1.f, r.y + 1.f, std::max(0.f, r.w - 2.f), std::max(0.f, hh - 1.f)}, head, {}, 0.f, s.corner * z});
        drawPins(*g, cam);

        const float fs = s.fontSize * z;
        if (fs < 4.f) continue;
        const std::string& named = t.titleField.empty() ? std::string{} : g->Field(t.titleField);
        const std::string  title = named.empty() ? "Untitled " + t.label : named;
        std::string        detail;
        for (const FieldDef& f : t.fields) {
            const std::string& v = g->Field(f.id);
            if (!f.onCard || f.id == t.titleField || v.empty()) continue;
            if (!detail.empty()) detail += " · ";
            detail += v;
        }
        const float pad = s.padding * z;
        const float w   = std::max(0.f, r.w - 2 * pad);
        const float ts  = fs * 1.1f;
        const float tw  = detail.empty() ? w : std::min(w, ts * 0.62f * static_cast<float>(title.size()) + pad);
        c.push_back(Text{{r.x + pad, r.y, tw, hh}, title, ts, named.empty() ? s.muted : s.text, true});
        if (!detail.empty() && w - tw > 20.f)
            c.push_back(Text{{r.x + pad + tw + pad * 0.5f, r.y, w - tw - pad * 0.5f, hh}, detail, fs, s.muted});
    }

    // Edges, under the nodes; their labels go on top of everything, so a short edge between two cards still reads.
    std::vector<DrawCmd> labels;
    const float      arrow = std::clamp(9.f * z, 5.f, 12.f);
    const Rect screenArea{-20.f, -20.f, viewport.x + 40.f, viewport.y + 40.f};
    for (std::size_t i = 0; i < graph_->edges.size(); ++i) {
        const Edge& e = graph_->edges[i];
        if (!paths_[i] || !cam.ToScreen(boxes_[i]).Intersects(screenArea)) continue;   // off screen
        const std::vector<Vec2>& wire = *paths_[i];
        static const EdgeType kPlain{};
        const EdgeType* t     = schema_->FindEdge(e.type);
        const EdgeType& type  = t ? *t : kPlain;
        const bool      sel   = chrome && e.id == selEdge_;
        Color wireColor = type.color;
        if (type.portColor && !e.fromPort.empty())
            if (const Node* from = NodeById(e.from))
                if (const PortType* pt = schema_->FindPortType(PortTypeOf(*graph_, *schema_, *from, e.fromPort))) wireColor = pt->color;
        const Color     color = sel ? s.selection : wireColor;
        const auto      path  = ToScreen(cam, wire);
        c.push_back(Polyline{path, color, sel ? 2.5f : 1.5f, type.dashed});
        if (type.arrow) c.push_back(Arrowhead(path, arrow, color));
        if (!e.label.empty() && s.fontSize * z >= 6.f) {
            const Vec2  mid = Midpoint(path);
            const float fs  = s.fontSize * z * 0.9f;
            const float w   = std::max(40.f, fs * 0.55f * static_cast<float>(e.label.size()) + 8.f);
            labels.push_back(FillRect{{mid.x - w * 0.5f, mid.y - fs * 0.8f, w, fs * 1.6f}, s.background, {}, 0.f, 3.f});
            labels.push_back(Text{{mid.x - w * 0.5f, mid.y - fs * 0.8f, w, fs * 1.6f}, e.label, fs, s.muted, false, false, false, Align::Center});
        }
    }

    // A connection being drawn.
    if (chrome && mode_ == Mode::Connecting)
        if (const Node* from = graph_->FindNode(active_)) {
            const Node* to = connectTo_.empty() ? nullptr : graph_->FindNode(connectTo_);
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
        if (PortsOf(n, *schema_).empty()) c.push_back(Circle{ConnectHandle(r), hr, s.background, sel ? s.selection : s.muted, 1.5f});
        const Rect g = ResizeGrip(r, s);
        c.push_back(Polygon{{{g.Right() - 2, g.y + 4}, {g.Right() - 2, g.Bottom() - 2}, {g.x + 4, g.Bottom() - 2}},
                            sel ? s.selection : s.muted});
    };

    // Nodes: unselected, then selected on top.
    auto drawNode = [&](const Node& n) {
        const NodeType& t    = TypeOf(n);
        const bool      sel  = chrome && selNodes_.contains(n.id);
        const bool      hov  = chrome && (n.id == hovered_ || n.id == connectTo_);
        const bool      note = t.shape == NodeShape::Note;
        const Rect      r    = cam.ToScreen(n.frame);
        if (!r.Intersects(Rect{0, 0, viewport.x, viewport.y})) return;

        if (t.shape == NodeShape::Dot) {
            c.push_back(Circle{r.Center(), std::min(r.w, r.h) * 0.5f, t.band, sel ? s.selection : (hov ? s.text : t.band), sel ? 2.f : 1.f});
        } else if (t.shape == NodeShape::Pill) {
            c.push_back(FillRect{r, s.nodeFill, sel ? s.selection : (hov ? s.text : t.band), sel ? 2.f : 1.5f, r.h * 0.5f});
            std::string text;
            for (const FieldDef& f : t.fields) {
                const std::string& v = n.Field(f.id);
                if (!f.onCard || v.empty()) continue;
                if (!text.empty()) text += " · ";
                text += v;
            }
            if (s.fontSize * z >= 4.f)
                c.push_back(Text{{r.x + r.h * 0.4f, r.y, std::max(0.f, r.w - r.h * 0.8f), r.h}, text.empty() ? t.label : text,
                                 s.fontSize * z, text.empty() ? s.muted : s.text, true, false, false, Align::Center});
        } else {
        c.push_back(FillRect{r, note ? s.noteFill : s.nodeFill, sel ? s.selection : (hov ? s.muted : s.nodeStroke),
                             sel ? 2.f : 1.f, s.corner * z});
        const float rad = s.corner * z;
        c.push_back(FillRect{{r.x + 1.f, r.y + rad, std::max(2.f, s.band * z), std::max(0.f, r.h - 2 * rad)}, t.band});

        const float pad = s.padding * z;
        const float x   = r.x + pad + s.band * z;
        const float w   = r.Right() - pad - x;
        float       y   = r.y + pad * 0.8f;
        const float fs  = s.fontSize * z;

        if (fs >= 4.f && w > 4.f) {
            if (note) {
                const std::string& body = n.body;
                c.push_back(Text{{x, y, w, r.Bottom() - pad - y}, body.empty() ? std::string{"Double-click to write"} : body,
                                 fs, body.empty() ? s.muted : s.text, false, true, true});
            } else {
                const std::string& named = t.titleField.empty() ? std::string{} : n.Field(t.titleField);
                const float        ts    = s.titleSize * z;
                const float        iconW = t.drillable ? 18.f * z : 0.f;
                c.push_back(Text{{x, y, w - iconW, ts * 1.3f}, named.empty() ? "Untitled " + t.label : named, ts,
                                 named.empty() ? s.muted : s.text, true});
                y += ts * 1.35f;
                c.push_back(Text{{x, y, w, fs * 1.3f}, t.label, fs * 0.9f, t.band});
                y += fs * 1.5f;

                // Port rows: a left port's label after its pin, a right port's before it; a row holding both splits.
                const auto slots = SlotsOf(n);
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
                    if (const std::string& v = n.Field(sl.def.field); !sl.def.field.empty() && !v.empty() &&
                                                                   !wired(n.id, sl.def.id))
                        text = text.empty() ? v : text + "  " + v;   // a literal or default shows while nothing is wired
                    if (!text.empty())
                        c.push_back(Text{box, text, fs, s.text, false, false, false, left ? Align::Left : Align::Right});
                }
                y += static_cast<float>(PortRows(n, *schema_)) * MetricsOf(s).Row() * z;

                // The picture sits at the foot of the card; text above stops short of it.
                const bool  pictured = t.imageHeight > 0.f && !n.image.empty();
                const float foot     = r.Bottom() - pad * 0.5f - (pictured ? (t.imageHeight + s.padding * 0.5f) * z : 0.f);
                if (pictured)
                    c.push_back(Image{{x, foot + s.padding * 0.25f * z, w, t.imageHeight * z}, n.image, s.corner * z * 0.5f});

                // The node's own rows: a label on the left, a value (or a button, or a choice) on the right.
                const float rowH = MetricsOf(s).Row() * z;
                for (const Row& row : n.rows) {
                    if (y + rowH > foot + 0.5f) break;
                    if (row.separator)
                        c.push_back(Polyline{{{x, y}, {r.Right() - pad, y}}, s.nodeStroke, 1.f});
                    const RowStyle* rs    = schema_->FindRowStyle(row.style);
                    const Color     label = rs && rs->color ? *rs->color : s.text;
                    const Color     value = rs && rs->valueColor ? *rs->valueColor : s.muted;
                    const bool      bold  = rs && rs->bold, italic = rs && rs->italic;
                    std::string     right = row.value;
                    if (row.action == RowAction::Choice) right += right.empty() ? "\u25be" : " \u25be";
                    const float rw = right.empty() ? 0.f : std::min(w * 0.5f, fs * 0.6f * static_cast<float>(right.size()) + pad);
                    const std::string text = (rs ? rs->prefix : std::string{}) + row.label;
                    c.push_back(Text{{x, y, w - rw, rowH}, text, fs, label, bold, false, false, Align::Left, italic});
                    if (row.action == RowAction::Button) {
                        const Rect b{r.Right() - pad - rw, y + rowH * 0.1f, rw, rowH * 0.8f};
                        c.push_back(FillRect{b, s.background, s.muted, 1.f, rowH * 0.3f});
                        c.push_back(Text{b, right, fs, s.text, false, false, false, Align::Center});
                    } else if (!right.empty()) {
                        c.push_back(Text{{r.Right() - pad - rw, y, rw, rowH}, right, fs, value, false, false, false, Align::Right, italic});
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
                    const Color on = n.link.empty() ? s.background : t.band;
                    c.push_back(FillRect{{ix + 0.6f * u, iy, 1.8f * u, 1.4f * u}, on, s.muted, 1.f, 1.f});
                    c.push_back(FillRect{{ix, iy + 0.6f * u, 1.8f * u, 1.4f * u}, on, s.muted, 1.f, 1.f});
                }
            }
        }
        }   // card / note

        drawPins(n, cam);
        if (sel || hov) drawHandles(n, r, sel);
    };
    for (std::size_t i = 0; i < graph_->nodes.size(); ++i)
        if (!grouped_[i] && (!chrome || !selNodes_.contains(graph_->nodes[i].id))) drawNode(graph_->nodes[i]);
    if (chrome)
        for (const std::string& id : selNodes_)
            if (const auto it = index_.find(id); it != index_.end() && !grouped_[it->second]) drawNode(graph_->nodes[it->second]);
    // A group's handles go over what it holds, or its grip could hide under a member.
    if (chrome)
        for (const Node* g : groups) {
            const bool sel = selNodes_.contains(g->id);
            if (sel || g->id == hovered_ || g->id == connectTo_) drawHandles(*g, cam.ToScreen(g->frame), sel);
        }

    c.insert(c.end(), labels.begin(), labels.end());
    // Zoomed far out, text is noise: only the shapes draw.
    if (z < s.textZoom) std::erase_if(c, [](const DrawCmd& d) { return std::holds_alternative<Text>(d); });

    if (chrome && mode_ == Mode::Marquee && started_)
        c.push_back(FillRect{Rect::FromPoints(downAt_, pointer_), s.marquee, s.selection, 1.f});

    return out;
}

} // namespace grapher
