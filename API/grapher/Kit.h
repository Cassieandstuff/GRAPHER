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
    // Parsed and kept; drawn from K2 and K3:
    Pins, Badge, Button, Chip, Bar, Glyph, Image, Rim, Stub,
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

/// @brief `kind? ([tag])* (:state)* (> part)?`, for example `box[class:Blend]:selected > header`; `*` or nothing = any kind.
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
};

/// @brief Patch the selected part with what `patch` sets.
struct Rule {
    Selector select;
    PartSpec patch;
    friend bool operator==(const Rule&, const Rule&) = default;
};

struct Kit {
    std::map<std::string, std::string, std::less<>> tokens;   ///< colour expressions by name ($name)
    std::map<std::string, FontSpec, std::less<>>    fonts;
    MetricsSpec                                     metrics;
    std::vector<NodeKind>                           nodes;
    std::vector<Rule>                               rules;     ///< in file order; specificity orders them when applied
    /// @brief Sections later milestones read (pins, rows, groups, edges, actions), kept as the JSON text they came in.
    std::map<std::string, std::string, std::less<>> reserved;
    /// @brief Bump after changing the kit in place, so a canvas drops what it resolved. PutNodeKind bumps it.
    std::uint64_t                                   revision = 0;

    [[nodiscard]] const NodeKind* FindNode(std::string_view id) const;
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
    std::string id, from, placeholder;
    Color       color, placeholderColor;
    float       size = 12.f, scale = 1.f, box = 1.3f, advance = 1.35f;
    bool        bold = false, italic = false;
    int         align = 0;   ///< 0 left, 1 center, 2 right (Align)
};

/// @brief A part of a resolved look.
struct LookPart {
    PartKind              kind = PartKind::Body;
    std::string           id;
    Color                 fill, stroke;
    float                 strokeWidth = 0.f, corner = 0.f, width = 0.f, top = 0.8f, height = 0.f;
    bool                  dash = false;
    std::vector<LookText> texts;   ///< a header's lines
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
};

/// @brief Resolve the look of a node of type `type` with `tags` in `states`, under `kit` (already merged and with its
///        rules in application order: see Canvas). `base` is the geometry the kit's metrics refine.
[[nodiscard]] NodeLook ResolveLook(const Kit& kit, const NodeType& type, const std::vector<std::string>& tags, std::uint32_t states,
                                   const CardMetrics& base);

/// @brief `over` laid on `under`: its tokens, fonts and metrics win, its kinds replace theirs by id, its rules follow.
[[nodiscard]] Kit MergeKits(const Kit& under, const Kit& over);

} // namespace grapher
