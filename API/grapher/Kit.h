#pragma once
/// @file
/// @brief Kits: what a host's nodes are made of and how they look, as data. A kit holds named colours (tokens), node
///        kinds (each a small tree of parts: a header, a body, an accent strip, text lines) and rules that patch parts
///        by selector: a node's kind, its tags (Node::tags, opaque to GRAPHER) and its state (selected, hovered…).
///        The canvas resolves a node's look once per (kind, tags, state) and paints from it. A canvas with no kit draws
///        exactly as before: its Schema and Style are turned into the kit that reproduces them (KitFromSchema).

#include <grapher/Geometry.h>
#include <grapher/Ports.h>
#include <grapher/Schema.h>
#include <grapher/Style.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grapher {

/// @brief The parts a kit composes. The set is closed: a new kind of part is a GRAPHER feature.
enum class PartKind {
    Card,     ///< a node kind's root: its fill and outline
    Header,   ///< a band at the top holding text lines (the title, a subtitle)
    Body,     ///< what follows the header: port rows, rows, fields, the body text, a picture
    Text,     ///< one line of text, in a header
    Accent,   ///< a strip down the card's left side
    /// @brief Pins a node shows besides its ports: a closed group's `ends` (`pin` names their pin kind).
    Pins,
    Badge, Button,   ///< parsed and kept; drawn from a later milestone
    Chip,    ///< a pill's text on a rounded tint
    Bar,     ///< a strip: a pill's leading bar, a dial's needle
    Glyph,   ///< a pin-like mark: a pill's event diamond, a dial's handles
    Image,   ///< parsed and kept
    Rim,     ///< a dial's ring; its parts are the `needle` (bar), the `handle` (glyph), the `label` and `reading` (text)
    Stub,    ///< a short wire with an arrowhead out of a pill's end
};

/// @brief A part as a kit states it. Every look field is optional, so a rule's patch sets only what it names.
struct PartSpec {
    PartKind    kind = PartKind::Body;
    std::string id;   ///< unique in its kind; rules address parts by it

    std::optional<bool>        visible;
    std::optional<std::string> fill, stroke, color;   ///< colour expressions: `#rrggbb[aa]`, `$token`, `band`, `none`,
                                                       ///< `lighter(c, 125)`, `darker(c, 130)` (Qt's percentages), `alpha(c, 0.5)`
    std::optional<float>       strokeWidth, corner, opacity;
    std::optional<bool>        dash;
    std::optional<float>       width;    ///< an accent's
    std::optional<float>       top;      ///< a header's first line, in paddings from the card's top
    std::optional<float>       height;   ///< a header's, world units; unset = what its lines take

    // A text line:
    std::optional<std::string> from;          ///< `field:<id>`, `row:<id>` (its label; the row stays in the body), `type`
    std::optional<std::string> placeholder;   ///< shown when it is empty; `{type}` is the node type's label
    std::optional<std::string> placeholderColor;
    std::optional<std::string> font;          ///< a font of the kit, under this line's own settings
    std::optional<float>       size;          ///< world units
    std::optional<float>       scale;         ///< drawn at size × scale
    std::optional<float>       box;           ///< its line box, in sizes
    std::optional<float>       advance;       ///< how far the next line starts below it, in sizes
    std::optional<bool>        bold, italic;
    std::optional<std::string> align;         ///< left, center, right

    std::vector<PartSpec>                           content;   ///< child parts, in order
    std::string                                     condition; ///< `if`: a selector the node must match to show it
    std::map<std::string, std::string, std::less<>> on;        ///< gesture -> action id (acted on from K2)
    std::string                                     editor;    ///< from K2

    // From K3:
    std::optional<std::string> shape;       ///< the root's when its group is closed: box (a header-high frame), pill
    /// @brief A text's template over its value: `{value}`, `{type}` (the type's label), `{count}` (what a group holds),
    ///        `{arrow}` (a collapsible group's ▾ or ▸, with a space), `{fields}` (its on-card fields, " · " between).
    std::optional<std::string> format;
    std::optional<std::string> pin;         ///< a `pins` part's pin kind
    std::optional<std::string> glyph;       ///< a glyph's: circle, square, diamond, arrow
    std::optional<bool>        hollow;      ///< a glyph drawn as an outline
    std::optional<std::string> side;        ///< a stub's end: left, right
    std::optional<float>       length;      ///< a stub's, world units
    std::optional<std::string> direction;   ///< a stub's arrow: in (toward the node), out
    friend bool operator==(const PartSpec&, const PartSpec&) = default;
};

