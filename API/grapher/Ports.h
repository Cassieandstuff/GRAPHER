#pragma once
/// @file
/// @brief Where a node's ports sit, in world units: the one layout the canvas draws and hit-tests and every router ends
///        wires on. On a card, ports on the left and right stack in rows under the header (row k holds the k-th left
///        port and the k-th right port, as a material node's inputs and outputs pair up); a port `onHeader` sits on the
///        title line; ports on the top or bottom spread evenly along that side. On any other shape every side's ports
///        spread evenly along it.

#include <grapher/Graph.h>
#include <grapher/Schema.h>
#include <grapher/Style.h>

#include <string_view>
#include <vector>

namespace grapher {

/// @brief The parts of a Style a card's layout depends on, in world units.
struct CardMetrics {
    float padding   = 10.f;
    float titleSize = 15.f;
    float fontSize  = 12.f;
    float groupHeader = 30.f;   ///< a group's header strip
    friend bool operator==(const CardMetrics&, const CardMetrics&) = default;

    [[nodiscard]] float Top() const { return padding * 0.8f; }                         ///< the title line's top, from the frame's
    [[nodiscard]] float Header() const { return Top() + titleSize * 1.35f + fontSize * 1.5f; }   ///< where the rows start
    [[nodiscard]] float Row() const { return fontSize * 1.4f; }
};

[[nodiscard]] inline CardMetrics MetricsOf(const Style& s) { return {s.padding, s.titleSize, s.fontSize, s.groupHeader}; }

/// @brief One port placed on its node.
struct PortSlot {
    PortDef  def;
    PortSide side = PortSide::Left;   ///< resolved, never Auto
    Vec2     at;                      ///< the pin's centre, on the node's outline
    Vec2     normal;                  ///< outward, the way a wire leaves
    Rect     row;                     ///< a row port's line (its label goes there); empty otherwise
};

/// @return the node's ports: its type's, then its own (one of its own with a type port's id replaces that one in place)
[[nodiscard]] std::vector<PortDef> PortsOf(const Node& node, const Schema& schema);

/// @return every port of the node, placed
[[nodiscard]] std::vector<PortSlot> LayoutPorts(const Node& node, const Schema& schema, const CardMetrics& metrics);

/// @return the slot of port `id`, or nullptr
[[nodiscard]] const PortSlot* FindSlot(const std::vector<PortSlot>& slots, std::string_view id);

/// @return how many port rows a card of this node has (the longer of its left and right columns)
[[nodiscard]] int PortRows(const Node& node, const Schema& schema);

/// @return the height a card needs to show its header, its port rows and its on-card fields
[[nodiscard]] float CardHeight(const Node& node, const Schema& schema, const CardMetrics& metrics);

/// @brief The type port `port` of `node` carries now: its declared type, or, for a port with `typeFrom`, the type of
///        the port wired to that other port (followed through chains of reroutes), while it has a wire.
/// @return a PortType id; empty for an untyped port or an unknown one
[[nodiscard]] std::string PortTypeOf(const Graph& graph, const Schema& schema, const Node& node, std::string_view port);

} // namespace grapher
