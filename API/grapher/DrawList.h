#pragma once
/// @file
/// @brief What GRAPHER asks a backend to paint, in screen pixels, back to front. A backend (Qt, ImGui, a web canvas)
///        paints these and nothing else, so every backend draws the same picture.

#include <grapher/Geometry.h>

#include <string>
#include <variant>
#include <vector>

namespace grapher {

struct FillRect {
    Rect  rect;
    Color fill;
    Color stroke;
    float strokeWidth = 0.f;   ///< 0 = no outline
    float radius      = 0.f;
    bool  dashed      = false;   ///< the outline
    friend bool operator==(const FillRect&, const FillRect&) = default;
};

/// @brief An open polyline (curves arrive already flattened).
struct Polyline {
    std::vector<Vec2> points;
    Color             color;
    float             width  = 1.f;
    bool              dashed = false;
    bool              dotted = false;
    friend bool operator==(const Polyline&, const Polyline&) = default;
};

struct Polygon {
    std::vector<Vec2> points;
    Color             fill;
    friend bool operator==(const Polygon&, const Polygon&) = default;
};

struct Circle {
    Vec2  center;
    float radius = 0.f;
    Color fill;
    Color stroke;
    float strokeWidth = 0.f;
    friend bool operator==(const Circle&, const Circle&) = default;
};

enum class Align { Left, Center, Right };

/// @brief Text confined to `box`. Single-line text is elided to fit; wrapped text is clipped at the box.
/// @note The box's height is the line height for single-line text; `size` is the font's pixel size.
struct Text {
    Rect        box;
    std::string text;   ///< UTF-8
    float       size = 13.f;
    Color       color;
    bool        bold  = false;
    bool        wrap  = false;
    bool        mono  = false;   ///< a fixed-width face (pseudo-code)
    Align       align = Align::Left;
    bool        italic = false;
    friend bool operator==(const Text&, const Text&) = default;
};

/// @brief A picture the backend resolves by `key` (a node's thumbnail), scaled to fill `box`.
struct Image {
    Rect        box;
    std::string key;
    float       radius = 0.f;   ///< rounded corners
    friend bool operator==(const Image&, const Image&) = default;
};

using DrawCmd = std::variant<FillRect, Polyline, Polygon, Circle, Text, Image>;

struct DrawList {
    Vec2                 size;   ///< the surface the list was laid out for
    std::vector<DrawCmd> cmds;
    friend bool operator==(const DrawList&, const DrawList&) = default;
};

} // namespace grapher
