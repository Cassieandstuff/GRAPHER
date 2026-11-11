#pragma once
/// @file
/// @brief The geometry of a NodeShape::Dial node: where a row's value falls on its ring, and the value a point on the
///        ring stands for. Angles are clockwise from the top, in radians. A cyclic axis goes round the whole circle at
///        its true angles. An open one lies on a 270° arc with its gap at the bottom, so its ends never meet, its rows
///        evenly spaced in order and the axis linear between neighbours, so values far apart in size still spread out.

#include <grapher/Graph.h>

#include <span>
#include <vector>

namespace grapher {

struct DialAxis {
    float              lo = 0.f, hi = 1.f;   ///< a cyclic axis' range; an open one's first and last values
    bool               cyclic = false;
    std::vector<float> knots;                ///< an open axis' distinct values, ascending

    /// @param values the rows' values; ignored for a cyclic axis
    [[nodiscard]] static DialAxis Make(bool cyclic, float min, float max, std::span<const float> values);

    /// @brief Where `value` falls along the axis, 0..1; a cyclic one is not wrapped.
    [[nodiscard]] double Fraction(float value) const;
    [[nodiscard]] double Angle(float value) const;
    /// @brief The value at angle `a`; an open axis clamps to its arc, a cyclic one wraps into its range.
    [[nodiscard]] float Value(double a) const;
    /// @brief `value` snapped to the nearest sixteenth along the axis (of the range, or of the arc between neighbours).
    [[nodiscard]] float Snap(float value) const;
};

/// @return a row's value as a number; 0 when it is not one
[[nodiscard]] float RowNumber(const Row& row);

/// @brief The axis a dial node's rows make: the values of the rows that claim a port.
[[nodiscard]] DialAxis AxisOf(const Node& node);

/// @brief A dial's ring inside its frame: room is left around it for the pins and the rows' labels.
struct Ring {
    Vec2  center;
    float radius = 0.f;
    [[nodiscard]] Vec2 At(double angle, float scale = 1.f) const;   ///< the point at `angle`, `scale` times the radius out
};
[[nodiscard]] Ring RingOf(const Rect& frame);

} // namespace grapher
