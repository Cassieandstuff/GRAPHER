#pragma once
/// @file
/// @brief The geometry of a NodeShape::Dial node: where a row's value falls on its rim, and the value a point on the
///        rim stands for. Angles are clockwise from the top, in radians. A cyclic axis goes round the whole rim at
///        its true angles. An open one lies on an arc with its gap at the bottom (270° on a circle; on an octagon, all
///        but the flat bottom edge), so its ends never meet, its rows evenly spaced in order and the axis linear between
///        neighbours, so values far apart in size still spread out.
///        The rim is a circle, or an octagon (a kit's rim part, CardMetrics::dialChamfer): a row stands where the ray
///        at its angle meets the rim, its handle drags along the edges, and the node is the octagon, with a header on
///        its top edge and its other rows listed under it.

#include <grapher/Graph.h>
#include <grapher/Ports.h>

#include <numbers>
#include <span>
#include <vector>

namespace grapher {

struct DialAxis {
    float              lo = 0.f, hi = 1.f;   ///< a cyclic axis' range; an open one's first and last values
    bool               cyclic = false;
    std::vector<float> knots;                ///< an open axis' distinct values, ascending
    double             reach = 0.75;   ///< an open axis' arc each side of the top, in half-turns (0.75 = 135°)

    /// @param values the rows' values; ignored for a cyclic axis
    /// @param reach  an open axis' arc each side of the top (the rim's OpenReach)
    [[nodiscard]] static DialAxis Make(bool cyclic, float min, float max, std::span<const float> values,
                                       double reach = 0.75);

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

/// @return whether `row` of dial node `node` stands on its rim (Dial::rim, else every row claiming a port)
[[nodiscard]] bool OnRim(const Node& node, const Row& row);

/// @brief The axis a dial node's rows make: the values of the rows on its rim.
[[nodiscard]] DialAxis AxisOf(const Node& node, double reach = 0.75);

/// @brief A dial's rim: a circle of `radius`, or with `chamfer` ≥ 0 an octagon `radius` from its centre to each flat
///        side, its corners cut by `chamfer` × radius along each side.
struct Ring {
    Vec2  center;
    float radius  = 0.f;
    float chamfer = -1.f;
    /// @brief The point on the rim where the ray at `angle` meets it, `scale` times as far out.
    [[nodiscard]] Vec2 At(double angle, float scale = 1.f) const;
    /// @brief Outward there: the ray's on a circle, the edge's on an octagon (a wire leaves square to the side).
    [[nodiscard]] Vec2 Normal(double angle) const;
    /// @brief An open axis' arc each side of the top, in half-turns: 0.75 (270° in all) on a circle, all but the flat
    ///        bottom edge on an octagon.
    [[nodiscard]] double OpenReach() const;
    /// @brief The rim from `a0` to `a1` (clockwise), its corners kept.
    [[nodiscard]] std::vector<Vec2> Outline(double a0, double a1) const;
    [[nodiscard]] bool Holds(Vec2 p) const;
};
[[nodiscard]] Ring RingOf(const Rect& frame);

/// @brief A dial node laid out in `frame`: its rim, and with an octagon rim its body (the node's outline, a header on
///        its top edge) and where its listed rows run.
struct DialShape {
    Ring              rim;
    std::vector<Vec2> body;   ///< an octagon's outline, clockwise from the top edge's left end; empty = the frame
    float             cut = 0.f;                         ///< how much the body's corners are cut along each side
    float             header = 0.f;                      ///< the body's header height
    float             rowsTop = 0.f, rowsBottom = 0.f;   ///< the listed rows' band
    Rect              frame;
    [[nodiscard]] bool Octagon() const { return !body.empty(); }
    /// @brief The body's left (or right) edge at height `y`.
    [[nodiscard]] float EdgeX(float y, bool right) const;
    [[nodiscard]] bool  Holds(Vec2 p) const;
};
[[nodiscard]] DialShape DialOf(const Node& node, const CardMetrics& m, const Rect& frame);

/// @return the part of the convex polygon `poly` at or above height `y` (a header band in an octagon)
[[nodiscard]] std::vector<Vec2> ClipAbove(const std::vector<Vec2>& poly, float y);

} // namespace grapher
