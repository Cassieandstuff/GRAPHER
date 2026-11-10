#include <grapher/avoid/AvoidRouter.h>

#include <libavoid/libavoid.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace grapher::avoid {

namespace {

constexpr unsigned kAnySide = 1;   ///< the pin class every side's pins share: libavoid picks the best free one
constexpr int      kCubicSteps = 8;

struct SidePins {
    double            x0, y0, x1, y1;   ///< proportional ends of the side
    Avoid::ConnDirFlags dir;
};

/// @brief Pins along each side, away from the corners. Exclusive (libavoid's default), so two wires never share one.
void AddPins(Avoid::ShapeRef* shape)
{
    static const SidePins kSides[] = {
        {0.0, 0.0, 1.0, 0.0, Avoid::ConnDirUp},
        {0.0, 1.0, 1.0, 1.0, Avoid::ConnDirDown},
        {0.0, 0.0, 0.0, 1.0, Avoid::ConnDirLeft},
        {1.0, 0.0, 1.0, 1.0, Avoid::ConnDirRight},
    };
    static const double kAlong[] = {0.2, 0.35, 0.5, 0.65, 0.8};
    for (const SidePins& s : kSides)
        for (double t : kAlong) {
            const double x = s.x0 == s.x1 ? s.x0 : t;
            const double y = s.y0 == s.y1 ? s.y0 : t;
            new Avoid::ShapeConnectionPin(shape, kAnySide, x, y, true, 0.0, s.dir);
        }
}

Avoid::Rectangle RectOf(const Rect& r) { return Avoid::Rectangle(Avoid::Point(r.x, r.y), Avoid::Point(r.Right(), r.Bottom())); }

Avoid::ConnType TypeOf(EdgeLine line) { return line == EdgeLine::Straight ? Avoid::ConnType_PolyLine : Avoid::ConnType_Orthogonal; }

/// @brief A libavoid path (curvedPolyline's M/L/C tags, or none) as a polyline.
std::vector<Vec2> Flatten(const Avoid::Polygon& poly)
{
    std::vector<Vec2> out;
    auto at = [&](std::size_t i) { return Vec2{static_cast<float>(poly.ps[i].x), static_cast<float>(poly.ps[i].y)}; };
    if (poly.ts.size() != poly.ps.size()) {
        for (std::size_t i = 0; i < poly.ps.size(); ++i) out.push_back(at(i));
        return out;
    }
    for (std::size_t i = 0; i < poly.ps.size(); ++i) {
        if (poly.ts[i] == 'C' && i + 2 < poly.ps.size() && !out.empty()) {
            const Vec2 p0 = out.back(), c1 = at(i), c2 = at(i + 1), p3 = at(i + 2);
            for (int k = 1; k <= kCubicSteps; ++k) {
                const float t = static_cast<float>(k) / kCubicSteps, u = 1.f - t;
                out.push_back(p0 * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + p3 * (t * t * t));
            }
            i += 2;
        } else {
            out.push_back(at(i));
        }
    }
    return out;
}

} // namespace

struct AvoidRouter::Impl {
    struct Shape {
        Avoid::ShapeRef* ref = nullptr;
        Rect             frame;
    };
    struct Conn {
        Avoid::ConnRef* ref = nullptr;
        std::string     from, to;
        EdgeLine        line = EdgeLine::Curve;
    };

    std::unique_ptr<Avoid::Router>         router;
    std::unordered_map<std::string, Shape> shapes;
    std::unordered_map<std::string, Conn>  conns;
    RoutingOptions                         options;

    void Reset(const RoutingOptions& o)
    {
        conns.clear();
        shapes.clear();
        router.reset();   // deletes every shape, pin and connector it still holds
        router = std::make_unique<Avoid::Router>(Avoid::OrthogonalRouting | Avoid::PolyLineRouting);
        router->setRoutingParameter(Avoid::segmentPenalty, o.bendCost);
        if (o.crossCost > 0.f) router->setRoutingParameter(Avoid::crossingPenalty, o.crossCost);
        router->setRoutingParameter(Avoid::shapeBufferDistance, o.clearance * 0.75);
        router->setRoutingParameter(Avoid::idealNudgingDistance, o.laneSpacing);
        router->setRoutingOption(Avoid::nudgeOrthogonalSegmentsConnectedToShapes, true);
        router->setRoutingOption(Avoid::nudgeSharedPathsWithCommonEndPoint, true);
        router->setRoutingOption(Avoid::performUnifyingNudgingPreprocessingStep, true);
        options = o;
    }
};