/// @brief Card geometry a kit may set; unset fields come from the kit's, then the canvas's Style.
struct MetricsSpec {
    std::optional<float> padding, titleSize, fontSize, header, row, groupHeader;
    friend bool operator==(const MetricsSpec&, const MetricsSpec&) = default;
};

struct FontSpec {
    std::string          family;   ///< a backend's name for a face; empty = its default
    std::optional<float> size;
    std::optional<bool>  bold, italic;
    friend bool operator==(const FontSpec&, const FontSpec&) = default;
};

/// @brief A kind of node: its part tree, its geometry, and how it sizes. Its id is a NodeType id.
struct NodeKind {
    std::string          id;
    PartSpec             card{PartKind::Card, "card"};
    MetricsSpec          metrics;
    std::optional<float> minWidth, maxWidth;
    std::optional<bool>  fitWidth;   ///< the card grows (or shrinks) to its widest line, between minWidth and maxWidth
    friend bool operator==(const NodeKind&, const NodeKind&) = default;
};

/// @brief How a kind of pin looks and what its gestures do. A pin kind is named by a PortType id: the Schema keeps what
///        connects to what, the kit how it looks. Every field is optional, so a rule's patch sets only what it names.
struct PinSpec {
    std::optional<std::string> glyph;    ///< circle, square, diamond, arrow; unset = the port type's
    std::optional<float>       size;     ///< the pin's radius, world units; unset = the canvas's (4.5 at zoom 1, kept 2.5–6 px)
    std::optional<std::string> fill;     ///< wired (filled while a wire uses it), always, never
    std::optional<std::string> color;    ///< outline, and fill when filled; unset = the port type's colour
    std::optional<std::string> hollow;   ///< the fill while hollow; unset = the card's fill
    std::optional<float>       hit;      ///< how far past its radius a press still takes it, pixels; unset = 4
    std::optional<bool>        label;    ///< its row's label shows (a port row's); unset = yes
    std::map<std::string, std::string, std::less<>> on;   ///< gesture -> action id: click, doubleClick, contextMenu, dragToNothing
    friend bool operator==(const PinSpec&, const PinSpec&) = default;
};
struct PinKind {
    std::string id;   ///< a PortType id
    PinSpec     spec;
    friend bool operator==(const PinKind&, const PinKind&) = default;
};

/// @brief How a kind of row looks, how it is edited and what its gestures do. Row::style names one.
struct RowSpec {
    std::optional<std::string> color, valueColor;   ///< the label's and the value's
    std::optional<std::string> prefix, suffix;       ///< before the label, after the value
    std::optional<std::string> align, valueAlign;    ///< the label's (left, center, right) and the value's
    std::optional<bool>        bold, italic, chip;
    /// @brief How it is edited in place: none, text, number, slider, toggle, choice, color, path, reference (a list the
    ///        host supplies, Canvas::choicesFor). A gesture bound to the action `edit` opens it.
    std::optional<std::string> editor;
    std::optional<float>       min, max, step;   ///< a number's or a slider's
    /// @brief Rows of the same list (this name) may be dragged up and down among themselves (ReorderRow).
    std::optional<std::string> reorder;
    std::map<std::string, std::string, std::less<>> on;   ///< gesture -> action id: click, doubleClick, contextMenu
    friend bool operator==(const RowSpec&, const RowSpec&) = default;
};
struct RowKind {
    std::string id;   ///< what Row::style names
    RowSpec     spec;
    friend bool operator==(const RowKind&, const RowKind&) = default;
};

/// @brief How a kind of edge looks. An edge kind is named by an EdgeType id: the Schema keeps what it joins, the kit how
///        it draws. Every field is optional, so a rule's patch sets only what it names.
struct EdgeSpec {
    /// @brief curve, straight, elbow. The route follows it, so rules never change it: only the kind says it.
    std::optional<std::string> line;
    std::optional<float>       width;      ///< pixels at zoom 1
    std::optional<float>       widen;      ///< pixels added to the width (a state's emphasis, whatever the kind's width)
    std::optional<bool>        dash, dotted, onTop;   ///< dotted wins over dash; onTop draws it over the nodes
    std::optional<std::string> arrow;      ///< none, end, start, both
    std::optional<float>       arrowSize;  ///< world units (9 by default, kept 5–12 px at that size)
    std::optional<std::string> color;      ///< a colour expression
    std::optional<bool>        pinColor;   ///< the colour of the type its Out port carries, when it has one (else `color`)
    std::optional<float>       opacity;
    std::optional<std::string> labelColor, labelFill;
    std::optional<float>       labelScale;   ///< a label's size, in the canvas's font sizes
    friend bool operator==(const EdgeSpec&, const EdgeSpec&) = default;
};
struct EdgeKind {
    std::string id;   ///< an EdgeType id
    EdgeSpec    spec;
    friend bool operator==(const EdgeKind&, const EdgeKind&) = default;
};

