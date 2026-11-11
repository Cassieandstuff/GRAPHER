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
        if (IsMemberGroup(n, *schema_) && (!which || which(n))) SetCollapsed(n.id, collapsed);
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
    camera.zoom    = std::clamp(std::min({zx, zy, zoomCap}), minZoom, maxZoom);
    const Vec2 c   = b->Center();
    camera.pan     = {viewport_.x * 0.5f - c.x * camera.zoom, viewport_.y * 0.5f - c.y * camera.zoom};
}

void Canvas::CenterOn(std::string_view nodeId, std::optional<float> zoom)
{
    const Node* n = ShownNode(nodeId);
    if (!n) return;
    if (zoom) camera.zoom = std::clamp(*zoom, minZoom, maxZoom);
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
    const Node* n = ShownNode(nodeId);
    if (!n) return std::nullopt;
    const CardMetrics m = MetricsOf(style);
    const float       y = IsGroup(*n, *schema_) ? std::min(n->frame.h, m.groupHeader) * 0.5f : m.Top() + m.titleSize * 0.65f;
    return camera.ToScreen(Vec2{n->frame.Center().x, n->frame.y + std::min(y, n->frame.h)});
}

std::optional<Rect> Canvas::RowScreenRect(std::string_view nodeId, std::string_view row) const
{
    const Node* n = ShownNode(nodeId);
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
        if (const Node* n = ShownNode(hit.id)) {
            const Row* r = n->FindRow(hit.row);
            return r && !r->tooltip.empty() ? r->tooltip : n->tooltip;
        }
        return {};
    case HitKind::Port:
        if (const Node* n = ShownNode(hit.id))
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
            if (TypeOf(*n).shape == NodeShape::Dial) {
                // A dial's handles sit inside its ring, one per row on it.
                const Ring     ring = RingOf(n->frame);
                const DialAxis axis = AxisOf(*n);
                for (const Row& row : n->rows)
                    if (!row.port.empty() && Distance(screen, camera.ToScreen(ring.At(axis.Angle(RowNumber(row)), kHandleAt))) <= pin)
                        return {HitKind::Handle, n->id, {}, row.id};
                return {HitKind::Node, n->id};
            }
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
    // Wires into a closed group end on it; several that then join the same two ends draw as one, dashed.
    std::vector<Edge>               edges;
    std::vector<char>               dashed;
    std::unordered_set<std::string> seen;
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
        if (ra != a->second) x.from = view_.nodes[ra].id, x.fromPort.clear(), remapped = true;
        if (rb != b->second) x.to = view_.nodes[rb].id, x.toPort.clear(), remapped = true;
        if (remapped && !seen.insert(x.from + '\x1f' + x.fromPort + '\x1f' + x.to + '\x1f' + x.toPort).second) continue;
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
    const CardMetrics     m = MetricsOf(style);
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
                        const Rect line = RowRect(view_.nodes[i], *schema_, m, k);
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

void Canvas::DrawDial(std::vector<DrawCmd>& c, const Node& n, const NodeType& t, const Camera& cam, bool sel, bool hov) const
{
    constexpr double kPi = std::numbers::pi;
    const Style&     s   = style;
    const float      z   = cam.zoom;
    const Ring       ring = RingOf(n.frame);
    const DialAxis   axis = AxisOf(n);
    const Vec2       o    = cam.ToScreen(ring.center);
    const float      R    = ring.radius * z;
    const float      fs   = s.fontSize * z;
    auto dir = [](double a) { return Vec2{static_cast<float>(std::sin(a)), static_cast<float>(-std::cos(a))}; };

    const Rect r = cam.ToScreen(n.frame);
    c.push_back(FillRect{r, s.nodeFill, sel ? s.selection : (hov ? s.muted : s.nodeStroke), sel ? 2.f : 1.f, s.corner * z});
    // The ring: whole when cyclic, else a 270° arc open at the bottom.
    const double      a0 = axis.cyclic ? 0.0 : -0.75 * kPi, a1 = axis.cyclic ? 2 * kPi : 0.75 * kPi;
    std::vector<Vec2> arc;
    for (int k = 0; k <= 64; ++k) arc.push_back(o + dir(a0 + (a1 - a0) * k / 64.0) * R);
    c.push_back(Polyline{std::move(arc), t.band, std::max(1.5f, 3.f * z)});
    const std::string& named = t.titleField.empty() ? std::string{} : n.Field(t.titleField);
    c.push_back(Text{{r.x + s.padding * z, r.y + s.padding * 0.3f * z, r.w - 2 * s.padding * z, s.titleSize * z * 1.3f},
                     named.empty() ? t.label : named, s.titleSize * z, s.text, true, false, false, Align::Center});
    // The needle, and what it reads.
    const Vec2 tip = o + dir(axis.Angle(n.dial.value)) * (R * 0.7f);
    c.push_back(Polyline{{o, tip}, s.text, 2.f});
    c.push_back(Circle{o, std::max(2.f, 3.f * z), s.text, s.text, 0.f});
    char       buf[32];
    const auto end = std::to_chars(buf, buf + sizeof buf, n.dial.value).ptr;
    c.push_back(Text{{o.x - R, o.y + R * 0.2f, 2 * R, fs * 1.3f}, n.dial.label + "  " + std::string(buf, end), fs, s.muted, false,
                     false, false, Align::Center});
    // Each row on the ring: a handle inside it (following a drag), its label outside.
    const Color gold{220, 180, 60};
    for (const Row& row : n.rows) {
        if (row.port.empty()) continue;
        const bool  dragging = mode_ == Mode::DialDrag && active_ == n.id && pressRow_ == row.id;
        const float v        = dragging ? dialValue_ : RowNumber(row);
        const auto  a        = axis.Angle(v);
        const Vec2  h        = o + dir(a) * (R * kHandleAt);
        const float d        = std::clamp(5.f * z, 3.f, 7.f);
        c.push_back(Polygon{{{h.x, h.y - d}, {h.x + d, h.y}, {h.x, h.y + d}, {h.x - d, h.y}}, gold});
        const Vec2  l = o + dir(a) * (R * 1.32f);
        const float w = 110.f * z;
        std::string text = row.label;
        if (dragging) {
            const auto e = std::to_chars(buf, buf + sizeof buf, v).ptr;
            text += " " + std::string(buf, e);
        } else if (!row.value.empty()) {
            text += " " + row.value;
        }
        c.push_back(Text{{l.x - w * 0.5f, l.y - fs * 0.65f, w, fs * 1.3f}, text, fs * 0.9f, s.text, false, false, false, Align::Center});
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
    const bool     local = !router && dragging && graph_->nodes.size() > kLargeGraph && wiresValid_ && synced_ == graph_;
    const bool     plain = (!router || patch) && dragging && routeMs_ > kLiveRouteMs && !local;
    RoutingOptions o     = routing;
    if (plain) o.avoidNodes = false;
    o.card = MetricsOf(style);

    std::size_t optionsKey = Mix(reinterpret_cast<std::uintptr_t>(router.get()),
                                 (o.avoidNodes ? 1u : 0u) | (o.spreadPorts ? 2u : 0u));
    for (float f : {o.clearance, o.bendCost, o.shareCost, o.crossCost, o.laneSpacing, o.cornerRadius, o.curveRadius,
                    o.card.padding, o.card.titleSize, o.card.fontSize, o.card.groupHeader, style.groupPadding})
        optionsKey = Mix(optionsKey, Bits(f));

    const auto  hs  = HashText;
    // A drag takes the dragged nodes out of their docks while it lasts, so whether one is under way is part of the key.
    std::size_t key = Mix(reinterpret_cast<std::uintptr_t>(schema_),
                          (plain ? 1u : 0u) | (patch ? 2u : 0u) | (local ? 4u : 0u) | (dragging && mode_ == Mode::Moving && hasDocks_ ? 8u : 0u));
    key = Mix(key, reinterpret_cast<std::uintptr_t>(graph_));
    key = Mix(Mix(Mix(key, optionsKey), static_cast<std::size_t>(schema_->revision)), viewRev_);
    for (const Node& n : graph_->nodes) {
        key = Mix(Mix(Mix(key, hs(n.id)), hs(n.type)), n.parent.empty() ? 0 : hs(n.parent));
        if (!n.dock.parent.empty())
            key = Mix(Mix(Mix(key, hs(n.dock.parent)), hs(n.dock.row)),
                      static_cast<std::size_t>(n.dock.order) * 16 + static_cast<std::size_t>(n.dock.side) * 2 + (n.dock.flush ? 1 : 0));
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

    std::unordered_map<std::string_view, const NodeType*> types;
    types.reserve(schema_->nodeTypes.size());
    for (const NodeType& t : schema_->nodeTypes) types.emplace(t.id, &t);

    const auto start   = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
    if (local) {
        // Only the wires of the dragged nodes (and of the open groups around them, which grow with them) reroute, among
        // the nodes they join; the rest keep their settled paths.
        std::unordered_set<std::string_view> moving, joined;
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
        const Graph&             shown = Shown();
        Graph                    sub;
        std::vector<std::size_t> touched;
        for (std::size_t i = 0; i < shown.edges.size(); ++i)
            if (const Edge& e = shown.edges[i]; moving.contains(e.from) || moving.contains(e.to)) {
                touched.push_back(i);
                sub.edges.push_back(e);
                joined.insert(e.from);
                joined.insert(e.to);
            }
        for (const Node& n : shown.nodes)
            if (joined.contains(n.id)) sub.nodes.push_back(n);
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
                !n.dock.parent.empty() || (t != types.end() && t->second->groupMode == GroupMode::Members && t->second->shape == NodeShape::Group)) {
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
    for (std::size_t i = 0; i < shown.edges.size(); ++i) {
        const Edge& e = shown.edges[i];
        if (!e.fromPort.empty()) wiredPorts_.insert(PortKey(e.from, e.fromPort));
        if (!e.toPort.empty()) wiredPorts_.insert(PortKey(e.to, e.toPort));
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

    if (button != Button::Left) {
        mode_      = panWithButtons ? Mode::Panning : Mode::Idle;
        panAtDown_ = camera.pan;
        return false;
    }

    const Hit hit = HitTest(at);
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
    case Mode::DialDrag:
        if (const Node* n = ShownNode(active_)) {
            const Ring  ring = RingOf(n->frame);
            const Vec2  d    = camera.ToWorld(at) - ring.center;
            const float v    = AxisOf(*n).Value(std::atan2(d.x, -d.y));
            dialValue_       = snapDial_ ? AxisOf(*n).Snap(v) : v;
        }
        return true;
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
    PointerMove(at, Modifiers{snapDial_, false, false});   // the last move keeps the snap the drag had
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
    for (std::size_t i = 0; i < Shown().nodes.size(); ++i)
        if (grouped_[i]) groups.push_back(&Shown().nodes[i]);
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
        std::string        title = named.empty() ? "Untitled " + t.label : named;
        if (t.groupMode == GroupMode::Members) title = (IsCollapsed(g->id) ? "▸ " : "▾ ") + title;   // its arrow
        if (t.showCount) title += " (" + std::to_string(count_[static_cast<std::size_t>(g - Shown().nodes.data())]) + ")";
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
    for (std::size_t i = 0; i < Shown().edges.size(); ++i) {
        const Edge& e = Shown().edges[i];
        if (!paths_[i] || !cam.ToScreen(boxes_[i]).Intersects(screenArea)) continue;   // off screen
        if (const Node* a = NodeById(Shown().edges[i].from), *b = NodeById(Shown().edges[i].to); (a && !Visible(*a)) || (b && !Visible(*b)))
            continue;   // an end shows only with what it belongs to
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
        c.push_back(Polyline{path, color, sel ? type.width + 1.f : type.width, !type.dotted && (type.dashed || dashed_[i]), type.dotted});
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

        if (!Visible(n)) return;
        if (t.shape == NodeShape::Dial) {
            DrawDial(c, n, t, cam, sel, hov);
        } else if (t.shape == NodeShape::Dot) {
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
                    if (rs && rs->chip) {
                        const float cw = std::min(w - rw, fs * 0.6f * static_cast<float>(text.size()) + pad);
                        c.push_back(FillRect{{x - pad * 0.3f, y + rowH * 0.1f, cw, rowH * 0.8f}, Color{label.r, label.g, label.b, 40}, {}, 0.f, rowH * 0.4f});
                    }
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

        // An alias draws dimmed, with a dashed outline.
        if (!n.aliasOf.empty())
            c.push_back(FillRect{r, Color{s.background.r, s.background.g, s.background.b, 120}, s.muted, 1.f, s.corner * z, true});
        drawPins(n, cam);
        if (sel || hov) drawHandles(n, r, sel);
    };
    for (std::size_t i = 0; i < Shown().nodes.size(); ++i)
        if (!grouped_[i] && (!chrome || !selNodes_.contains(Shown().nodes[i].id))) drawNode(Shown().nodes[i]);
    if (chrome)
        for (const std::string& id : selNodes_)
            if (const auto it = index_.find(id); it != index_.end() && !grouped_[it->second]) drawNode(Shown().nodes[it->second]);
    // A group's handles go over what it holds, or its grip could hide under a member.
    if (chrome)
        for (const Node* g : groups) {
            const bool sel = selNodes_.contains(g->id);
            if (sel || g->id == hovered_ || g->id == connectTo_) drawHandles(*g, cam.ToScreen(g->frame), sel);
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
