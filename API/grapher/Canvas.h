#pragma once
/// @file
/// @brief The canvas: a camera over a Graph, the gestures that edit it (select, move, resize, connect, marquee, pan,
///        zoom) and the DrawList that shows it. UI-agnostic: a backend feeds it pointer events in screen pixels and
///        paints what Draw returns. The host owns the Graph and the undo history; the canvas calls `onBeforeChange`
///        right before it mutates the graph, so the host can snapshot it.

#include <grapher/DrawList.h>
#include <grapher/Graph.h>
#include <grapher/Routing.h>
#include <grapher/Schema.h>
#include <grapher/Style.h>

#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grapher {

struct Camera {
    Vec2  pan;          ///< screen position of the world origin
    float zoom = 1.f;

    [[nodiscard]] Vec2 ToScreen(Vec2 w) const { return {w.x * zoom + pan.x, w.y * zoom + pan.y}; }
    [[nodiscard]] Vec2 ToWorld(Vec2 s) const { return {(s.x - pan.x) / zoom, (s.y - pan.y) / zoom}; }
    [[nodiscard]] Rect ToScreen(const Rect& r) const { return {r.x * zoom + pan.x, r.y * zoom + pan.y, r.w * zoom, r.h * zoom}; }
    [[nodiscard]] Rect ToWorld(const Rect& r) const {
        const Vec2 p = ToWorld(Vec2{r.x, r.y});
        return {p.x, p.y, r.w / zoom, r.h / zoom};
    }
};

enum class Button { Left, Middle, Right };

struct Modifiers {
    bool shift = false;
    bool ctrl  = false;
    bool alt   = false;
};

enum class HitKind {
    None,
    Node,      ///< a node's body
    Resize,    ///< a node's resize grip
    Connect,   ///< a node's connect handle
    Edge,
};

struct Hit {
    HitKind     kind = HitKind::None;
    std::string id;   ///< the node or edge
};

using NodeSet = std::set<std::string, std::less<>>;

class Canvas {
public:
    Canvas(Graph& graph, const Schema& schema);

    /// @brief Edit another graph (drilling into a node); clears the selection and any gesture.
    void SetGraph(Graph& graph);
    [[nodiscard]] Graph&       GetGraph() { return *graph_; }
    [[nodiscard]] const Graph& GetGraph() const { return *graph_; }
    void SetSchema(const Schema& schema) { schema_ = &schema; }
    [[nodiscard]] const Schema& GetSchema() const { return *schema_; }

    Camera         camera;
    Style          style;
    RoutingOptions routing;
    float          snap = 10.f;   ///< world grid that moves and resizes land on; 0 = free

    std::function<void(std::string_view label)> onBeforeChange;   ///< right before the graph mutates
    std::function<void()>                       onChanged;        ///< after a mutation is complete
    std::function<void()>                       onSelectionChanged;

    void SetViewport(Vec2 size) { viewport_ = size; }
    [[nodiscard]] Vec2 Viewport() const { return viewport_; }

    /// @name Input, in screen pixels. Each returns true when the picture changed.
    /// @{
    bool PointerDown(Vec2 at, Button button, Modifiers mods);
    bool PointerMove(Vec2 at);
    bool PointerUp(Vec2 at, Button button);
    /// @param steps wheel notches; positive zooms in, about `at`
    bool Wheel(Vec2 at, float steps);
    /// @}

    [[nodiscard]] Hit  HitTest(Vec2 screen) const;
    [[nodiscard]] bool Busy() const { return mode_ != Mode::Idle; }

    /// @name Selection
    /// @{
    [[nodiscard]] const NodeSet&     SelectedNodes() const { return selNodes_; }
    [[nodiscard]] const std::string& SelectedEdge() const { return selEdge_; }
    void SelectNodes(NodeSet ids);
    void SelectEdge(std::string id);
    void ClearSelection();
    void SelectAll();
    /// @}

    /// @name Edits (each snapshots through onBeforeChange, then reports onChanged)
    /// @{
    /// @return the new node's id; empty when `type` is unknown
    std::string AddNode(std::string_view type, Vec2 worldCenter);
    /// @return false when nothing was selected
    bool DeleteSelection();
    /// @}

    /// @brief Fit every node in the viewport (zoom capped at 1).
    void Frame();
    [[nodiscard]] Rect ScreenRectOf(std::string_view nodeId) const;

    /// @brief The picture as it is now: grid, edges, nodes, selection, gesture feedback.
    [[nodiscard]] DrawList Draw() const;
    /// @brief The whole graph at `zoom` with `margin` pixels around it and no selection or grid: an export.
    [[nodiscard]] DrawList DrawAll(float zoom = 1.f, float margin = 40.f) const;

private:
    enum class Mode { Idle, Moving, Resizing, Connecting, Marquee, Panning };

    [[nodiscard]] DrawList Render(const Camera& cam, Vec2 viewport, bool chrome) const;
    /// @brief Every wire's world-space path, rerouted only when a node frame, an edge or the options changed. While a drag
    ///        is under way and a full route has been slow, wires take their plain shapes until the drop.
    [[nodiscard]] const WirePaths& Wires() const;
    [[nodiscard]] const NodeType& TypeOf(const Node& n) const;
    [[nodiscard]] float Snap(float v) const;
    void Changed();
    void SelectionChanged();
    void Before(std::string_view label);

    Graph*        graph_;
    const Schema* schema_;
    Vec2          viewport_{800.f, 600.f};

    NodeSet     selNodes_;
    std::string selEdge_;
    std::string hovered_;

    Mode        mode_ = Mode::Idle;
    bool        started_ = false;   ///< the gesture moved past the drag threshold (a click never snapshots)
    bool        additive_ = false;
    Vec2        downAt_;
    Vec2        pointer_;
    Vec2        panAtDown_;
    std::string active_;            ///< the node a resize or connect starts from
    std::string connectTo_;
    std::vector<std::pair<std::string, Rect>> origin_;   ///< frames at the start of a move or resize

    mutable WirePaths   wires_;
    mutable std::size_t wiresKey_   = 0;
    mutable bool        wiresValid_ = false;
    mutable double      routeMs_    = 0.0;   ///< how long the last full route took
};

} // namespace grapher