/// @brief `kind? ([tag])* (:state)* (> part)?`, for example `box[class:Blend]:selected > header`; `*` or nothing = any kind.
///        A part may be `pin:<pin kind>` or `row:<row kind>`, and then the rule's pin or row fields patch it; or
///        `edge:<edge kind>`, whose kind, tags and states are then the edge's own (Edge::tags; selected, lit, merged,
///        dimmed).
struct Selector {
    std::string              kind;
    std::vector<std::string> tags;
    std::uint32_t            states = 0;
    std::string              part;   ///< empty = the card
    friend bool operator==(const Selector&, const Selector&) = default;
};

/// @brief The states a selector can ask for.
enum KitState : std::uint32_t {
    StateSelected  = 1u << 0,
    StateHovered   = 1u << 1,
    StateDimmed    = 1u << 2,
    StateAlias     = 1u << 3,
    StateCollapsed = 1u << 4,
    StateInGroup   = 1u << 5,
    StateWired     = 1u << 6,   ///< a pin a wire uses
    StateUnwired   = 1u << 7,   ///< a pin none does
    StateLit       = 1u << 8,   ///< an edge touching the selection (Style::highlightSelectedWires)
    StateMerged    = 1u << 9,   ///< an edge standing for several that join a closed group
};

/// @brief Patch the selected part with what `patch` sets (`pin`, `row` and `edge` for a `pin:`, `row:` or `edge:` part).
struct Rule {
    Selector select;
    PartSpec patch;
    PinSpec  pin;
    RowSpec  row;
    EdgeSpec edge;
    friend bool operator==(const Rule&, const Rule&) = default;
};

struct Kit {
    std::map<std::string, std::string, std::less<>> tokens;   ///< colour expressions by name ($name)
    std::map<std::string, FontSpec, std::less<>>    fonts;
    MetricsSpec                                     metrics;
    std::vector<NodeKind>                           nodes;
    std::vector<PinKind>                            pins;
    std::vector<RowKind>                            rows;
    std::vector<EdgeKind>                           edges;
    std::vector<Rule>                               rules;     ///< in file order; specificity orders them when applied
    /// @brief Sections a later milestone reads (actions), kept as the JSON text they came in.
    std::map<std::string, std::string, std::less<>> reserved;
    /// @brief Bump after changing the kit in place, so a canvas drops what it resolved. PutNodeKind bumps it.
    std::uint64_t                                   revision = 0;

    [[nodiscard]] const NodeKind* FindNode(std::string_view id) const;
    [[nodiscard]] const EdgeKind* FindEdge(std::string_view id) const;
    /// @brief Add a node kind, or replace the one with its id (a kind defined at runtime from data); bumps `revision`.
    void PutNodeKind(NodeKind kind);
    friend bool operator==(const Kit&, const Kit&) = default;
};

/// @brief The kit that draws `schema`'s nodes exactly as a canvas with `style` and no kit does.
[[nodiscard]] Kit KitFromSchema(const Schema& schema, const Style& style);

/// @return the kit, or nullopt with `error` saying where and why
[[nodiscard]] std::optional<Kit> ParseKit(std::string_view json, std::string& error);
/// @brief The kit as JSON: what ParseKit reads back to an equal kit.
[[nodiscard]] std::string WriteKit(const Kit& kit);

/// @return the selector, or nullopt when it does not read as one
[[nodiscard]] std::optional<Selector> ParseSelector(std::string_view text);
/// @return the colour an expression gives, with `band` for `band` and the kit's tokens; nullopt when it does not read
[[nodiscard]] std::optional<Color> ResolveColor(std::string_view expr, Color band, const Kit& kit);

/// @brief A text line of a resolved look.
struct LookText {
    std::string id, from, placeholder, format;
    Color       color, placeholderColor;
    float       size = 12.f, scale = 1.f, box = 1.3f, advance = 1.35f;
    bool        bold = false, italic = false;
    int         align = 0;   ///< 0 left, 1 center, 2 right (Align)
};

