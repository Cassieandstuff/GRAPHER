#include <grapher/Canvas.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>

namespace grapher {

namespace {

constexpr double kLiveRouteMs = 12.0;   ///< a full route slower than this pauses routing while dragging

std::size_t Mix(std::size_t h, std::size_t v) { return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2)); }
std::size_t Bits(float f) { return std::bit_cast<std::uint32_t>(f); }

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
constexpr float kMinZoom       = 0.1f;
constexpr float kMaxZoom       = 4.f;

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

/// @brief The world-space path of an edge between two node frames.
std::vector<Vec2> EdgePath(const Rect& from, const Rect& to, EdgeLine line)
{
    const auto [sa, sb] = FacingSides(from, to);
    return Route(SideAnchor(from, sa), SideAnchor(to, sb), line);
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

Vec2 ConnectHandle(const Rect& screen) { return {screen.Right(), screen.y + screen.h * 0.5f}; }

Rect ResizeGrip(const Rect& screen, const Style& s)
{
    return {screen.Right() - s.resizeGrip, screen.Bottom() - s.resizeGrip, s.resizeGrip, s.resizeGrip};
}

} // namespace

// ── Setup ────────────────────────────────────────────────────────────────────

Canvas::Canvas(Graph& graph, const Schema& schema) : graph_(&graph), schema_(&schema) {}

