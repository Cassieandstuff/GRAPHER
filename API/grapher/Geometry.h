#pragma once
/// @file
/// @brief The plain value types every layer of GRAPHER passes around: points, rectangles and colours.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace grapher {

struct Vec2 {
    float x = 0.f;
    float y = 0.f;

    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
    friend constexpr Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
    friend constexpr Vec2 operator/(Vec2 a, float s) { return {a.x / s, a.y / s}; }
    friend constexpr bool operator==(Vec2, Vec2) = default;
};

[[nodiscard]] inline float Length(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
[[nodiscard]] inline float Distance(Vec2 a, Vec2 b) { return Length(b - a); }

/// @brief An axis-aligned rectangle; `w` and `h` are never negative.
struct Rect {
    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;

    [[nodiscard]] constexpr float Right() const { return x + w; }
    [[nodiscard]] constexpr float Bottom() const { return y + h; }
    [[nodiscard]] constexpr Vec2  Center() const { return {x + w * 0.5f, y + h * 0.5f}; }
    [[nodiscard]] constexpr bool  Contains(Vec2 p) const { return p.x >= x && p.x <= Right() && p.y >= y && p.y <= Bottom(); }
    [[nodiscard]] constexpr bool  Intersects(const Rect& o) const {
        return x <= o.Right() && o.x <= Right() && y <= o.Bottom() && o.y <= Bottom();
    }
    /// @brief Whether `o` lies wholly inside (touching the outline counts).
    [[nodiscard]] constexpr bool Holds(const Rect& o) const {
        return o.x >= x - 0.01f && o.Right() <= Right() + 0.01f && o.y >= y - 0.01f && o.Bottom() <= Bottom() + 0.01f;
    }
    [[nodiscard]] constexpr float Area() const { return w * h; }
    [[nodiscard]] constexpr Rect Inflated(float d) const { return {x - d, y - d, w + 2 * d, h + 2 * d}; }
    [[nodiscard]] static Rect FromPoints(Vec2 a, Vec2 b) {
        return {std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y)};
    }
    /// @brief The smallest rectangle holding both.
    [[nodiscard]] static Rect Union(const Rect& a, const Rect& b) {
        const float l = std::min(a.x, b.x), t = std::min(a.y, b.y);
        return {l, t, std::max(a.Right(), b.Right()) - l, std::max(a.Bottom(), b.Bottom()) - t};
    }
    friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

/// @brief 8-bit RGBA.
struct Color {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
    friend constexpr bool operator==(Color, Color) = default;

    /// @return `#rrggbb`, or `#rrggbbaa` when not opaque
    [[nodiscard]] std::string ToHex() const;
};

/// @brief Reads `#rgb`, `#rrggbb` or `#rrggbbaa` (the `#` optional).
/// @return nullopt on anything else
[[nodiscard]] std::optional<Color> ParseColor(std::string_view text);

} // namespace grapher