/// @brief A pin kind as resolved for one node, unwired [0] or wired [1].
struct PinLook {
    std::optional<PortGlyph> glyph;
    float                    size = 0.f;   ///< 0 = the canvas's
    int                      fill = 0;     ///< 0 wired, 1 always, 2 never
    std::optional<Color>     color, hollow;
    float                    hit   = 4.f;
    bool                     label = true;
    std::map<std::string, std::string, std::less<>> on;
};

/// @brief A row kind as resolved for one node.
struct RowLook {
    std::optional<Color> color, valueColor;
    std::string          prefix, suffix;
    int                  align = 0, valueAlign = 2;   ///< Align
    bool                 bold = false, italic = false, chip = false;
    std::string          editor;
    float                min = 0.f, max = 0.f, step = 0.f;
    std::string          reorder;
    std::map<std::string, std::string, std::less<>> on;
};

/// @brief An edge kind as resolved for one edge (its type, tags and state).
struct EdgeLook {
    EdgeLine line = EdgeLine::Curve;
    Color    color{140, 145, 155};
    bool     pinColor = false;
    float    width = 1.5f, opacity = 1.f, arrowSize = 9.f;
    bool     dash = false, dotted = false, onTop = false;
    int      arrow = 1;   ///< bits: 1 at the target end, 2 at the source end
    Color    labelColor{154, 158, 166}, labelFill{29, 30, 34};
    float    labelScale = 0.9f;
};

/// @brief A part of a resolved look.
struct LookPart {
    PartKind              kind = PartKind::Body;
    std::string           id;
    Color                 fill, stroke;
    float                 strokeWidth = 0.f, corner = 0.f, width = 0.f, top = 0.8f, height = 0.f;
    bool                  dash = false;
    std::vector<LookText> texts;   ///< a header's lines; a text's or a chip's own line
    std::map<std::string, std::string, std::less<>> on;   ///< gesture -> action id
    Color                    color{226, 228, 233};   ///< a glyph's, a bar's, a stub's
    std::optional<PortGlyph> glyph;
    float                    size = 0.f, length = 0.f;
    bool                     hollow = false, out = false;
    int                      side = 0;   ///< a stub's: 0 left, 1 right
    std::string              pin;        ///< a `pins` part's pin kind
    std::vector<LookPart>    content;    ///< its parts that are not text (a rim's needle and handle)
};

/// @brief What one node looks like: its card's paint, its visible parts in order, and its geometry. Resolved once per
///        (kit, kind, tags, state) and kept.
struct NodeLook {
    CardMetrics              metrics;
    Color                    fill, stroke;
    float                    strokeWidth = 1.f, corner = 6.f;
    bool                     dash = false;
    std::vector<LookPart>    parts;
    float                    minWidth = 0.f, maxWidth = 0.f;
    bool                     fitWidth = false;
    std::map<std::string, std::string, std::less<>> on;   ///< the card's own gestures
    /// @brief The kit's pin kinds as this node shows them, by PortType id: unwired, wired.
    std::map<std::string, std::array<PinLook, 2>, std::less<>> pins;
    std::map<std::string, RowLook, std::less<>>                rows;   ///< the kit's row kinds, by id
    int                      shape = 0;          ///< a closed group's: 0 a header-high frame, 1 a pill
    bool                     endPins = false;    ///< a closed group's merged wires end on pins at its two ends
    std::string              endPin;             ///< their pin kind
};

/// @brief Resolve the look of a node of type `type` with `tags` in `states`, under `kit` (already merged and with its
///        rules in application order: see Canvas). `base` is the geometry the kit's metrics refine.
[[nodiscard]] NodeLook ResolveLook(const Kit& kit, const NodeType& type, const std::vector<std::string>& tags, std::uint32_t states,
                                   const CardMetrics& base);

/// @brief Resolve the look of an edge of `type` with `tags` in `states`, under `kit` (its rules in application order).
[[nodiscard]] EdgeLook ResolveEdgeLook(const Kit& kit, const EdgeType& type, const std::vector<std::string>& tags, std::uint32_t states);

/// @brief `over` laid on `under`: its tokens, fonts and metrics win, its kinds replace theirs by id, its rules follow.
[[nodiscard]] Kit MergeKits(const Kit& under, const Kit& over);

} // namespace grapher
