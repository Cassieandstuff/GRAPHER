#include <grapher/Dial.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>

namespace grapher {

namespace {
constexpr double kPi = std::numbers::pi;
constexpr float  kLabelRoom = 34.f;   ///< world units between the ring and the frame, for pins and labels
} // namespace

DialAxis DialAxis::Make(bool cyclic, float min, float max, std::span<const float> values)
{
    DialAxis a;
    a.cyclic = cyclic;
    if (cyclic) {
        a.lo = min;
        a.hi = max > min ? max : min + 1.f;
        return a;
    }
    a.knots.assign(values.begin(), values.end());
    std::ranges::sort(a.knots);
    a.knots.erase(std::unique(a.knots.begin(), a.knots.end()), a.knots.end());
    if (a.knots.size() < 2) {
        const float at = a.knots.empty() ? 0.f : a.knots.front();
        a.knots        = {at, at + 1.f};
    }
    a.lo = a.knots.front();
    a.hi = a.knots.back();
    return a;
}

double DialAxis::Fraction(float value) const
{
    if (cyclic) return (value - lo) / static_cast<double>(hi - lo);
    if (value <= knots.front()) return 0;
    if (value >= knots.back()) return 1;
    const auto   up = std::ranges::upper_bound(knots, value);
    const auto   i  = static_cast<std::size_t>(up - knots.begin()) - 1;
    const double t  = (value - knots[i]) / static_cast<double>(knots[i + 1] - knots[i]);
    return (static_cast<double>(i) + t) / static_cast<double>(knots.size() - 1);
}

double DialAxis::Angle(float value) const
{
    const double f = Fraction(value);
    return cyclic ? f * 2 * kPi : (-0.75 + 1.5 * f) * kPi;
}

float DialAxis::Value(double a) const
{
    a = std::remainder(a, 2 * kPi);   // -pi..pi
    if (cyclic) return static_cast<float>(lo + (a < 0 ? a + 2 * kPi : a) / (2 * kPi) * (hi - lo));
    const double f = std::clamp((a / kPi + 0.75) / 1.5, 0.0, 1.0) * static_cast<double>(knots.size() - 1);
    const auto   i = std::min(static_cast<std::size_t>(f), knots.size() - 2);
    return static_cast<float>(knots[i] + (f - static_cast<double>(i)) * (knots[i + 1] - knots[i]));
}

float DialAxis::Snap(float value) const
{
    const double f = std::round(Fraction(value) * 16) / 16;
    return cyclic ? static_cast<float>(lo + f * (hi - lo)) : Value((-0.75 + 1.5 * f) * kPi);
}

float RowNumber(const Row& row)
{
    float v = 0.f;
    const char* s = row.value.data();
    std::from_chars(s, s + row.value.size(), v);
    return v;
}

DialAxis AxisOf(const Node& node)
{
    std::vector<float> values;
    for (const Row& r : node.rows)
        if (!r.port.empty()) values.push_back(RowNumber(r));
    return DialAxis::Make(node.dial.cyclic, node.dial.min, node.dial.max, values);
}

Vec2 Ring::At(double angle, float scale) const
{
    return {center.x + static_cast<float>(std::sin(angle)) * radius * scale, center.y - static_cast<float>(std::cos(angle)) * radius * scale};
}

Ring RingOf(const Rect& frame)
{
    return {frame.Center(), std::max(8.f, std::min(frame.w, frame.h) * 0.5f - kLabelRoom)};
}

} // namespace grapher
