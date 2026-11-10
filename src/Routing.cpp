#include <grapher/Routing.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <queue>
#include <tuple>

namespace grapher {

namespace {

constexpr int   kCurveSegments = 24;
constexpr int   kCornerSteps   = 6;
constexpr float kEps           = 0.01f;

enum Side { Left = 0, Right = 1, Top = 2, Bottom = 3 };

struct Port {
    Vec2 p;
    Vec2 n;   ///< outward normal
};

Vec2 NormalOf(Side s)
{
    switch (s) {
    case Left: return {-1, 0};
    case Right: return {1, 0};
    case Top: return {0, -1};
    default: return {0, 1};
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

/// @brief The point at fraction `t` along a rectangle's side.
Vec2 OnSide(const Rect& r, Side s, float t)
{
    switch (s) {
    case Left: return {r.x, r.y + r.h * t};
    case Right: return {r.Right(), r.y + r.h * t};
    case Top: return {r.x + r.w * t, r.y};
    default: return {r.x + r.w * t, r.Bottom()};
    }
}

std::vector<Vec2> Bezier(Port a, Port b)
{
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

std::vector<Vec2> PlainElbow(Port a, Port b)
{
    if (a.n.x != 0.f) {
        const float mx = (a.p.x + b.p.x) * 0.5f;
        return {a.p, {mx, a.p.y}, {mx, b.p.y}, b.p};
    }
    const float my = (a.p.y + b.p.y) * 0.5f;
    return {a.p, {a.p.x, my}, {b.p.x, my}, b.p};
}

/// @brief Whether segment a-b passes through the inside of `r` (Liang-Barsky).
bool SegmentHitsRect(Vec2 a, Vec2 b, const Rect& r)
{
    float t0 = 0.f, t1 = 1.f;
    const float dx = b.x - a.x, dy = b.y - a.y;
    auto clip = [&](float p, float q) {
        if (p == 0.f) return q > 0.f;
        const float t = q / p;
        if (p < 0.f) {
            if (t > t1) return false;
            t0 = std::max(t0, t);
        } else {
            if (t < t0) return false;
            t1 = std::min(t1, t);
        }
        return true;
    };
    return clip(-dx, a.x - r.x) && clip(dx, r.Right() - a.x) && clip(-dy, a.y - r.y) && clip(dy, r.Bottom() - a.y) && t1 - t0 > 1e-4f;
}

bool PathClear(const std::vector<Vec2>& pts, const std::vector<Rect>& nodes, std::size_t skipA, std::size_t skipB, float pad)
{
    for (std::size_t k = 0; k < nodes.size(); ++k) {
        if (k == skipA || k == skipB) continue;
        const Rect r = nodes[k].Inflated(pad);
        for (std::size_t i = 1; i < pts.size(); ++i)
            if (SegmentHitsRect(pts[i - 1], pts[i], r)) return false;
    }
    return true;
}

/// @brief Drops repeated points and the middle of three collinear ones.
std::vector<Vec2> Simplify(const std::vector<Vec2>& pts)
{
    std::vector<Vec2> out;
    for (Vec2 p : pts) {
        if (!out.empty() && Distance(out.back(), p) < kEps) continue;
        if (out.size() >= 2) {
            const Vec2 a = out[out.size() - 2], b = out.back();
            const float cross = (b.x - a.x) * (p.y - b.y) - (b.y - a.y) * (p.x - b.x);
            if (std::abs(cross) < 1e-3f) {
                out.back() = p;
                continue;
            }
        }
        out.push_back(p);
    }
    return out;
}

/// @brief Rounds each corner with a quadratic arc of up to `radius` (never more than half of either neighbouring run).
std::vector<Vec2> RoundCorners(const std::vector<Vec2>& pts, float radius)
{
    if (radius <= 0.f || pts.size() < 3) return pts;
    std::vector<Vec2> out{pts.front()};
    for (std::size_t i = 1; i + 1 < pts.size(); ++i) {
        const Vec2  a = pts[i - 1], p = pts[i], b = pts[i + 1];
        const float la = Distance(a, p), lb = Distance(p, b);
        const float r  = std::min({radius, la * 0.5f, lb * 0.5f});
        if (r < 0.5f) {
            out.push_back(p);
            continue;
        }
        const Vec2 p1 = p + (a - p) / la * r;
        const Vec2 p2 = p + (b - p) / lb * r;
        for (int k = 0; k <= kCornerSteps; ++k) {
            const float t = static_cast<float>(k) / kCornerSteps, u = 1.f - t;
            out.push_back(p1 * (u * u) + p * (2 * u * t) + p2 * (t * t));
        }
    }
    out.push_back(pts.back());
    return out;
}

/// @brief Sorted coordinates with near-duplicates (closer than 0.25) merged.
std::vector<float> Coordinates(std::vector<float> v)
{
    std::sort(v.begin(), v.end());
    std::vector<float> out;
    for (float c : v)
        if (out.empty() || c - out.back() > 0.25f) out.push_back(c);
    return out;
}

/// @brief The sparse routing grid: every line through a node outline (inflated by the clearance), the lane between two
///        neighbouring outlines, a ring around everything, and every port stub. Points inside an outline are blocked, and
///        so are the steps that would cross one.
class Grid {
public:
    Grid(const std::vector<Rect>& obstacles, std::vector<float> xs, std::vector<float> ys)
        : xs_(Coordinates(std::move(xs))), ys_(Coordinates(std::move(ys))), nx_(xs_.size()), ny_(ys_.size())
    {
        free_.assign(nx_ * ny_, 1);
        hOk_.assign(nx_ * ny_, 1);   // (i,j) -> (i+1,j)
        vOk_.assign(nx_ * ny_, 1);   // (i,j) -> (i,j+1)
        hUse_.assign(nx_ * ny_, 0);
        vUse_.assign(nx_ * ny_, 0);
        for (const Rect& r : obstacles) {
            const auto inX = Range(xs_, r.x, r.Right());
            const auto inY = Range(ys_, r.y, r.Bottom());
            for (std::size_t j = inY.first; j < inY.second; ++j)
                for (std::size_t i = inX.first; i < inX.second; ++i) free_[j * nx_ + i] = 0;
            // A step between two lines on the outline (or one outside it) crosses the inside when it runs through it.
            for (std::size_t j = inY.first; j < inY.second; ++j)
                for (std::size_t i = 0; i + 1 < nx_; ++i)
                    if (xs_[i] >= r.x - kEps && xs_[i + 1] <= r.Right() + kEps) hOk_[j * nx_ + i] = 0;
            for (std::size_t i = inX.first; i < inX.second; ++i)
                for (std::size_t j = 0; j + 1 < ny_; ++j)
                    if (ys_[j] >= r.y - kEps && ys_[j + 1] <= r.Bottom() + kEps) vOk_[j * nx_ + i] = 0;
        }
        g_.assign(nx_ * ny_ * 4, 0.f);
        parent_.assign(nx_ * ny_ * 4, -1);
        stamp_.assign(nx_ * ny_ * 4, 0);
        closed_.assign(nx_ * ny_ * 4, 0);
    }

    /// @return the grid points of a route from `from` (leaving along `dirFrom`) to `to` (arriving along `dirTo`), or empty
    std::vector<Vec2> Route(Vec2 from, Vec2 dirFrom, Vec2 to, Vec2 dirTo, const RoutingOptions& o)
    {
        const long s = Find(from), t = Find(to);
        if (s < 0 || t < 0 || !free_[static_cast<std::size_t>(s)] || !free_[static_cast<std::size_t>(t)]) return {};
        ++gen_;
        const int d0 = DirOf(dirFrom), dEnd = DirOf(dirTo);

        using Item = std::tuple<float, float, long>;   // f, g, state
        std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
        auto h = [&](long idx) {
            const auto [i, j] = IJ(idx);
            return std::abs(xs_[i] - to.x) + std::abs(ys_[j] - to.y);
        };
        auto push = [&](long state, float g, long parent) {
            const auto k = static_cast<std::size_t>(state);
            if (stamp_[k] == gen_ && g >= g_[k]) return;
            stamp_[k]  = gen_;
            closed_[k] = 0;
            g_[k]      = g;
            parent_[k] = parent;
            open.emplace(g + h(state / 4), g, state);
        };
        push(s * 4 + d0, 0.f, -1);

        float best = std::numeric_limits<float>::infinity();
        long  bestState = -1;
        while (!open.empty()) {
            const auto [f, g, state] = open.top();
            open.pop();
            if (f >= best) break;
            const auto k = static_cast<std::size_t>(state);
            if (closed_[k] || g > g_[k]) continue;
            closed_[k]     = 1;
            const long idx = state / 4;
            const int  dir = static_cast<int>(state % 4);
            if (idx == t) {
                // Arriving against the end's direction is free; at right angles is one bend; head-on is two.
                const float extra = dir == dEnd ? 0.f : (dir == Opposite(dEnd) ? 2.f : 1.f) * o.bendCost;
                if (g + extra < best) {
                    best      = g + extra;
                    bestState = state;
                }
                continue;
            }
            const auto [i, j] = IJ(idx);
            for (int nd = 0; nd < 4; ++nd) {
                if (nd == Opposite(dir)) continue;
                long  next = -1;
                float len = 0.f, share = 0.f;
                switch (nd) {
                case 0:   // +x
                    if (i + 1 < nx_ && hOk_[j * nx_ + i] && free_[j * nx_ + i + 1]) {
                        next = idx + 1, len = xs_[i + 1] - xs_[i], share = static_cast<float>(hUse_[j * nx_ + i]);
                    }
                    break;
                case 1:   // -x
                    if (i > 0 && hOk_[j * nx_ + i - 1] && free_[j * nx_ + i - 1]) {
                        next = idx - 1, len = xs_[i] - xs_[i - 1], share = static_cast<float>(hUse_[j * nx_ + i - 1]);
                    }
                    break;
                case 2:   // +y
                    if (j + 1 < ny_ && vOk_[j * nx_ + i] && free_[(j + 1) * nx_ + i]) {
                        next = idx + static_cast<long>(nx_), len = ys_[j + 1] - ys_[j], share = static_cast<float>(vUse_[j * nx_ + i]);
                    }
                    break;
                default:  // -y
                    if (j > 0 && vOk_[(j - 1) * nx_ + i] && free_[(j - 1) * nx_ + i]) {
                        next = idx - static_cast<long>(nx_), len = ys_[j] - ys_[j - 1], share = static_cast<float>(vUse_[(j - 1) * nx_ + i]);
                    }
                    break;
                }
                if (next < 0) continue;
                const float cost = len + share * o.shareCost + (nd != dir ? o.bendCost : 0.f);
                push(next * 4 + nd, g + cost, state);
            }
        }
        if (bestState < 0) return {};

        std::vector<long> chain;
        for (long st = bestState; st >= 0; st = parent_[static_cast<std::size_t>(st)]) chain.push_back(st / 4);
        std::reverse(chain.begin(), chain.end());
        std::vector<Vec2> pts;
        for (std::size_t k = 0; k < chain.size(); ++k) {
            const auto [i, j] = IJ(chain[k]);
            pts.push_back({xs_[i], ys_[j]});
            if (k > 0) Use(chain[k - 1], chain[k]);
        }
        return pts;
    }

private:
    static int  DirOf(Vec2 d) { return std::abs(d.x) >= std::abs(d.y) ? (d.x >= 0 ? 0 : 1) : (d.y >= 0 ? 2 : 3); }
    static int  Opposite(int d) { return d ^ 1; }
    std::pair<std::size_t, std::size_t> IJ(long idx) const
    {
        return {static_cast<std::size_t>(idx) % nx_, static_cast<std::size_t>(idx) / nx_};
    }

    /// @brief The index range of coordinates strictly inside (lo, hi).
    static std::pair<std::size_t, std::size_t> Range(const std::vector<float>& c, float lo, float hi)
    {
        const auto a = std::upper_bound(c.begin(), c.end(), lo + kEps);
        const auto b = std::lower_bound(c.begin(), c.end(), hi - kEps);
        const auto first = static_cast<std::size_t>(a - c.begin());
        const auto last  = static_cast<std::size_t>(b - c.begin());
        return {first, std::max(first, last)};
    }

    static long Nearest(const std::vector<float>& c, float v)
    {
        const auto it = std::lower_bound(c.begin(), c.end(), v - 0.5f);
        if (it == c.end() || std::abs(*it - v) > 0.5f) return -1;
        return static_cast<long>(it - c.begin());
    }

    long Find(Vec2 p) const
    {
        const long i = Nearest(xs_, p.x), j = Nearest(ys_, p.y);
        return i < 0 || j < 0 ? -1 : j * static_cast<long>(nx_) + i;
    }

    void Use(long a, long b)
    {
        const long lo = std::min(a, b), hi = std::max(a, b);
        if (hi - lo == 1) ++hUse_[static_cast<std::size_t>(lo)];
        else ++vUse_[static_cast<std::size_t>(lo)];
    }

    std::vector<float>    xs_, ys_;
    std::size_t           nx_, ny_;
    std::vector<char>     free_, hOk_, vOk_;
    std::vector<int>      hUse_, vUse_;
    std::vector<float>    g_;
    std::vector<long>     parent_;
    std::vector<uint32_t> stamp_;
    std::vector<char>     closed_;
    uint32_t              gen_ = 0;
};

struct Wire {
    const Edge* edge;
    std::size_t a, b;   ///< node indices
    Side        sa, sb;
    Port        pa, pb;
    EdgeLine    line;
};

} // namespace

WirePaths RouteWires(const Graph& graph, const Schema& schema, const RoutingOptions& o)
{
    std::unordered_map<std::string_view, std::size_t> index;
    std::vector<Rect> frames;
    for (std::size_t k = 0; k < graph.nodes.size(); ++k) {
        index.emplace(graph.nodes[k].id, k);
        frames.push_back(graph.nodes[k].frame);
    }

    std::vector<Wire> wires;
    for (const Edge& e : graph.edges) {
        const auto ia = index.find(e.from), ib = index.find(e.to);
        if (ia == index.end() || ib == index.end() || ia->second == ib->second) continue;
        const auto [sa, sb] = FacingSides(frames[ia->second], frames[ib->second]);
        const EdgeType* t   = schema.FindEdge(e.type);
        wires.push_back({&e, ia->second, ib->second, sa, sb, {}, {}, t ? t->line : EdgeLine::Curve});
    }

    // Ports: the ends on one side of a node spread along it, ordered by where their other end sits, so they leave
    // without crossing each other.
    struct End {
        std::size_t wire;
        bool        source;
        float       key;
    };
    std::map<std::pair<std::size_t, int>, std::vector<End>> sides;
    for (std::size_t w = 0; w < wires.size(); ++w) {
        const Wire& wire = wires[w];
        auto        key  = [&](std::size_t other, Side s) {
            const Vec2 c = frames[other].Center();
            return (s == Left || s == Right) ? c.y : c.x;
        };
        sides[{wire.a, wire.sa}].push_back({w, true, key(wire.b, wire.sa)});
        sides[{wire.b, wire.sb}].push_back({w, false, key(wire.a, wire.sb)});
    }
    for (auto& [nodeSide, ends] : sides) {
        std::stable_sort(ends.begin(), ends.end(), [](const End& x, const End& y) { return x.key < y.key; });
        const Rect& r     = frames[nodeSide.first];
        const auto  side  = static_cast<Side>(nodeSide.second);
        const float len   = (side == Left || side == Right) ? r.h : r.w;
        const float inset = std::min(len * 0.2f, 14.f) / std::max(len, 1.f);
        for (std::size_t k = 0; k < ends.size(); ++k) {
            const float t = o.spreadPorts ? inset + (1.f - 2 * inset) * (static_cast<float>(k) + 1.f) / (static_cast<float>(ends.size()) + 1.f)
                                          : 0.5f;
            const Port port{OnSide(r, side, t), NormalOf(side)};
            (ends[k].source ? wires[ends[k].wire].pa : wires[ends[k].wire].pb) = port;
        }
    }

    WirePaths out;
    std::vector<std::size_t> needRoute;
    for (std::size_t w = 0; w < wires.size(); ++w) {
        const Wire& wire = wires[w];
        std::vector<Vec2> plain;
        switch (wire.line) {
        case EdgeLine::Straight: plain = {wire.pa.p, wire.pb.p}; break;
        case EdgeLine::Elbow: plain = PlainElbow(wire.pa, wire.pb); break;
        default: plain = Bezier(wire.pa, wire.pb); break;
        }
        const bool clear = !o.avoidNodes || PathClear(plain, frames, wire.a, wire.b, o.clearance * 0.4f);
        if (wire.line != EdgeLine::Elbow && clear) out.emplace(wire.edge->id, std::move(plain));
        else if (!o.avoidNodes) out.emplace(wire.edge->id, RoundCorners(plain, o.cornerRadius));
        else {
            out.emplace(wire.edge->id, std::move(plain));   // the fallback if routing finds no way
            needRoute.push_back(w);
        }
    }
    if (needRoute.empty()) return out;

    // One grid for every wire, so later wires see the lanes earlier ones took. Shorter wires go first: they have the
    // fewest ways around, and the long ones can afford the detour.
    const float stub = o.clearance + 2.f;
    std::vector<Rect>  obstacles;
    std::vector<float> xs, ys;
    Rect               all = frames.front();
    for (const Rect& f : frames) {
        const Rect r = f.Inflated(o.clearance);
        obstacles.push_back(r);
        xs.insert(xs.end(), {r.x, r.Right()});
        ys.insert(ys.end(), {r.y, r.Bottom()});
        all = Rect::Union(all, r);
    }
    {
        // The lane down the middle of each gap between neighbouring outlines.
        auto lanes = [](std::vector<float> c) {
            std::sort(c.begin(), c.end());
            std::vector<float> mids;
            for (std::size_t k = 1; k < c.size(); ++k)
                if (c[k] - c[k - 1] > 8.f) mids.push_back((c[k] + c[k - 1]) * 0.5f);
            return mids;
        };
        const auto mx = lanes(xs), my = lanes(ys);
        xs.insert(xs.end(), mx.begin(), mx.end());
        ys.insert(ys.end(), my.begin(), my.end());
    }
    const Rect ring = all.Inflated(o.clearance);
    xs.insert(xs.end(), {ring.x, ring.Right()});
    ys.insert(ys.end(), {ring.y, ring.Bottom()});
    for (std::size_t w : needRoute)
        for (const Port& p : {wires[w].pa, wires[w].pb}) {
            const Vec2 s = p.p + p.n * stub;
            xs.push_back(s.x);
            ys.push_back(s.y);
        }
    Grid grid(obstacles, std::move(xs), std::move(ys));

    std::stable_sort(needRoute.begin(), needRoute.end(), [&](std::size_t x, std::size_t y) {
        auto span = [&](std::size_t w) { return std::abs(wires[w].pa.p.x - wires[w].pb.p.x) + std::abs(wires[w].pa.p.y - wires[w].pb.p.y); };
        return span(x) < span(y);
    });
    for (std::size_t w : needRoute) {
        const Wire& wire = wires[w];
        const Vec2  s    = wire.pa.p + wire.pa.n * stub;
        const Vec2  t    = wire.pb.p + wire.pb.n * stub;
        std::vector<Vec2> mid = grid.Route(s, wire.pa.n, t, wire.pb.n * -1.f, o);
        if (mid.empty()) {
            if (wire.line == EdgeLine::Elbow) out[wire.edge->id] = RoundCorners(out[wire.edge->id], o.cornerRadius);
            continue;
        }
        std::vector<Vec2> path{wire.pa.p};
        path.insert(path.end(), mid.begin(), mid.end());
        path.push_back(wire.pb.p);
        path = Simplify(path);
        const float radius = wire.line == EdgeLine::Curve ? o.curveRadius : (wire.line == EdgeLine::Elbow ? o.cornerRadius : 0.f);
        out[wire.edge->id] = RoundCorners(path, radius);
    }
    return out;
}

} // namespace grapher
