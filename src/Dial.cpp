#include <grapher/Dial.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>

namespace grapher {

namespace {
constexpr double kPi = std::numbers::pi;
constexpr float  kLabelRoom = 34.f;   ///< world units between the rim and the frame (or the body), for pins and labels

Vec2 Dir(double a) { return {static_cast<float>(std::sin(a)), static_cast<float>(-std::cos(a))}; }
double AngleOf(Vec2 d) { return std::atan2(static_cast<double>(d.x), -static_cast<double>(d.y)); }

/// @brief How far along unit ray `d` an octagon reaches: half-sizes `hx`, `hy` to its flat sides, corners cut by `k`.
///        `edge` says which side it meets: 0 a side, 1 the top or bottom, 2 a cut corner.
float Reach(Vec2 d, float hx, float hy, float k, int& edge)
{
    const float ax = std::abs(d.x), ay = std::abs(d.y);
    float       t = 1e30f;
    edge          = 0;
    if (ax > 1e-6f) t = hx / ax;
    if (ay > 1e-6f && hy / ay < t) t = hy / ay, edge = 1;
    if (ax + ay > 1e-6f && (hx + hy - k) / (ax + ay) < t) t = (hx + hy - k) / (ax + ay), edge = 2;
    return t;
}

/// @brief An octagon's corners, clockwise from the top edge's left end.
std::vector<Vec2> Corners(Vec2 c, float hx, float hy, float k)
{
    return {{c.x - hx + k, c.y - hy}, {c.x + hx - k, c.y - hy}, {c.x + hx, c.y - hy + k}, {c.x + hx, c.y + hy - k},
            {c.x + hx - k, c.y + hy}, {c.x - hx + k, c.y + hy}, {c.x - hx, c.y + hy - k}, {c.x - hx, c.y - hy + k}};
}
} // namespace

DialAxis DialAxis::Make(bool cyclic, float min, float max, std::span<const float> values, double reach)
{
    DialAxis a;
    a.cyclic = cyclic;
    a.reach  = reach;
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
    return cyclic ? f * 2 * kPi : (-reach + 2 * reach * f) * kPi;
}

float DialAxis::Value(double a) const
{
    a = std::remainder(a, 2 * kPi);   // -pi..pi
    if (cyclic) return static_cast<float>(lo + (a < 0 ? a + 2 * kPi : a) / (2 * kPi) * (hi - lo));
    const double f = std::clamp((a / kPi + reach) / (2 * reach), 0.0, 1.0) * static_cast<double>(knots.size() - 1);
    const auto   i = std::min(static_cast<std::size_t>(f), knots.size() - 2);
    return static_cast<float>(knots[i] + (f - static_cast<double>(i)) * (knots[i + 1] - knots[i]));
}

float DialAxis::Snap(float value) const
{
    const double f = std::round(Fraction(value) * 16) / 16;
    return cyclic ? static_cast<float>(lo + f * (hi - lo)) : Value((-reach + 2 * reach * f) * kPi);
}

float RowNumber(const Row& row)
{
    float v = 0.f;
    const char* s = row.value.data();
    std::from_chars(s, s + row.value.size(), v);
    return v;
}

bool OnRim(const Node& node, const Row& row)
{
    if (node.dial.rim.empty()) return !row.port.empty();
    return std::ranges::find(node.dial.rim, row.id) != node.dial.rim.end();
}

DialAxis AxisOf(const Node& node, double reach)
{
    std::vector<float> values;
    for (const Row& r : node.rows)
        if (OnRim(node, r)) values.push_back(RowNumber(r));
    return DialAxis::Make(node.dial.cyclic, node.dial.min, node.dial.max, values, reach);
}

Vec2 Ring::At(double angle, float scale) const
{
    const Vec2 d = Dir(angle);
    if (chamfer < 0.f) return {center.x + d.x * radius * scale, center.y + d.y * radius * scale};
    int         edge = 0;
    const float t    = Reach(d, radius, radius, std::min(chamfer, 1.f) * radius, edge);
    return {center.x + d.x * t * scale, center.y + d.y * t * scale};
}

Vec2 Ring::Normal(double angle) const
{
    const Vec2 d = Dir(angle);
    if (chamfer < 0.f) return d;
    int edge = 0;
    (void)Reach(d, radius, radius, std::min(chamfer, 1.f) * radius, edge);
    const float sx = d.x < 0.f ? -1.f : 1.f, sy = d.y < 0.f ? -1.f : 1.f;
    if (edge == 0) return {sx, 0.f};
    if (edge == 1) return {0.f, sy};
    return {sx * std::numbers::sqrt2_v<float> * 0.5f, sy * std::numbers::sqrt2_v<float> * 0.5f};
}

double Ring::OpenReach() const
{
    if (chamfer < 0.f) return 0.75;
    const double k = std::min(static_cast<double>(chamfer), 1.0) * radius;
    return 1.0 - std::atan2(radius - k, static_cast<double>(radius)) / kPi;   // to the bottom edge's ends
}

std::vector<Vec2> Ring::Outline(double a0, double a1) const
{
    std::vector<Vec2> out;
    if (chamfer < 0.f) {
        for (int k = 0; k <= 64; ++k) out.push_back(At(a0 + (a1 - a0) * k / 64.0));
        return out;
    }
    // Its corners between the ends, in order, so the edges stay straight.
    std::vector<std::pair<double, Vec2>> corners;
    for (const Vec2& p : Corners(center, radius, radius, std::min(chamfer, 1.f) * radius)) {
        double a = AngleOf(p - center);
        while (a < a0) a += 2 * kPi;
        while (a >= a0 + 2 * kPi) a -= 2 * kPi;
        if (a > a0 + 1e-6 && a < a1 - 1e-6) corners.emplace_back(a, p);   // an end on a corner keeps it once
    }
    std::ranges::sort(corners, {}, &std::pair<double, Vec2>::first);
    out.push_back(At(a0));
    for (const auto& c : corners) out.push_back(c.second);
    out.push_back(At(a1));
    return out;
}

bool Ring::Holds(Vec2 p) const
{
    const Vec2 d = p - center;
    if (chamfer < 0.f) return d.x * d.x + d.y * d.y <= radius * radius;
    const float ax = std::abs(d.x), ay = std::abs(d.y);
    return ax <= radius && ay <= radius && ax + ay <= 2 * radius - std::min(chamfer, 1.f) * radius;
}

Ring RingOf(const Rect& frame)
{
    return {frame.Center(), std::max(8.f, std::min(frame.w, frame.h) * 0.5f - kLabelRoom)};
}

DialShape DialOf(const Node& node, const CardMetrics& m, const Rect& frame)
{
    DialShape s;
    s.frame = frame;
    if (m.dialChamfer < 0.f) {   // a circle in the frame, as ever
        s.rim = RingOf(frame);
        return s;
    }
    const float cut = std::min(m.dialChamfer, 1.f);
    const float hx = frame.w * 0.5f, hy = frame.h * 0.5f;
    s.cut    = cut * std::min(hx, hy);
    s.body   = Corners(frame.Center(), hx, hy, s.cut);
    s.header = std::min(frame.h, m.Header());
    std::size_t listed = 0;
    for (const Row& r : node.rows) listed += OnRim(node, r) ? 0 : 1;
    s.rowsTop    = frame.y + s.header;
    s.rowsBottom = std::min(frame.Bottom(), s.rowsTop + static_cast<float>(listed) * m.Row());
    // The rim below them, centred in what is left, with room around it for the pins' labels.
    const float room = frame.Bottom() - s.rowsBottom;
    s.rim.center     = {frame.x + hx, s.rowsBottom + room * 0.5f};
    s.rim.radius     = std::max(8.f, std::min(frame.w, room) * 0.5f - kLabelRoom);
    s.rim.chamfer    = cut;
    return s;
}

float DialShape::EdgeX(float y, bool right) const
{
    if (body.empty()) return right ? frame.Right() : frame.x;
    const float hx = frame.w * 0.5f, hy = frame.h * 0.5f;
    const float reach = std::clamp(hx + hy - cut - std::abs(y - (frame.y + hy)), 0.f, hx);
    return frame.x + hx + (right ? reach : -reach);
}

bool DialShape::Holds(Vec2 p) const
{
    if (body.empty()) return frame.Contains(p);
    const float hx = frame.w * 0.5f, hy = frame.h * 0.5f;
    const float ax = std::abs(p.x - (frame.x + hx)), ay = std::abs(p.y - (frame.y + hy));
    return ax <= hx && ay <= hy && ax + ay <= hx + hy - cut;
}

std::vector<Vec2> ClipAbove(const std::vector<Vec2>& poly, float y)
{
    std::vector<Vec2> out;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const Vec2 a = poly[i], b = poly[(i + 1) % poly.size()];
        const bool ina = a.y <= y, inb = b.y <= y;
        if (ina) out.push_back(a);
        if (ina != inb) {
            const float t = (y - a.y) / (b.y - a.y);
            out.push_back({a.x + (b.x - a.x) * t, y});
        }
    }
    return out;
}

} // namespace grapher