void Canvas::SetGraph(Graph& graph)
{
    graph_ = &graph;
    mode_  = Mode::Idle;
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

void Canvas::Before(std::string_view label)
{
    if (onBeforeChange) onBeforeChange(label);
}

void Canvas::Changed()
{
    if (onChanged) onChanged();
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
    n.frame = {Snap(worldCenter.x - t->width * 0.5f), Snap(worldCenter.y - t->height * 0.5f), t->width, t->height};
    graph_->nodes.push_back(n);
    SelectNodes({n.id});
    Changed();
    return n.id;
}

bool Canvas::DeleteSelection()
{
    if (!selNodes_.empty()) {
        Before("Delete");
        graph_->RemoveNodes(selNodes_);
    } else if (!selEdge_.empty() && graph_->FindEdge(selEdge_)) {
        Before("Delete");
        graph_->RemoveEdges({selEdge_});
    } else {
        return false;
    }
    ClearSelection();
    Changed();
    return true;
}

// ── Camera ───────────────────────────────────────────────────────────────────

void Canvas::Frame()
{
    if (graph_->nodes.empty()) {
        camera = {{viewport_.x * 0.5f, viewport_.y * 0.5f}, 1.f};
        return;
    }
    const Rect  b      = graph_->Bounds();
    const float margin = 40.f;
    const float zx     = (viewport_.x - 2 * margin) / std::max(b.w, 1.f);
    const float zy     = (viewport_.y - 2 * margin) / std::max(b.h, 1.f);
    camera.zoom        = std::clamp(std::min(zx, zy), kMinZoom, 1.f);
    const Vec2 c       = b.Center();
    camera.pan         = {viewport_.x * 0.5f - c.x * camera.zoom, viewport_.y * 0.5f - c.y * camera.zoom};
}

bool Canvas::Wheel(Vec2 at, float steps)
{
    const float before = camera.zoom;
    const float after  = std::clamp(before * std::pow(1.15f, steps), kMinZoom, kMaxZoom);
    if (after == before) return false;
    const Vec2 world = camera.ToWorld(at);
    camera.zoom      = after;
    camera.pan       = at - world * after;
    return true;
}

Rect Canvas::ScreenRectOf(std::string_view nodeId) const
{
    const Node* n = graph_->FindNode(nodeId);
    return n ? camera.ToScreen(n->frame) : Rect{};
}

// ── Hit testing ──────────────────────────────────────────────────────────────

Hit Canvas::HitTest(Vec2 screen) const
{
    // Topmost first: selected nodes draw last, so they are tested first.
    std::vector<const Node*> order;
    order.reserve(graph_->nodes.size());
    for (auto it = graph_->nodes.rbegin(); it != graph_->nodes.rend(); ++it)
        if (selNodes_.contains(it->id)) order.push_back(&*it);
    for (auto it = graph_->nodes.rbegin(); it != graph_->nodes.rend(); ++it)
        if (!selNodes_.contains(it->id)) order.push_back(&*it);

    const float handle = HandleRadius(style, camera.zoom) + 4.f;
    for (const Node* n : order) {
        const Rect r = camera.ToScreen(n->frame);
        if (Distance(screen, ConnectHandle(r)) <= handle) return {HitKind::Connect, n->id};
        if (r.Contains(screen)) {
            if (ResizeGrip(r, style).Contains(screen)) return {HitKind::Resize, n->id};
            return {HitKind::Node, n->id};
        }
    }
    const WirePaths& wires = Wires();
    for (auto it = graph_->edges.rbegin(); it != graph_->edges.rend(); ++it) {
        const auto w = wires.find(it->id);
        if (w == wires.end() || w->second.size() < 2) continue;
        if (PolylineDistance(screen, ToScreen(camera, w->second)) <= kEdgeHitPx) return {HitKind::Edge, it->id};
    }
    return {};
}

const WirePaths& Canvas::Wires() const
{
    const bool     fast = (mode_ == Mode::Moving || mode_ == Mode::Resizing) && started_ && routeMs_ > kLiveRouteMs;
    RoutingOptions o    = routing;
    if (fast) o.avoidNodes = false;

    const std::hash<std::string_view> hs;
    std::size_t key = Mix(reinterpret_cast<std::uintptr_t>(schema_), fast);
    key = Mix(key, (o.avoidNodes ? 1u : 0u) | (o.spreadPorts ? 2u : 0u));
    for (float f : {o.clearance, o.bendCost, o.shareCost, o.cornerRadius, o.curveRadius}) key = Mix(key, Bits(f));
    for (const Node& n : graph_->nodes) {
        key = Mix(key, hs(n.id));
        for (float f : {n.frame.x, n.frame.y, n.frame.w, n.frame.h}) key = Mix(key, Bits(f));
    }
    for (const Edge& e : graph_->edges) {
        for (const std::string* s : {&e.id, &e.from, &e.to, &e.type}) key = Mix(key, hs(*s));
        const EdgeType* t = schema_->FindEdge(e.type);
        key = Mix(key, t ? static_cast<std::size_t>(t->line) + 1 : 0);
    }
    if (wiresValid_ && key == wiresKey_) return wires_;

    const auto start = std::chrono::steady_clock::now();
    wires_           = RouteWires(*graph_, *schema_, o);
    if (!fast) routeMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    wiresKey_   = key;
    wiresValid_ = true;
    return wires_;
}

// ── Gestures ─────────────────────────────────────────────────────────────────

bool Canvas::PointerDown(Vec2 at, Button button, Modifiers mods)
{
    downAt_ = pointer_ = at;
    started_ = false;
    origin_.clear();

    if (button != Button::Left) {
        mode_      = Mode::Panning;
        panAtDown_ = camera.pan;
        return false;
    }

    const Hit hit = HitTest(at);
    switch (hit.kind) {
    case HitKind::Connect:
        mode_   = Mode::Connecting;
        active_ = hit.id;
        connectTo_.clear();
        return true;
    case HitKind::Resize:
        if (!selNodes_.contains(hit.id)) SelectNodes({hit.id});
        mode_   = Mode::Resizing;
        active_ = hit.id;
        origin_.emplace_back(hit.id, graph_->FindNode(hit.id)->frame);
        return true;
    case HitKind::Node:
        if (mods.ctrl || mods.shift) {
            NodeSet next = selNodes_;
            if (!next.erase(hit.id)) next.insert(hit.id);
            SelectNodes(std::move(next));
            mode_ = Mode::Idle;
            return true;
        }
        if (!selNodes_.contains(hit.id)) SelectNodes({hit.id});
        mode_ = Mode::Moving;
        for (const Node& n : graph_->nodes)
            if (selNodes_.contains(n.id)) origin_.emplace_back(n.id, n.frame);
        return true;
    case HitKind::Edge:
        SelectEdge(hit.id);
        mode_ = Mode::Idle;
        return true;
    case HitKind::None:
    default:
        additive_ = mods.ctrl || mods.shift;
        if (!additive_) ClearSelection();
        mode_ = Mode::Marquee;
        return true;
    }
}

bool Canvas::PointerMove(Vec2 at)
{
    pointer_ = at;
    if (mode_ != Mode::Idle && !started_) {
        if (Distance(at, downAt_) < kDragThreshold) return false;
        started_ = true;
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
        const Hit hit = HitTest(at);
        connectTo_    = (hit.kind != HitKind::None && hit.kind != HitKind::Edge && hit.id != active_) ? hit.id : std::string{};
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
    case Mode::Moving:
    case Mode::Resizing:
        if (started_) Changed();
        return true;
    case Mode::Connecting: {
        const std::string to = std::exchange(connectTo_, {});
        if (to.empty()) return true;
        for (const Edge& e : graph_->edges)
            if (e.from == active_ && e.to == to) return true;   // one edge per ordered pair
        Before("Connect");
        Edge e;
        e.id   = NewId();
        e.from = active_;
        e.to   = to;
        const Node* fromNode = graph_->FindNode(active_);
        const Node* toNode   = graph_->FindNode(to);
        e.type = schema_->EdgeFor(fromNode ? fromNode->type : std::string{}, toNode ? toNode->type : std::string{});
        graph_->edges.push_back(e);
        Changed();
        return true;
    }
    case Mode::Marquee: {
        if (!started_) return true;
        const Rect box  = camera.ToWorld(Rect::FromPoints(downAt_, at));
        NodeSet    next = additive_ ? selNodes_ : NodeSet{};
        for (const Node& n : graph_->nodes)
            if (box.Intersects(n.frame)) next.insert(n.id);
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

    // Edges, under the nodes; their labels go on top of everything, so a short edge between two cards still reads.
    std::vector<DrawCmd> labels;
    const float      arrow = std::clamp(9.f * z, 5.f, 12.f);
    const WirePaths& wires = Wires();
    for (const Edge& e : graph_->edges) {
        const auto wire = wires.find(e.id);
        if (wire == wires.end() || wire->second.size() < 2) continue;
        static const EdgeType kPlain{};
        const EdgeType* t     = schema_->FindEdge(e.type);
        const EdgeType& type  = t ? *t : kPlain;
        const bool      sel   = chrome && e.id == selEdge_;
        const Color     color = sel ? s.selection : type.color;
        const auto      path  = ToScreen(cam, wire->second);
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
            const Node*       to = connectTo_.empty() ? nullptr : graph_->FindNode(connectTo_);
            std::vector<Vec2> path;
            if (to) {
                path = ToScreen(cam, EdgePath(from->frame, to->frame, EdgeLine::Curve));
            } else {
                const Vec2 cursor = cam.ToWorld(pointer_);
                const auto side   = FacingSides(from->frame, Rect{cursor.x, cursor.y, 0, 0}).first;
                path = ToScreen(cam, Route(SideAnchor(from->frame, side), {cursor, {0, 0}}, EdgeLine::Curve));
            }
            c.push_back(Polyline{path, s.selection, 2.f, true});
        }

    // Nodes: unselected, then selected on top.
    auto drawNode = [&](const Node& n) {
        const NodeType& t    = TypeOf(n);
        const bool      sel  = chrome && selNodes_.contains(n.id);
        const bool      hov  = chrome && (n.id == hovered_ || n.id == connectTo_);
        const bool      note = t.shape == NodeShape::Note;
        const Rect      r    = cam.ToScreen(n.frame);
        if (!r.Intersects(Rect{0, 0, viewport.x, viewport.y})) return;

        if (t.shape == NodeShape::Pill) {
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

                if (z >= s.detailZoom) {
                    const float line = fs * 1.4f;
                    for (const FieldDef& f : t.fields) {
                        if (!f.onCard || f.id == t.titleField) continue;
                        const std::string& v = n.Field(f.id);
                        if (v.empty()) continue;
                        if (y + line > r.Bottom() - pad * 0.5f) break;
                        const float lw = std::min(w * 0.38f, 90.f * z);
                        c.push_back(Text{{x, y, lw, line}, f.label, fs, s.muted});
                        c.push_back(Text{{x + lw, y, w - lw, line}, v, fs, s.text});
                        y += line;
                    }
                    if (!n.body.empty() && y + line <= r.Bottom() - pad * 0.5f) {
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

        if (sel || hov) {
            const float hr = HandleRadius(s, z);
            c.push_back(Circle{ConnectHandle(r), hr, s.background, sel ? s.selection : s.muted, 1.5f});
            const Rect g = ResizeGrip(r, s);
            c.push_back(Polygon{{{g.Right() - 2, g.y + 4}, {g.Right() - 2, g.Bottom() - 2}, {g.x + 4, g.Bottom() - 2}},
                                sel ? s.selection : s.muted});
        }
    };
    for (const Node& n : graph_->nodes)
        if (!chrome || !selNodes_.contains(n.id)) drawNode(n);
    if (chrome)
        for (const Node& n : graph_->nodes)
            if (selNodes_.contains(n.id)) drawNode(n);

    c.insert(c.end(), labels.begin(), labels.end());

    if (chrome && mode_ == Mode::Marquee && started_)
        c.push_back(FillRect{Rect::FromPoints(downAt_, pointer_), s.marquee, s.selection, 1.f});

    return out;
}

} // namespace grapher
