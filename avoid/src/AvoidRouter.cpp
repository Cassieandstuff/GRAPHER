#include <grapher/avoid/AvoidRouter.h>

#include <grapher/Groups.h>
#include <grapher/Ports.h>

#include <libavoid/libavoid.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace grapher::avoid {

namespace {

constexpr unsigned kAnySide = 1;   ///< the pin class every side's pins share: libavoid picks the best free one
constexpr unsigned kFirstPort = 2;  ///< a port's pins are class kFirstPort + its index among the node's ports
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

Avoid::ConnDirFlags DirOf(PortSide s)
{
    switch (s) {
    case PortSide::Left: return Avoid::ConnDirLeft;
    case PortSide::Right: return Avoid::ConnDirRight;
    case PortSide::Top: return Avoid::ConnDirUp;
    default: return Avoid::ConnDirDown;
    }
}

/// @brief One port's pin, as libavoid takes it: offsets from the shape's top-left, the far edge as ATTACH_POS_MAX_OFFSET.
struct PortPin {
    unsigned            cls = 0;
    double              x = 0, y = 0;
    Avoid::ConnDirFlags dir = Avoid::ConnDirNone;
    std::string         port;
    bool operator==(const PortPin&) const = default;
};

std::vector<PortPin> PortPins(const Node& n, const Schema& schema, const CardMetrics& m)
{
    std::vector<PortPin> out;
    const auto slots = LayoutPorts(n, schema, m);
    for (std::size_t k = 0; k < slots.size(); ++k) {
        const PortSlot& s = slots[k];
        PortPin p{kFirstPort + static_cast<unsigned>(k), s.at.x - n.frame.x, s.at.y - n.frame.y, DirOf(s.side), s.def.id};
        if (s.side == PortSide::Left) p.x = Avoid::ATTACH_POS_MIN_OFFSET;
        if (s.side == PortSide::Right) p.x = Avoid::ATTACH_POS_MAX_OFFSET;
        if (s.side == PortSide::Top) p.y = Avoid::ATTACH_POS_MIN_OFFSET;
        if (s.side == PortSide::Bottom) p.y = Avoid::ATTACH_POS_MAX_OFFSET;
        out.push_back(std::move(p));
    }
    return out;
}

/// @brief A port's pins carry any number of wires (an output feeds many inputs), so they are not exclusive.
void AddPortPins(Avoid::ShapeRef* shape, const std::vector<PortPin>& pins)
{
    for (const PortPin& p : pins) new Avoid::ShapeConnectionPin(shape, p.cls, p.x, p.y, false, 0.0, p.dir);
}

const PortPin* FindPin(const std::vector<PortPin>& pins, std::string_view port)
{
    for (const PortPin& p : pins)
        if (p.port == port) return &p;
    return nullptr;
}

/// @brief Where a wire meets a group, which is no obstacle and so has no shape or pins: the middle of the group's side
///        that faces the wire's other end, leaving outward.
Avoid::ConnEnd GroupEnd(const Rect& group, const Rect& other, Avoid::Point& at)
{
    const Vec2 d = other.Center() - group.Center();
    Avoid::ConnDirFlags dir;
    if (std::abs(d.x) * (group.h + other.h) >= std::abs(d.y) * (group.w + other.w)) {
        at  = Avoid::Point(d.x >= 0 ? group.Right() : group.x, group.y + group.h * 0.5);
        dir = d.x >= 0 ? Avoid::ConnDirRight : Avoid::ConnDirLeft;
    } else {
        at  = Avoid::Point(group.x + group.w * 0.5, d.y >= 0 ? group.Bottom() : group.y);
        dir = d.y >= 0 ? Avoid::ConnDirDown : Avoid::ConnDirUp;
    }
    return Avoid::ConnEnd(at, dir);
}

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
        Avoid::ShapeRef*     ref = nullptr;
        Rect                 frame;
        std::vector<PortPin> pins;   ///< its ports' pins, offsets from its top-left: a change rebuilds the shape
    };
    struct Conn {
        Avoid::ConnRef* ref = nullptr;
        std::string     from, to;
        std::string     fromPort, toPort;
        EdgeLine        line = EdgeLine::Curve;
        bool            groupEnd[2] = {false, false};   ///< the source / target end is a group's side, not a shape
        Avoid::Point    at[2];                         ///< where a group end sits
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

    // Groups are no obstacles (wires run over them to what they hold), so they get no shape.
    std::unordered_map<std::string_view, const Node*> nodes;
    std::unordered_set<std::string_view>              groups;
    for (const Node& n : graph.nodes) {
        nodes.emplace(n.id, &n);
        if (IsGroup(n, schema)) groups.insert(n.id);
    }

    // Each node's port pins. A shape whose pins moved (its ports changed, or a resize moved them) is rebuilt.
    std::unordered_map<std::string_view, std::vector<PortPin>> pins;
    std::unordered_set<std::string_view>                       rebuild;
    for (const Node& n : graph.nodes) {
        auto p = PortPins(n, schema, o.CardFor(n));
        if (const auto it = m.shapes.find(n.id); it != m.shapes.end() && it->second.pins != p) rebuild.insert(n.id);
        pins.emplace(n.id, std::move(p));
    }

    // Connectors that no longer exist, or whose ends changed, go first: a shape is never deleted under a connector.
    std::unordered_map<std::string, const Edge*> wanted;
    for (const Edge& e : graph.edges)
        if (e.from != e.to && nodes.contains(e.from) && nodes.contains(e.to)) wanted.emplace(e.id, &e);
    for (auto it = m.conns.begin(); it != m.conns.end();) {
        const auto w = wanted.find(it->first);
        if (w == wanted.end() || w->second->from != it->second.from || w->second->to != it->second.to ||
            w->second->fromPort != it->second.fromPort || w->second->toPort != it->second.toPort ||
            rebuild.contains(it->second.from) || rebuild.contains(it->second.to) ||
            groups.contains(it->second.from) != it->second.groupEnd[0] || groups.contains(it->second.to) != it->second.groupEnd[1]) {
            router.deleteConnector(it->second.ref);
            it = m.conns.erase(it);
        } else {
            ++it;
        }
    }

    // Shapes: drop the gone (and the nodes that became groups), move the moved, add the new.
    for (auto it = m.shapes.begin(); it != m.shapes.end();) {
        if (!nodes.contains(it->first) || groups.contains(it->first) || rebuild.contains(it->first)) {
            router.deleteShape(it->second.ref);
            it = m.shapes.erase(it);
        } else {
            ++it;
        }
    }
    for (const Node& n : graph.nodes) {
        if (groups.contains(n.id)) continue;
        auto it = m.shapes.find(n.id);
        if (it == m.shapes.end()) {
            Avoid::Rectangle rect = RectOf(n.frame);
            auto*            ref  = new Avoid::ShapeRef(&router, rect);
            AddPins(ref);
            AddPortPins(ref, pins.at(n.id));
            m.shapes.emplace(n.id, Impl::Shape{ref, n.frame, pins.at(n.id)});
        } else if (!(it->second.frame == n.frame)) {
            router.moveShape(it->second.ref, RectOf(n.frame));
            it->second.frame = n.frame;
        }
    }

    // Connectors: add the new, retype the changed, follow the group ends that moved.
    for (const auto& [id, e] : wanted) {
        const EdgeLine    line = o.LineFor(e->type, schema);
        const std::string ends[2]  = {e->from, e->to};
        const std::string ports[2] = {e->fromPort, e->toPort};
        Avoid::Point      at[2];
        auto endOf = [&](int k) {
            const std::string& self = ends[k];
            const PortPin*     pin  = ports[k].empty() ? nullptr : FindPin(pins.at(self), ports[k]);
            if (!groups.contains(self)) return Avoid::ConnEnd(m.shapes.at(self).ref, pin ? pin->cls : kAnySide);
            if (pin) {
                // A group has no shape: its port is a fixed point on its outline.
                const auto slots = LayoutPorts(*nodes.at(self), schema, o.CardFor(*nodes.at(self)));
                if (const PortSlot* s = FindSlot(slots, ports[k])) {
                    at[k] = Avoid::Point(s->at.x, s->at.y);
                    return Avoid::ConnEnd(at[k], pin->dir);
                }
            }
            return GroupEnd(nodes.at(self)->frame, nodes.at(ends[1 - k])->frame, at[k]);
        };
        auto it = m.conns.find(id);
        if (it == m.conns.end()) {
            const Avoid::ConnEnd src = endOf(0), dst = endOf(1);
            auto* ref = new Avoid::ConnRef(&router, src, dst);
            ref->setRoutingType(TypeOf(line));
            Impl::Conn c{ref, e->from, e->to, e->fromPort, e->toPort, line, {groups.contains(e->from), groups.contains(e->to)}, {at[0], at[1]}};
            m.conns.emplace(id, c);
            continue;
        }
        Impl::Conn& c = it->second;
        if (c.line != line) {
            c.ref->setRoutingType(TypeOf(line));
            c.line = line;
        }
        for (int k = 0; k < 2; ++k) {
            if (!c.groupEnd[k]) continue;
            const Avoid::ConnEnd end = endOf(k);
            if (at[k] == c.at[k]) continue;
            c.at[k] = at[k];
            if (k == 0) c.ref->setSourceEndpoint(end);
            else c.ref->setDestEndpoint(end);
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
