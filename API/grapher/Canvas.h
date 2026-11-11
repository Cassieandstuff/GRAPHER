#pragma once
/// @file
/// @brief The canvas: a camera over a Graph, the gestures that edit it (select, move, resize, connect, marquee, pan,
///        zoom) and the DrawList that shows it. UI-agnostic: a backend feeds it pointer events in screen pixels and
///        paints what Draw returns. The host owns the Graph and the undo history. In EditMode::Direct the canvas
///        calls `onBeforeChange` right before it mutates the graph, so the host can snapshot it. In EditMode::Propose
///        it never touches the host's graph: a gesture plays out on a copy, and the edit arrives as an Intent the host
///        applies (or refuses) through its own history, then calls Refresh.

#include <grapher/Dial.h>
#include <grapher/DrawList.h>
#include <grapher/Graph.h>
#include <grapher/Groups.h>
#include <grapher/Ports.h>
#include <grapher/Routing.h>
#include <grapher/Schema.h>
#include <grapher/Style.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <utility>
#include <variant>
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
    Connect,   ///< a node's connect handle (a node without ports)
    Edge,
    Port,      ///< a port's pin; Hit::port names it
    Row,       ///< one of a node's rows (Node::rows); Hit::row names it
    Handle,    ///< a dial's handle for one of its rows; Hit::row names it
};

struct Hit {
    HitKind     kind = HitKind::None;
    std::string id;     ///< the node or edge
    std::string port;   ///< Port: the port on node `id`
    std::string row;    ///< Row: the row of node `id`
};

using NodeSet = std::set<std::string, std::less<>>;

/// @brief What a host says of a wire about to be drawn (Canvas::checkWire).
struct WireCheck {
    bool        allowed = true;
    std::string type;    ///< an EdgeType id the wire takes instead; empty = as the schema says
    std::string label;   ///< the wire's label; empty = none
};

enum class EditMode {
    Direct,    ///< gestures edit the graph, between onBeforeChange and onChanged
    Propose,   ///< gestures play out on a copy and arrive as an Intent; the host's graph is never touched
};

/// @brief What Shift and Ctrl do to a click or a box select.
enum class SelectKeys {
    Toggle,           ///< either toggles a clicked node; either adds a box to the selection
    ExtendSubtract,   ///< Shift adds, Ctrl takes away (the Blender convention)
};

/// @name Intents: the edits a canvas in EditMode::Propose hands its host
/// @{
/// @brief Nodes that moved or were resized (a node dropped into a group slides in: its frame says where).
struct MoveNodes {
    std::vector<std::pair<std::string, Rect>> frames;   ///< every node whose frame changed, with its new frame
    /// @brief Nodes dropped into another member group (or out of all: an empty parent), with their new Node::parent.
    std::vector<std::pair<std::string, std::string>> parents;
    /// @brief Nodes docked, undocked (an empty Dock::parent) or reordered on their side, with their new Dock.
    std::vector<std::pair<std::string, Dock>> docks;
};

/// @brief Where a dragged node would dock: a parent, its side, the place on it, and the frame it would take there.
struct DockSlot {
    std::string parent;
    PortSide    side  = PortSide::Bottom;
    int         index = 0;
    Rect        at;
};
/// @brief A new wire, and the wires it replaces at a port that was full.
struct ConnectWire {
    Edge                     edge;
    std::vector<std::string> replaced;
};
/// @brief A wire dragged from a node or port and let go over empty canvas (the host may offer to make something there).
struct ConnectToNothing {
    std::string node;
    std::string port;   ///< empty: dragged from the node's connect handle
    Vec2        at;     ///< world point it was let go at
};
/// @brief Nodes and wires to remove: the nodes, and every wire going with them or selected.
struct RemoveItems {
    NodeSet                  nodes;
    std::vector<std::string> edges;
};
/// @brief A node to add, its id, type and frame chosen.
struct CreateNode {
    Node node;
};
/// @brief A choice picked on a row (RowAction::Choice): set `field` of `node` to `value`.
struct SetValue {
    std::string node, row, field, value;
};
struct Intent {
    std::string label;   ///< what an undo step would be called ("Move", "Connect")
    std::variant<MoveNodes, ConnectWire, ConnectToNothing, RemoveItems, CreateNode, SetValue> change;
};
/// @}

