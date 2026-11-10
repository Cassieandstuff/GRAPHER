// grapher::avoid tests: the libavoid backend avoids nodes, avoids crossings it can, and reroutes incrementally.
#include <grapher/Grapher.h>
#include <grapher/avoid/AvoidRouter.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

namespace {

int g_failed = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failed;                                                             \
        }                                                                           \
    } while (0)

using grapher::Rect;
using grapher::Vec2;

bool Crosses(const std::vector<Vec2>& path, const Rect& r)
{
    for (std::size_t i = 1; i < path.size(); ++i)
        for (int k = 0; k <= 20; ++k) {
            const Vec2 p = path[i - 1] + (path[i] - path[i - 1]) * (static_cast<float>(k) / 20.f);
            if (p.x > r.x + 0.5f && p.x < r.Right() - 0.5f && p.y > r.y + 0.5f && p.y < r.Bottom() - 0.5f) return true;
        }
    return false;
}

bool OnOutline(Vec2 p, const Rect& r)
{
    const bool inX = p.x >= r.x - 0.5f && p.x <= r.Right() + 0.5f;
    const bool inY = p.y >= r.y - 0.5f && p.y <= r.Bottom() + 0.5f;
    const bool edgeX = std::abs(p.x - r.x) < 0.5f || std::abs(p.x - r.Right()) < 0.5f;
    const bool edgeY = std::abs(p.y - r.y) < 0.5f || std::abs(p.y - r.Bottom()) < 0.5f;
    return inX && inY && (edgeX || edgeY);
}

/// @brief Proper intersections between two polylines.
int Intersections(const std::vector<Vec2>& a, const std::vector<Vec2>& b)
{
    auto cross = [](Vec2 o, Vec2 p, Vec2 q) { return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x); };
    int n = 0;
    for (std::size_t i = 1; i < a.size(); ++i)
        for (std::size_t j = 1; j < b.size(); ++j) {
            const float d1 = cross(b[j - 1], b[j], a[i - 1]), d2 = cross(b[j - 1], b[j], a[i]);
            const float d3 = cross(a[i - 1], a[i], b[j - 1]), d4 = cross(a[i - 1], a[i], b[j]);
            if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) ++n;
        }
    return n;
}

grapher::Schema MakeSchema()
{
    grapher::Schema s;
    s.nodeTypes.push_back({"box", "Box"});
    s.edgeTypes.push_back({"curve", "curve"});
    grapher::EdgeType elbow{"elbow", "elbow"};
    elbow.line = grapher::EdgeLine::Elbow;
    s.edgeTypes.push_back(elbow);
    return s;
}

void TestAvoidsNodes()
{
    const grapher::Schema s = MakeSchema();
    grapher::Graph        g;
    g.nodes.push_back({"a", "box", {0, 0, 100, 60}});
    g.nodes.push_back({"b", "box", {400, 0, 100, 60}});
    g.nodes.push_back({"c", "box", {200, -40, 80, 140}});
    g.edges.push_back({"e", "a", "b", "elbow"});
    auto router = grapher::avoid::MakeRouter();
    const auto w = router->Route(g, s, {});
    CHECK(w.contains("e"));
    if (!w.contains("e")) return;
    const auto& path = w.at("e");
    CHECK(!Crosses(path, g.nodes[2].frame));
    CHECK(OnOutline(path.front(), g.nodes[0].frame));
    CHECK(OnOutline(path.back(), g.nodes[1].frame));

    // Incremental: move the blocker out of the way and the wire straightens (fewer points than the detour).
    g.nodes[2].frame = {200, 300, 80, 140};
    const auto moved = router->Route(g, s, {});
    CHECK(moved.contains("e") && moved.at("e").size() <= path.size());
    CHECK(!Crosses(moved.at("e"), g.nodes[2].frame));

    // A deleted node takes its wire with it.
    g.nodes.erase(g.nodes.begin() + 1);
    g.edges.clear();
    CHECK(router->Route(g, s, {}).empty());
}

/// @brief The x of every vertical run longer than `min` in a path.
std::vector<float> VerticalRuns(const std::vector<Vec2>& path, float min)
{
    std::vector<float> xs;
    for (std::size_t i = 1; i < path.size(); ++i)
        if (std::abs(path[i].x - path[i - 1].x) < 0.01f && std::abs(path[i].y - path[i - 1].y) > min) xs.push_back(path[i].x);
    return xs;
}

void TestNestsParallelElbows()
{
    // Two elbows leave stacked sources and drop to stacked targets far below and to the right. Turning at the same
    // place would overlap them, and turning in the wrong order would cross them; nested, they do neither.
    const grapher::Schema s = MakeSchema();
    grapher::Graph        g;
    g.nodes.push_back({"A", "box", {0, 0, 100, 60}});
    g.nodes.push_back({"B", "box", {0, 100, 100, 60}});
    g.nodes.push_back({"C", "box", {500, 400, 100, 60}});
    g.nodes.push_back({"D", "box", {500, 500, 100, 60}});
    g.edges.push_back({"ac", "A", "C", "elbow"});
    g.edges.push_back({"bd", "B", "D", "elbow"});
    grapher::RoutingOptions sharp;
    sharp.cornerRadius = 0.f;
    const auto w = grapher::avoid::MakeRouter()->Route(g, s, sharp);
    CHECK(w.contains("ac") && w.contains("bd"));
    if (!w.contains("ac") || !w.contains("bd")) return;
    CHECK(Intersections(w.at("ac"), w.at("bd")) == 0);
    for (float a : VerticalRuns(w.at("ac"), 40.f))
        for (float b : VerticalRuns(w.at("bd"), 40.f)) CHECK(std::abs(a - b) >= sharp.laneSpacing - 0.5f);   // apart, not stacked
}

void TestCanvasUsesBackend()
{
    const grapher::Schema s = MakeSchema();
    grapher::Graph        g;
    g.nodes.push_back({"a", "box", {0, 0, 100, 60}});
    g.nodes.push_back({"b", "box", {400, 0, 100, 60}});
    g.edges.push_back({"e", "a", "b", "curve", "label"});
    grapher::Canvas canvas(g, s);
    canvas.router = grapher::avoid::MakeRouter();
    canvas.SetViewport({800, 400});
    bool label = false;
    for (const auto& c : canvas.Draw().cmds)
        if (const auto* t = std::get_if<grapher::Text>(&c); t && t->text == "label") label = true;
    CHECK(label);   // the wire was routed and drawn
}

} // namespace

int main()
{
#ifdef _MSC_VER
    for (int kind : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
    }
#endif
    TestAvoidsNodes();
    TestNestsParallelElbows();
    TestCanvasUsesBackend();
    if (g_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failed);
        return EXIT_FAILURE;
    }
    std::puts("grapher-avoid-test: all checks passed");
    return EXIT_SUCCESS;
}
