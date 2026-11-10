#pragma once
/// @file
/// @brief GRAPHER's libavoid backend: wires routed by libavoid (adaptagrams, LGPL-2.1+, loaded as avoid.dll). Every wire
///        is routed orthogonally around the nodes; crossings cost far more than bends, so wires only cross when there
///        is no other way; parallel runs are nudged apart and ordered so they nest. Each node offers pins along all
///        four sides and libavoid picks the best free one, so ends spread without being told to. Routing is
///        incremental: the router lives between calls and only moves what changed.
/// @note A consumer ships avoid.dll and libavoid's LICENSE.LGPL (GRAPHER_AVOID_LICENSE names it) beside its program.

#include <grapher/Routing.h>

#include <memory>

namespace grapher::avoid {

class AvoidRouter final : public WireRouter {
public:
    AvoidRouter();
    ~AvoidRouter() override;
    AvoidRouter(const AvoidRouter&)            = delete;
    AvoidRouter& operator=(const AvoidRouter&) = delete;

    [[nodiscard]] WirePaths Route(const Graph& graph, const Schema& schema, const RoutingOptions& options) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief A fresh router for one canvas.
[[nodiscard]] std::shared_ptr<WireRouter> MakeRouter();

} // namespace grapher::avoid