/// @brief An arrow drawn between two nodes for a while (a transition under way): a dashed arc bowing out of their left
///        sides (a loop when they are the same node), with a head at its middle that fills as `progress` goes 0..1, and a
///        flash when `flash` is above 0.
struct Arrow {
    std::string from, to;
    float       progress = 0.f;
    float       flash    = 0.f;   ///< 0..1
};

/// @brief What the host lights over the graph, without editing it: a glow behind nodes and wires (0..1; a closed group
///        glows with the brightest thing it holds, a merged wire with the brightest it stands for), coloured rings,
///        nodes dimmed, and arrows. None of it is an edit.
struct Highlights {
    std::unordered_map<std::string, float> glow;       ///< by node id
    std::unordered_map<std::string, float> edgeGlow;   ///< by edge id
    std::unordered_map<std::string, Color> rings;      ///< by node id
    NodeSet                                dimmed;
    std::vector<Arrow>                     arrows;
};

/// @brief A click on a row with an action, for the host (or a backend, for a Choice) to act on.
struct RowClick {
    std::string node, row;
    RowAction   action = RowAction::None;
    Vec2        at;   ///< screen point
};

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
    std::shared_ptr<WireRouter> router;   ///< a routing backend; null = the built-in RouteWires
    Highlights     highlights;   ///< what the host lights; set it and repaint
    float          snap = 10.f;   ///< world grid that moves and resizes land on; 0 = free
    EditMode       editMode   = EditMode::Direct;
    SelectKeys     selectKeys = SelectKeys::Toggle;
    float          minZoom = 0.1f;
    float          maxZoom = 4.f;
    /// @brief The middle and right buttons pan. False when the host navigates the view itself (PanBy, ZoomAbout).
    bool           panWithButtons = true;

    std::function<void(std::string_view label)> onBeforeChange;   ///< Direct: right before the graph mutates
    std::function<void()>                       onChanged;        ///< Direct: after a mutation is complete
    std::function<void()>                       onSelectionChanged;
    /// @brief Propose: every edit. Direct: only what the canvas can't do itself (ConnectToNothing).
    std::function<void(const Intent&)>          onIntent;
    /// @brief A member group opened or closed (view state, not an edit: a host may keep it, or lay the graph out again).
    std::function<void(std::string_view group, bool collapsed)> onCollapseChanged;
    /// @brief Wires may end on a node as a whole (a port-less node's connect handle). False: every wire runs port to
    ///        port, and port-less nodes (notes, frames) take none.
    bool wholeNodeWires = true;
    /// @brief The host's say on a wire, after the schema's: refuse it (a cycle), or mark it (a converting wire's own edge
    ///        type, a label). Unset: the schema alone decides.
    std::function<WireCheck(const Edge& wire)> checkWire;
    /// @brief A press on a full one-wire input lifts its wire: dropped on another input it moves there (a ConnectWire that
    ///        replaces it), dropped on nothing it comes off (RemoveItems). Off by default: then the press starts a new wire.
    bool liftWires = false;
    /// @brief A click on an alias (Node::aliasOf) selects the original and shows it, opening the groups around it.
    bool followAliases = true;
    /// @brief Dragging a node docks it to a slot beside a parent it may dock to, or undocks it (Node::dock). Off by
    ///        default: a host whose nodes dock turns it on.
    bool docking = false;
    /// @brief Whether `child` may dock to `parent`; unset = when a wire runs from the parent to the child.
    std::function<bool(const Node& child, const Node& parent)> canDock;

    /// @name Member groups, open and closed: the canvas's view state, never an edit
    /// @{
    void SetCollapsed(std::string_view group, bool collapsed);
    [[nodiscard]] bool IsCollapsed(std::string_view group) const { return collapsed_.contains(group); }
    [[nodiscard]] const NodeSet& CollapsedGroups() const { return collapsed_; }
    /// @brief Open or close every member group, or those `which` picks.
    void SetAllCollapsed(bool collapsed, const std::function<bool(const Node&)>& which = {});
    /// @brief Open every member group around the node, so it shows.
    void Reveal(std::string_view nodeId);
    /// @return whether the node is inside a closed member group
    [[nodiscard]] bool IsHidden(std::string_view nodeId) const;
    /// @return the node's frame as drawn (a member group's wraps what it holds), world units; empty when hidden or unknown
    [[nodiscard]] Rect FrameOf(std::string_view nodeId) const;
    /// @}
    /// @brief The slot a node dragged to `frame` would dock into, if any (`docking` on).
    [[nodiscard]] std::optional<DockSlot> FindDockSlot(std::string_view nodeId, const Rect& frame) const;
    /// @return whether the node shows now (Node::showWith)
    [[nodiscard]] bool Visible(const Node& node) const;
    /// @brief Keep the node's header where it is on screen through the next Refresh (the host laying the graph out again
    ///        after an edit), so the view doesn't jump.
    void HoldInPlace(std::string_view nodeId);

    /// @brief The host applied (or refused) what the canvas proposed: show its graph again. A no-op in Direct mode.
    void Refresh();
    /// @brief The host changed the graph itself (an undo, a live update, another panel's edit), perhaps mid-gesture: the
    ///        next frame checks every node and edge again instead of trusting that only the drag moved anything.
    void GraphChanged()
    {
        dragKey_ = 0;
        resync_  = true;
    }
    /// @return whether a proposal is showing (Propose mode, before Refresh)
    [[nodiscard]] bool Proposing() const { return graph_ != host_; }
    /// @brief Stop the gesture under way (Esc): a move or resize goes back where it started, a wire or box is dropped.
    void CancelGesture();

    void SetViewport(Vec2 size) { viewport_ = size; }
    [[nodiscard]] Vec2 Viewport() const { return viewport_; }

    /// @name Input, in screen pixels. Each returns true when the picture changed.
    /// @{
    bool PointerDown(Vec2 at, Button button, Modifiers mods);
    /// @param mods held while moving (Shift snaps a dial's handle)
    bool PointerMove(Vec2 at, Modifiers mods = {});
    bool PointerUp(Vec2 at, Button button);
    /// @param steps wheel notches; positive zooms in, about `at`
    bool Wheel(Vec2 at, float steps);
    /// @}

    /// @brief What is under a screen point, topmost first: nodes, then edges, then groups. A group is hit by its
    ///        header strip, its resize grip and its connect handle; its empty interior is empty canvas (a press there
    ///        starts a marquee, a right-click adds a node into it).
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
    /// @note A node added with its centre in a group's frame lands wholly inside it (see the move gesture).
    std::string AddNode(std::string_view type, Vec2 worldCenter);
    /// @return false when nothing was selected
    bool DeleteSelection();
    /// @brief Wire two ports (or a node as a whole, with an empty port id), in either order: the Out end becomes the
    ///        edge's `from`. A port already at its MaxWires loses its oldest wire to the new one. The edge type is the
    ///        Out port type's `edge`, else Schema::EdgeFor.
    /// @return the new edge's id; empty when CanWire refuses
    std::string Connect(std::string_view nodeA, std::string_view portA, std::string_view nodeB, std::string_view portB);
    /// @}

    /// @brief Whether Connect would wire these: two distinct nodes; with ports, one In and one Out whose types the schema
    ///        lets connect (Schema::CanConnect); and no edge between the same ends already.
    [[nodiscard]] bool CanWire(std::string_view nodeA, std::string_view portA, std::string_view nodeB, std::string_view portB) const;
    /// @return the pin of `port` on `nodeId` in screen pixels, or nullopt when there is none
    [[nodiscard]] std::optional<Vec2> PortScreenPos(std::string_view nodeId, std::string_view port) const;

    /// @brief Fit every node in the viewport (zoom capped at 1).
    void Frame();
    /// @brief Fit `ids` (every node when empty) with `margin` pixels around them, zoom no closer than `zoomCap`.
    void Frame(const NodeSet& ids, float margin = 40.f, float zoomCap = 1.f);
    /// @brief Put the node's centre in the middle of the view, at `zoom` when given.
    void CenterOn(std::string_view nodeId, std::optional<float> zoom = std::nullopt);
    /// @brief Pan as little as it takes to show the whole node with `margin` pixels around it (as much as fits).
    void EnsureVisible(std::string_view nodeId, float margin = 60.f);
    /// @brief Move the view by `pixels` (the content follows the hand: right and down positive).
    void PanBy(Vec2 pixels);
    /// @brief Scale the view by `factor` (> 1 closer), keeping `anchor` (screen) where it is, within minZoom and maxZoom.
    /// @return false when the zoom was already at its limit
    bool ZoomAbout(float factor, Vec2 anchor);
    [[nodiscard]] Rect ScreenRectOf(std::string_view nodeId) const;
    /// @return the middle of the node's title line in screen pixels, or nullopt
    [[nodiscard]] std::optional<Vec2> HeaderScreenPos(std::string_view nodeId) const;
    /// @return the line of row `row` of the node in screen pixels, or nullopt
    [[nodiscard]] std::optional<Rect> RowScreenRect(std::string_view nodeId, std::string_view row) const;
    /// @return what to show over a screen point: a row's tooltip, else its node's; a port's label and type; an edge's label
    [[nodiscard]] std::string TooltipAt(Vec2 screen) const;

    /// @brief The last click on a row with an action, once: a backend acts on it after PointerUp.
    [[nodiscard]] std::optional<RowClick> TakeRowClick();
    /// @brief Pick `value` on a Choice row: sets its field (Direct) or proposes it (Propose).
    /// @return false when the row isn't a Choice with a field
    bool PickChoice(std::string_view nodeId, std::string_view row, std::string_view value);

    /// @brief The picture as it is now: grid, edges, nodes, selection, gesture feedback.
    [[nodiscard]] DrawList Draw() const;
    /// @brief The whole graph drawn small into `size` pixels: groups, nodes (selected and ringed ones in their colours) and
    ///        the part in view. When the graph is too tall or wide to read at that size, it keeps a readable scale and
    ///        follows the view.
    [[nodiscard]] DrawList DrawMinimap(Vec2 size) const;
    /// @return the world point a minimap of `size` shows at `at`
    [[nodiscard]] Vec2 MinimapToWorld(Vec2 at, Vec2 size) const;
    /// @brief Put a world point in the middle of the view.
    void CenterOnWorld(Vec2 world);
    /// @brief The whole graph at `zoom` with `margin` pixels around it and no selection or grid: an export.
    [[nodiscard]] DrawList DrawAll(float zoom = 1.f, float margin = 40.f) const;