AvoidRouter::AvoidRouter() : impl_(std::make_unique<Impl>()) {}
AvoidRouter::~AvoidRouter() = default;

WirePaths AvoidRouter::Route(const Graph& graph, const Schema& schema, const RoutingOptions& o)
{
    Impl& m = *impl_;
    if (!m.router || !(m.options == o)) m.Reset(o);
    Avoid::Router& router = *m.router;

    // Connectors that no longer exist, or whose ends changed, go first: a shape is never deleted under a connector.
    std::unordered_set<std::string> nodeIds;
    for (const Node& n : graph.nodes) nodeIds.insert(n.id);
    std::unordered_map<std::string, const Edge*> wanted;
    for (const Edge& e : graph.edges)
        if (e.from != e.to && nodeIds.contains(e.from) && nodeIds.contains(e.to)) wanted.emplace(e.id, &e);
    for (auto it = m.conns.begin(); it != m.conns.end();) {
        const auto w = wanted.find(it->first);
        if (w == wanted.end() || w->second->from != it->second.from || w->second->to != it->second.to) {
            router.deleteConnector(it->second.ref);
            it = m.conns.erase(it);
        } else {
            ++it;
        }
    }

    // Shapes: drop the gone, move the moved, add the new.
    for (auto it = m.shapes.begin(); it != m.shapes.end();) {
        if (!nodeIds.contains(it->first)) {
            router.deleteShape(it->second.ref);
            it = m.shapes.erase(it);
        } else {
            ++it;
        }
    }
    for (const Node& n : graph.nodes) {
        auto it = m.shapes.find(n.id);
        if (it == m.shapes.end()) {
            Avoid::Rectangle rect = RectOf(n.frame);
            auto*            ref  = new Avoid::ShapeRef(&router, rect);
            AddPins(ref);
            m.shapes.emplace(n.id, Impl::Shape{ref, n.frame});
        } else if (!(it->second.frame == n.frame)) {
            router.moveShape(it->second.ref, RectOf(n.frame));
            it->second.frame = n.frame;
        }
    }

    // Connectors: add the new, retype the changed.
    for (const auto& [id, e] : wanted) {
        const EdgeType* t    = schema.FindEdge(e->type);
        const EdgeLine  line = t ? t->line : EdgeLine::Curve;
        auto            it   = m.conns.find(id);
        if (it == m.conns.end()) {
            auto* ref = new Avoid::ConnRef(&router, Avoid::ConnEnd(m.shapes.at(e->from).ref, kAnySide),
                                           Avoid::ConnEnd(m.shapes.at(e->to).ref, kAnySide));
            ref->setRoutingType(TypeOf(line));
            m.conns.emplace(id, Impl::Conn{ref, e->from, e->to, line});
        } else if (it->second.line != line) {
            it->second.ref->setRoutingType(TypeOf(line));
            it->second.line = line;
        }
    }

    router.processTransaction();

    WirePaths out;
    for (auto& [id, c] : m.conns) {
        const Avoid::PolyLine& route = c.ref->displayRoute();
        if (route.ps.size() < 2) continue;
        switch (c.line) {
        case EdgeLine::Straight: out.emplace(id, Flatten(route)); break;
        case EdgeLine::Elbow: out.emplace(id, Flatten(route.curvedPolyline(o.cornerRadius))); break;
        default: out.emplace(id, Flatten(route.curvedPolyline(o.curveRadius))); break;
        }
    }
    return out;
}

std::shared_ptr<WireRouter> MakeRouter() { return std::make_shared<AvoidRouter>(); }

} // namespace grapher::avoid
