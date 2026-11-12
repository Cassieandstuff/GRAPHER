#pragma once
/// @file
/// @brief Where a node's ports sit, in world units: the one layout the canvas draws and hit-tests and every router ends
///        wires on. On a card, ports on the left and right stack in rows under the header (row k holds the k-th left
///        port and the k-th right port, as a material node's inputs and outputs pair up); a port `onHeader` sits on the
///        title line; ports on the top or bottom spread evenly along that side. A port a Row claims (Row::port) sits
///        on that row's line instead. On any other shape every side's ports spread evenly along it.

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
    float header = 0.f;   ///< where the rows start, stated outright (a kit's header height); 0 = from the sizes
    float row    = 0.f;   ///< a row's height, stated outright; 0 = from the font size
    /// @brief A dial's rim: below 0 a circle filling the frame (the default); else an octagon whose corners are cut by
    ///        this fraction of its half-width, and the node is that shape (a header on its top edge, its other rows
    ///        listed under it, the rim below). A kit's rim part sets it (`"shape": "octagon"`, `"chamfer"`).
    float dialChamfer = -1.f;
    friend bool operator==(const CardMetrics&, const CardMetrics&) = default;

    [[nodiscard]] float Top() const { return padding * 0.8f; }                         ///< the title line's top, from the frame's
    [[nodiscard]] float Header() const { return header > 0.f ? header : Top() + titleSize * 1.35f + fontSize * 1.5f; }   ///< where the rows start
    [[nodiscard]] float Row() const { return row > 0.f ? row : fontSize * 1.4f; }
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
/// @brief The same, the node at `frame` (where it is drawn) instead of its own.
[[nodiscard]] std::vector<PortSlot> LayoutPorts(const Node& node, const Schema& schema, const CardMetrics& metrics, const Rect& frame);

/// @return the slot of port `id`, or nullptr
[[nodiscard]] const PortSlot* FindSlot(const std::vector<PortSlot>& slots, std::string_view id);

/// @return how many rows a card gives the ports no Row claims (the longer of its left and right columns); the
///        node's own rows (Node::rows) follow them
[[nodiscard]] int PortRows(const Node& node, const Schema& schema);

/// @return the index of the Row that claims port `port` (Row::port), or -1
[[nodiscard]] int RowOfPort(const Node& node, std::string_view port);

/// @return where a card's own rows (Node::rows) start, world units
[[nodiscard]] float RowsTop(const Node& node, const Schema& schema, const CardMetrics& metrics);
[[nodiscard]] float RowsTop(const Node& node, const Schema& schema, const CardMetrics& metrics, const Rect& frame);

/// @return the line of row `index` of Node::rows, world units
[[nodiscard]] Rect RowRect(const Node& node, const Schema& schema, const CardMetrics& metrics, std::size_t index);
[[nodiscard]] Rect RowRect(const Node& node, const Schema& schema, const CardMetrics& metrics, std::size_t index, const Rect& frame);

/// @return the height a card needs to show its header, its port rows, its own rows, its on-card fields and its image
[[nodiscard]] float CardHeight(const Node& node, const Schema& schema, const CardMetrics& metrics);

/// @brief The type port `port` of `node` carries now: its declared type, or, for a port with `typeFrom`, the type of
///        the port wired to that other port (followed through chains of reroutes), while it has a wire.
/// @return a PortType id; empty for an untyped port or an unknown one
[[nodiscard]] std::string PortTypeOf(const Graph& graph, const Schema& schema, const Node& node, std::string_view port);

} // namespace grapher