private:
    enum class Mode { Idle, Moving, Resizing, Connecting, Marquee, Panning, RowPress, DialDrag };

    [[nodiscard]] DrawList Render(const Camera& cam, Vec2 viewport, bool chrome) const;
    /// @param groupInterior a group's empty interior counts as the group (a connection's target)
    [[nodiscard]] Hit HitAt(Vec2 screen, bool groupInterior) const;
    /// @brief Each of `roots` whose centre lies in a group's frame but sticks out of it slides wholly inside, below the
    ///        header, when it fits; a group slides with what it holds. Groups in `moving` are no target.
    void Settle(const NodeSet& roots, const NodeSet& moving);
    /// @brief Every wire's world-space path, rerouted only when a node frame, an edge or the options changed.
    /// @note A backend router keeps routing live during a drag while its incremental reroute is quick. When it is slow, the
    ///       wires that touch no dragged node keep their routed paths, the rest take the built-in router's, and the drop
    ///       reroutes everything through the backend. Without a backend, a slow built-in route drags with plain shapes.
    [[nodiscard]] const WirePaths& Wires() const;
    [[nodiscard]] const NodeType& TypeOf(const Node& n) const;
    [[nodiscard]] std::vector<PortSlot> SlotsOf(const Node& n) const;
    /// @brief The port of `to` a wire dragged from `port` of `from` lands on when dropped on `to` as a whole: the first
    ///        that can take it, preferring one with room. Empty when none can.
    [[nodiscard]] std::string LandingPort(const Node& from, std::string_view port, const Node& to) const;
    /// @brief CanWire, not counting edge `ignoring` (one about to be replaced).
    [[nodiscard]] bool CanWire(std::string_view a, std::string_view pa, std::string_view b, std::string_view pb, std::string_view ignoring) const;
    /// @brief Connect, with `lifted` (an edge in the hand, or empty) taken out first and named among what it replaces.
    std::string Wire(std::string_view a, std::string_view pa, std::string_view b, std::string_view pb, std::string_view lifted);
    [[nodiscard]] float Snap(float v) const;
    void Changed();
    /// @brief The end of an edit: Direct reports onChanged, Propose hands `intent` to onIntent.
    void Changed(Intent intent);
    void SelectionChanged();
    void Before(std::string_view label);
    /// @brief Gather what a press on a node starts moving: the selection, and what its groups hold.
    void StartMove();
    /// @brief Propose: the frames on the copy that differ from the host's.
    [[nodiscard]] MoveNodes MovedFrames() const;

    Graph*        graph_;   ///< what the canvas shows and edits: the host's graph, or the copy a proposal plays out on
    Graph*        host_;
    Graph         scratch_;
    const Schema* schema_;
    Vec2          viewport_{800.f, 600.f};

    NodeSet     selNodes_;
    std::string selEdge_;
    std::string hovered_;

    Mode        mode_ = Mode::Idle;
    bool        started_ = false;   ///< the gesture moved past the drag threshold (a click never snapshots)
    bool        additive_ = false;
    bool        subtract_ = false;   ///< a box select that takes away
    std::string pressRow_;           ///< the row a RowPress started on
    std::optional<RowClick> rowClick_;
    Vec2        downAt_;
    Vec2        pointer_;
    Vec2        panAtDown_;
    std::string active_;            ///< the node a resize or connect starts from
    std::string connectTo_;
    std::string activePort_;        ///< the port a connect starts from; empty = the node's connect handle
    std::string connectPort_;       ///< the port of connectTo_ it would end on
    std::string lifted_;            ///< the wire a connect carries off its input (liftWires)
    std::vector<std::pair<std::string, Rect>> origin_;   ///< frames at the start of a move or resize
    NodeSet     carried_;           ///< moving because a selected group holds them, not because they are selected

    /// @brief What is drawn and hit: the graph itself, or, when it has member groups, a copy with their frames fitted,
    ///        what closed groups hold left out, and the wires to it ending on the closed group (merged).
    [[nodiscard]] const Graph& Shown() const { return useView_ ? view_ : *graph_; }
    /// @brief Fill `view_` from the graph (see Shown).
    void BuildView() const;
    /// @brief Place `view_`'s docked nodes against their parents (and what docked member groups hold with them).
    /// @brief Index `view_`'s docks: each node's dock parent, and each parent's lists by side and beside its rows.
    void IndexDocks() const;
    /// @brief Place docked nodes against their parents, carrying what docked member groups hold: the clusters of `roots`,
    ///        or every one. Nodes marked `dragged` are out of every list.
    /// @param moved collects the nodes it moved (a drag reroutes their wires)
    void PlaceDocks(const std::vector<char>& dragged, const std::vector<std::size_t>& roots, std::vector<std::size_t>* moved) const;
    void DrawDial(std::vector<DrawCmd>& c, const Node& n, const NodeType& t, const Camera& cam, bool sel, bool hov) const;
    /// @brief Fit every open member group of `view_` around what it shows, deepest first.
    void FitView() const;
    /// @brief Bring the per-graph caches up to date (rerouting when a frame, a port, an edge or the options changed):
    ///        the wires, each node's type and group flag, each edge's path and bounds, the wired ports, the id index.
    void Sync() const;
    /// @brief Sync unless nothing could have changed since the last one (the same graph, counts and canvas edits): a
    ///        hit test runs on the picture last drawn.
    void SyncForHit() const;
    /// @brief Point edge `i`'s table entries at its wire (or none).
    void SetPath(std::size_t i) const;
    [[nodiscard]] const Node* NodeById(std::string_view id) const;
    /// @brief The host's own node for a shown one: the view copies only places, so content (fields, rows, images) is
    ///        read live from the graph.
    [[nodiscard]] const Node* LiveOf(const Node& shown) const;
    /// @brief The node as drawn (a member group's frame fitted), after a Sync; nullptr when hidden or unknown.
    [[nodiscard]] const Node* ShownNode(std::string_view id) const;

    mutable WirePaths   wires_;
    mutable std::size_t wiresKey_   = 0;
    mutable bool        wiresValid_ = false;
    mutable std::vector<char>                     grouped_;    ///< per node: its type is a group
    mutable std::vector<const std::vector<Vec2>*> paths_;      ///< per edge: its wire, or null
    mutable std::vector<Rect>                     boxes_;      ///< per edge: its wire's bounds, world units
    mutable std::unordered_set<std::string>       wiredPorts_; ///< "node\x1fport" for every port a wire uses
    mutable std::unordered_map<std::string_view, std::size_t> index_;   ///< node id -> index
    mutable std::vector<std::vector<std::uint32_t>> incident_;   ///< per shown node: the shown edges touching it
    mutable std::size_t                             dragKey_ = 0;   ///< the cache key a large graph's drag began with
    mutable bool                                    resync_  = false;   ///< the host changed the graph: route it all once
    mutable bool                                  useView_ = false;
    mutable std::unordered_map<std::string_view, std::size_t> graphAt_;   ///< node id -> index in the graph (with a view)
    mutable Graph                                 view_;
    mutable std::vector<char>                     dashed_;    ///< per shown edge: it stands for wires into a closed group
    mutable std::vector<int>                      count_;     ///< per shown node: a group's nodes, nested ones included
    NodeSet                                       collapsed_;
    std::size_t                                   viewRev_ = 0;   ///< bumped when a group opens or closes
    std::vector<std::pair<std::string, Rect>>     dropTargets_;   ///< member groups a move may drop into, as they were
    std::string                                   pressed_;       ///< the node a press landed on
    float                                         dialValue_ = 0.f;   ///< a dial handle's value while it drags
    bool                                          snapDial_  = false;
    mutable bool                                  hasDocks_  = false;
    mutable bool                                  hasMembers_ = false;
    mutable std::unordered_map<std::string, std::string> shownAs_;      ///< a hidden node -> the closed group showing it
    mutable std::unordered_map<std::string, std::string> edgeShownAs_;  ///< a merged or remapped edge -> the one drawn
    /// @brief The minimap's scale and corner for `size`: the world point at its origin, and pixels per world unit.
    [[nodiscard]] std::pair<Vec2, float> MinimapFrame(Vec2 size) const;
    void DrawArrow(std::vector<DrawCmd>& c, const Arrow& a, const Camera& cam) const;
    mutable std::vector<std::size_t>              dockParent_;   ///< per shown node: its dock parent's index
    mutable std::unordered_map<std::size_t, std::array<std::vector<std::size_t>, 4>> dockSide_;   ///< by parent: bottom, top, right, left
    mutable std::unordered_map<std::size_t, std::vector<std::size_t>> dockBeside_;   ///< by parent: what sits beside its rows
    mutable std::vector<Vec2>                     offset_;       ///< PlaceDocks' working offsets
    mutable std::vector<char>                     visiting_;
    mutable std::unordered_map<std::string, std::uint8_t> sides_;   ///< each wire's last sides, so they don't flicker
    std::string                                   hold_;          ///< HoldInPlace's node
    Vec2                                          holdAt_;
    mutable std::vector<std::size_t>              viewUp_;     ///< per shown node: the shown group holding it
    mutable std::vector<std::vector<std::size_t>> viewKids_;   ///< per shown node: what it holds and shows
    mutable std::vector<std::size_t>              fitOrder_;   ///< the shown open member groups, deepest first
    mutable std::size_t  syncedNodes_ = 0, syncedEdges_ = 0;
    mutable const Graph* synced_     = nullptr;
    mutable std::size_t  syncedEdits_ = 0;
    std::size_t          edits_       = 0;   ///< bumped by every edit the canvas makes
    mutable double      routeMs_    = 0.0;   ///< how long the last built-in route around the nodes took
    mutable WirePaths   routed_;             ///< the backend's last paths
    mutable std::size_t backendKey_ = 0;     ///< the router and options the backend last routed with
    mutable double      backendMs_  = -1.0;  ///< how long the backend's last incremental route took; < 0 = not yet known
};

} // namespace grapher
