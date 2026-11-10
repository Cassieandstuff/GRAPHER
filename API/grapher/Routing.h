#pragma once
/// @file
/// @brief Wire routing: where each edge's line runs. Ends that share a node side spread along it, ordered so they don't
///        cross at the node; a wire whose plain shape would cut through another node is routed around the nodes
///        (orthogonal A* over a sparse grid of the nodes' outlines, preferring few bends and unused lanes) and drawn in
///        its type's style: an elbow keeps its corners (slightly rounded), a curve rounds them wide, a straight line
///        keeps them sharp. A curve or straight line that is already clear keeps its plain shape.
///        A group (NodeShape::Group) is no obstacle: wires run over it, though one can end on its outline.

#include <grapher/Graph.h>
#include <grapher/Schema.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace grapher {

struct RoutingOptions {
    bool  avoidNodes   = true;   ///< route around nodes; false = plain shapes (ports still spread)
    bool  spreadPorts  = true;   ///< spread the ends that share a side; false = every end at the side's middle
    float clearance    = 16.f;   ///< world units a routed wire keeps from the nodes it passes
    float bendCost     = 40.f;   ///< a bend costs as much as this much extra length
    float shareCost    = 24.f;   ///< running along a lane another wire already uses costs this much more per step
    /// @brief Crossing another wire, for backends that can weigh it (libavoid calls its version experimental and slow, and
    ///        it rarely buys a detour); 0 = off. Parallel runs are kept from crossing by nudging, not by this.
    float crossCost    = 0.f;
    float laneSpacing  = 8.f;    ///< the gap between parallel wires a backend spreads apart
    float cornerRadius = 10.f;   ///< an elbow's corners
    float curveRadius  = 40.f;   ///< a detouring curve's corners
    friend bool operator==(const RoutingOptions&, const RoutingOptions&) = default;
};

/// @brief World-space polylines, keyed by edge id.
using WirePaths = std::unordered_map<std::string, std::vector<Vec2>>;

/// @brief Route every edge that joins two distinct nodes of `graph` (a self-loop or a dangling edge gets no path).
/// @note Each path starts on the source node's outline and ends on the target's; the last segment points into the target,
///       so an arrowhead drawn along it lands on the node.
[[nodiscard]] WirePaths RouteWires(const Graph& graph, const Schema& schema, const RoutingOptions& options);

/// @brief A router a host can swap in for RouteWires (a backend over a routing library, e.g. grapher::avoid). It may keep
///        state between calls (incremental routing), so one instance serves one canvas.
class WireRouter {
public:
    virtual ~WireRouter() = default;
    /// @brief The same contract as RouteWires.
    [[nodiscard]] virtual WirePaths Route(const Graph& graph, const Schema& schema, const RoutingOptions& options) = 0;
};

} // namespace grapher
