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
};

/// @brief An open polyline (curves arrive already flattened).
struct Polyline {
    std::vector<Vec2> points;
    Color             color;
    float             width  = 1.f;
    bool              dashed = false;
};

struct Polygon {
    std::vector<Vec2> points;
    Color             fill;
};

struct Circle {
    Vec2  center;
    float radius = 0.f;
    Color fill;
    Color stroke;
    float strokeWidth = 0.f;
};

enum class Align { Left, Center };

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
};

using DrawCmd = std::variant<FillRect, Polyline, Polygon, Circle, Text>;

struct DrawList {
    Vec2                 size;   ///< the surface the list was laid out for
    std::vector<DrawCmd> cmds;
};

} // namespace grapher
