// GRAPHER core tests: plain asserts, no framework (the TARGETER convention).
#include <grapher/Grapher.h>

#include "../src/Json.h"

#include <algorithm>
#include <array>
#include <regex>
#include <iterator>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <variant>
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

grapher::Schema MakeSchema()
{
    grapher::Schema s;
    grapher::NodeType repo;
    repo.id = "repo";
    repo.label = "Repository";
    repo.drillable = true;
    repo.fields = {{"name", "Name"}, {"license", "License", grapher::FieldKind::Tag, {"MIT", "GPL-3.0"}}};
    s.nodeTypes.push_back(repo);
    grapher::NodeType note;
    note.id = "note";
    note.label = "Note";
    note.shape = grapher::NodeShape::Note;
    s.nodeTypes.push_back(note);
    s.edgeTypes.push_back({"depends", "depends on"});
    return s;
}

void TestColor()
{
    CHECK(grapher::ParseColor("#4f8cff") == (grapher::Color{0x4f, 0x8c, 0xff, 255}));
    CHECK(grapher::ParseColor("fff") == (grapher::Color{255, 255, 255, 255}));
    CHECK(!grapher::ParseColor("#12345").has_value());
    CHECK(!grapher::ParseColor("#zzzzzz").has_value());
    CHECK((grapher::Color{1, 2, 3, 255}).ToHex() == "#010203");
}

void TestGraph()
{
    grapher::Graph g;
    g.nodes.push_back({"a", "repo", {0, 0, 100, 50}});
    g.nodes.push_back({"b", "repo", {200, 0, 100, 50}});
    g.edges.push_back({"e", "a", "b", "depends"});
    CHECK(g.FindNode("b") != nullptr);
    CHECK(g.Bounds() == (grapher::Rect{0, 0, 300, 50}));
    g.RemoveNodes({"a"});
    CHECK(g.nodes.size() == 1);
    CHECK(g.edges.empty());   // edges touching a removed node go with it
    CHECK(grapher::NewId().size() == 16);
    CHECK(grapher::NewId() != grapher::NewId());
}

void TestCamera()
{
    grapher::Camera c{{100, 50}, 2.f};
    const grapher::Vec2 w{10, 20};
    CHECK(c.ToWorld(c.ToScreen(w)) == w);
}

void TestGestures()
{
    const grapher::Schema s = MakeSchema();
    grapher::Graph        g;
    grapher::Canvas       canvas(g, s);
    canvas.SetViewport({800, 600});

    int snapshots = 0, changes = 0;
    canvas.onBeforeChange = [&](std::string_view) { ++snapshots; };
    canvas.onChanged      = [&] { ++changes; };

    const std::string a = canvas.AddNode("repo", {120, 60});
    const std::string b = canvas.AddNode("repo", {520, 60});
    CHECK(!a.empty() && !b.empty());
    CHECK(canvas.AddNode("nope", {0, 0}).empty());
    CHECK(snapshots == 2 && changes == 2);

    // A click on a node selects it and changes nothing.
    const grapher::Rect ra = canvas.ScreenRectOf(a);
    const grapher::Vec2 inA{ra.x + 30, ra.y + 30};
    CHECK(canvas.HitTest(inA).kind == grapher::HitKind::Node);
    canvas.PointerDown(inA, grapher::Button::Left, {});
    canvas.PointerUp(inA, grapher::Button::Left);
    CHECK(canvas.SelectedNodes().size() == 1 && canvas.SelectedNodes().contains(a));
    CHECK(snapshots == 2);

    // A drag moves it on the snap grid, snapshotting once.
    canvas.PointerDown(inA, grapher::Button::Left, {});
    canvas.PointerMove({inA.x + 13, inA.y + 4});
    canvas.PointerMove({inA.x + 47, inA.y + 21});
    canvas.PointerUp({inA.x + 47, inA.y + 21}, grapher::Button::Left);
    const grapher::Node* na = g.FindNode(a);
    CHECK(snapshots == 3 && changes == 3);
    CHECK(na->frame.x == 50.f && na->frame.y == 20.f);   // 0+47 -> 50, 0+21 -> 20

    // Dragging from a's connect handle onto b draws one edge of the first edge type.
    const grapher::Rect ra2 = canvas.ScreenRectOf(a);
    const grapher::Vec2 handle{ra2.Right(), ra2.y + ra2.h * 0.5f};
    CHECK(canvas.HitTest(handle).kind == grapher::HitKind::Connect);
    const grapher::Rect rb = canvas.ScreenRectOf(b);
    const grapher::Vec2 inB{rb.x + 40, rb.y + 40};
    canvas.PointerDown(handle, grapher::Button::Left, {});
    canvas.PointerMove(inB);
    canvas.PointerUp(inB, grapher::Button::Left);
    CHECK(g.edges.size() == 1 && g.edges[0].from == a && g.edges[0].to == b && g.edges[0].type == "depends");

    // The same pair again adds nothing.
    canvas.PointerDown(handle, grapher::Button::Left, {});
    canvas.PointerMove(inB);
    canvas.PointerUp(inB, grapher::Button::Left);
    CHECK(g.edges.size() == 1);

    // The edge is hittable halfway along.
    const grapher::Vec2 mid{(ra2.Right() + rb.x) * 0.5f, (ra2.y + ra2.h * 0.5f + rb.y + rb.h * 0.5f) * 0.5f};
    CHECK(canvas.HitTest(mid).kind == grapher::HitKind::Edge);

    // A marquee over everything selects both nodes.
    canvas.PointerDown({1, 1}, grapher::Button::Left, {});
    canvas.PointerMove({799, 599});
    canvas.PointerUp({799, 599}, grapher::Button::Left);
    CHECK(canvas.SelectedNodes().size() == 2);

    // Deleting them takes the edge too.
    CHECK(canvas.DeleteSelection());
    CHECK(g.nodes.empty() && g.edges.empty());
    CHECK(!canvas.DeleteSelection());
}

void TestDraw()
{
    const grapher::Schema s = MakeSchema();
    grapher::Graph        g;
    g.nodes.push_back({"a", "repo", {0, 0, 240, 120}, {{"name", "Community Behaviors"}, {"license", "GPL-3.0"}}, "the plugin"});
    g.nodes.push_back({"n", "note", {300, 0, 200, 100}, {}, "pseudo { code }"});
    g.edges.push_back({"e", "a", "n", "depends", "uses"});
    grapher::Canvas canvas(g, s);
    canvas.SetViewport({800, 600});
    canvas.Frame();

    auto hasText = [](const grapher::DrawList& l, const std::string& text) {
        for (const auto& c : l.cmds)
            if (const auto* t = std::get_if<grapher::Text>(&c); t && t->text == text) return true;
        return false;
    };
    const grapher::DrawList live = canvas.Draw();
    CHECK(hasText(live, "Community Behaviors"));
    CHECK(hasText(live, "GPL-3.0"));
    CHECK(hasText(live, "pseudo { code }"));
    CHECK(hasText(live, "uses"));

    const grapher::DrawList all = canvas.DrawAll(1.f, 40.f);
    CHECK(all.size.x == 500.f + 80.f && all.size.y == 120.f + 80.f);
}

void TestTransitionsAndDefaults()
{
    grapher::Schema s = MakeSchema();
    s.edgeTypes.insert(s.edgeTypes.begin(), {"flow", "flows into"});
    s.edgeTypes.push_back({"relates", "relates to"});
    s.nodeTypes[1].defaultEdge = "relates";   // note
    grapher::NodeType via;
    via.id     = "via";
    via.label  = "Comes in via";
    via.shape  = grapher::NodeShape::Pill;
    via.fields = {{"how", "How", grapher::FieldKind::Tag}, {"through", "Through"}};
    s.nodeTypes.push_back(via);

    CHECK(s.EdgeFor("repo", "repo") == "flow");
    CHECK(s.EdgeFor("repo", "note") == "relates");   // either end's default wins over the first type
    CHECK(s.EdgeFor("note", "repo") == "relates");
    s.nodeTypes[1].defaultEdge = "missing";
    CHECK(s.EdgeFor("note", "repo") == "flow");      // an unknown default falls back

    grapher::Graph g;
    g.nodes.push_back({"p", "via", {0, 0, 170, 40}, {{"how", "vcpkg"}, {"through", "PORTER"}}});
    grapher::Canvas canvas(g, s);
    canvas.SetViewport({800, 600});
    bool pillText = false;
    for (const auto& c : canvas.Draw().cmds)
        if (const auto* t = std::get_if<grapher::Text>(&c); t && t->text == "vcpkg · PORTER") pillText = true;
    CHECK(pillText);
}

bool Crosses(const std::vector<grapher::Vec2>& path, const grapher::Rect& r)
{
    // Sample densely: any sample strictly inside the rectangle means the wire runs through it.
    for (std::size_t i = 1; i < path.size(); ++i)
        for (int k = 0; k <= 20; ++k) {
            const grapher::Vec2 p = path[i - 1] + (path[i] - path[i - 1]) * (static_cast<float>(k) / 20.f);
            if (p.x > r.x + 0.5f && p.x < r.Right() - 0.5f && p.y > r.y + 0.5f && p.y < r.Bottom() - 0.5f) return true;
        }
    return false;
}

void TestRouting()
{
    grapher::Schema s;
    s.nodeTypes.push_back({"box", "Box"});
    s.edgeTypes.push_back({"curve", "curve"});
    grapher::EdgeType elbow{"elbow", "elbow"};
    elbow.line = grapher::EdgeLine::Elbow;
    s.edgeTypes.push_back(elbow);
    grapher::RoutingOptions sharp;
    sharp.cornerRadius = 0.f;
    sharp.curveRadius  = 0.f;

    // A clear curve keeps its bezier.
    {
        grapher::Graph g;
        g.nodes.push_back({"a", "box", {0, 0, 100, 60}});
        g.nodes.push_back({"b", "box", {400, 0, 100, 60}});
        g.edges.push_back({"e", "a", "b", "curve"});
        const auto w = grapher::RouteWires(g, s, {});
        CHECK(w.at("e").size() == 25);
        CHECK(w.at("e").front().x == 100.f && w.at("e").back().x == 400.f);
    }

    // A node in the way: the curve detours around it, still starting and ending on the right sides.
    {
        grapher::Graph g;
        g.nodes.push_back({"a", "box", {0, 0, 100, 60}});
        g.nodes.push_back({"b", "box", {400, 0, 100, 60}});
        g.nodes.push_back({"c", "box", {200, -40, 80, 140}});
        g.edges.push_back({"e", "a", "b", "curve"});
        for (const grapher::RoutingOptions& o : {grapher::RoutingOptions{}, sharp}) {
            const auto path = grapher::RouteWires(g, s, o).at("e");   // a copy: the map is a temporary
            CHECK(!Crosses(path, g.nodes[2].frame));
            CHECK(path.front().x == 100.f && path.back().x == 400.f);
        }
        grapher::RoutingOptions off;
        off.avoidNodes = false;
        CHECK(Crosses(grapher::RouteWires(g, s, off).at("e"), g.nodes[2].frame));   // and does cross without routing
    }

    // An elbow with sharp corners is all horizontal and vertical runs, around a blocker too.
    {
        grapher::Graph g;
        g.nodes.push_back({"a", "box", {0, 0, 100, 60}});
        g.nodes.push_back({"b", "box", {400, 200, 100, 60}});
        g.nodes.push_back({"c", "box", {180, 60, 120, 140}});
        g.edges.push_back({"e", "a", "b", "elbow"});
        const auto path = grapher::RouteWires(g, s, sharp).at("e");
        bool orthogonal = true;
        for (std::size_t i = 1; i < path.size(); ++i)
            if (path[i].x != path[i - 1].x && path[i].y != path[i - 1].y) orthogonal = false;
        CHECK(orthogonal);
        CHECK(!Crosses(path, g.nodes[2].frame));
    }

    // Three wires into one side spread along it, in the order of their sources, so they don't cross at the target.
    {
        grapher::Graph g;
        g.nodes.push_back({"t", "box", {400, 0, 100, 300}});
        g.nodes.push_back({"low", "box", {0, 400, 100, 60}});
        g.nodes.push_back({"top", "box", {0, -200, 100, 60}});
        g.nodes.push_back({"mid", "box", {0, 120, 100, 60}});
        for (const char* src : {"low", "top", "mid"}) g.edges.push_back({std::string("e-") + src, src, "t", "curve"});
        const auto w = grapher::RouteWires(g, s, {});
        const float yTop = w.at("e-top").back().y, yMid = w.at("e-mid").back().y, yLow = w.at("e-low").back().y;
        CHECK(yTop < yMid && yMid < yLow);
        CHECK(w.at("e-mid").back().x == 400.f);   // all on the left side
    }
}

/// @brief A backend stand-in: every wire gets the same marker path, after a delay that makes the backend fast or slow.
class StubRouter : public grapher::WireRouter {
public:
    explicit StubRouter(int delayMs) : delayMs_(delayMs) {}
    static inline const std::vector<grapher::Vec2> kMarker{{-7.f, -7.f}, {-7.f, -9.f}, {-5.f, -9.f}};
    int calls = 0;
    grapher::WirePaths Route(const grapher::Graph& g, const grapher::Schema&, const grapher::RoutingOptions&) override
    {
        ++calls;
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_));
        grapher::WirePaths out;
        for (const grapher::Edge& e : g.edges) out.emplace(e.id, kMarker);
        return out;
    }

private:
    int delayMs_;
};

/// @brief The wires as drawn (identity camera: screen = world), told from the grid by their colour.
std::vector<std::vector<grapher::Vec2>> DrawnWires(const grapher::Canvas& canvas, grapher::Color wire)
{
    std::vector<std::vector<grapher::Vec2>> out;
    for (const auto& c : canvas.Draw().cmds)
        if (const auto* p = std::get_if<grapher::Polyline>(&c); p && p->color == wire) out.push_back(p->points);
    return out;
}

void TestBackendDrag()
{
    // Dragging a node never trades a backend's routes for plain curves. "ab" runs past the blocker c; "df" is elsewhere.
    grapher::Schema s;
    s.nodeTypes.push_back({"box", "Box"});
    grapher::EdgeType curve{"curve", "curve"};
    curve.color = {1, 2, 3};
    curve.arrow = false;
    s.edgeTypes.push_back(curve);
    for (int delayMs : {0, 30}) {
        grapher::Graph g;
        g.nodes.push_back({"a", "box", {0, 0, 100, 60}});
        g.nodes.push_back({"b", "box", {400, 0, 100, 60}});
        g.nodes.push_back({"c", "box", {200, -40, 80, 140}});
        g.nodes.push_back({"d", "box", {0, 300, 100, 60}});
        g.nodes.push_back({"f", "box", {400, 300, 100, 60}});
        g.edges.push_back({"ab", "a", "b", "curve"});
        g.edges.push_back({"df", "d", "f", "curve"});
        grapher::Canvas canvas(g, s);
        auto stub     = std::make_shared<StubRouter>(delayMs);
        canvas.router = stub;
        canvas.SetViewport({800, 600});
        CHECK(DrawnWires(canvas, curve.color).size() == 2);   // the first, full route

        canvas.PointerDown({30, 30}, grapher::Button::Left, {});
        canvas.PointerMove({30, 40});
        CHECK(canvas.Busy());
        int  before = stub->calls;
        auto wires  = DrawnWires(canvas, curve.color);   // the first step reroutes through the backend, which times it
        CHECK(stub->calls == before + 1);
        CHECK(wires.size() == 2 && wires[0] == StubRouter::kMarker && wires[1] == StubRouter::kMarker);

        canvas.PointerMove({30, 50});
        before = stub->calls;
        wires  = DrawnWires(canvas, curve.color);
        CHECK(wires.size() == 2);
        if (wires.size() != 2) continue;
        if (delayMs == 0) {
            CHECK(stub->calls == before + 1);   // quick: the backend keeps routing live
            CHECK(wires[0] == StubRouter::kMarker && wires[1] == StubRouter::kMarker);
        } else {
            CHECK(stub->calls == before);   // slow: the backend waits for the drop
            CHECK(wires[1] == StubRouter::kMarker);   // a wire away from the drag keeps its routed path
            CHECK(wires[0] != StubRouter::kMarker);   // the dragged node's wire is rerouted live, around the blocker
            CHECK(wires[0].size() >= 2 && !Crosses(wires[0], g.nodes[2].frame));
        }

        canvas.PointerUp({30, 50}, grapher::Button::Left);
        before = stub->calls;
        wires  = DrawnWires(canvas, curve.color);
        CHECK(stub->calls == before + (delayMs == 0 ? 0 : 1));   // live wires are already settled; patched ones reroute
        CHECK(wires.size() == 2 && wires[0] == StubRouter::kMarker && wires[1] == StubRouter::kMarker);   // dropped: all routed
    }
}

grapher::Schema GroupSchema()
{
    grapher::Schema s;
    grapher::NodeType box{"box", "Box"};
    box.width  = 100.f;
    box.height = 60.f;
    s.nodeTypes.push_back(box);
    grapher::NodeType folder{"folder", "Folder"};
    folder.shape  = grapher::NodeShape::Group;
    folder.fields = {{"name", "Name"}, {"path", "Path", grapher::FieldKind::Path}};
    s.nodeTypes.push_back(folder);
    grapher::EdgeType curve{"curve", "curve"};
    curve.color = {1, 2, 3};
    s.edgeTypes.push_back(curve);
    return s;
}

void TestGroupMembership()
{
    const grapher::Schema s = GroupSchema();
    grapher::Graph        g;
    g.nodes.push_back({"a", "box", {60, 100, 100, 60}});
    g.nodes.push_back({"G", "folder", {0, 0, 600, 400}});
    g.nodes.push_back({"H", "folder", {40, 60, 300, 200}});
    g.nodes.push_back({"b", "box", {400, 100, 100, 60}});
    g.nodes.push_back({"c", "box", {800, 100, 100, 60}});
    g.nodes.push_back({"half", "box", {550, 100, 100, 60}});   // sticks out of G: not a member
    CHECK(grapher::IsGroup(g.nodes[1], s) && !grapher::IsGroup(g.nodes[0], s));
    CHECK(grapher::ParentGroup(g, s, *g.FindNode("a")) == "H");   // the smallest frame wins, so groups nest
    CHECK(grapher::ParentGroup(g, s, *g.FindNode("b")) == "G");
    CHECK(grapher::ParentGroup(g, s, *g.FindNode("H")) == "G");
    CHECK(grapher::ParentGroup(g, s, *g.FindNode("G")).empty());
    CHECK(grapher::ParentGroup(g, s, *g.FindNode("c")).empty());
    CHECK(grapher::ParentGroup(g, s, *g.FindNode("half")).empty());
    CHECK((grapher::GroupMembers(g, s, "G") == std::vector<std::string>{"a", "H", "b"}));
    CHECK((grapher::GroupMembers(g, s, "H") == std::vector<std::string>{"a"}));

    // Two groups of the same frame never hold each other.
    g.nodes.push_back({"G2", "folder", {0, 0, 600, 400}});
    CHECK(!grapher::GroupHolds(g.nodes[1], g.nodes.back(), s) && !grapher::GroupHolds(g.nodes.back(), g.nodes[1], s));
}

void TestGroupGestures()
{
    const grapher::Schema s = GroupSchema();
    grapher::Graph        g;
    g.nodes.push_back({"G", "folder", {0, 0, 600, 400}, {{"name", "src"}, {"path", "D:/x/src"}}});
    g.nodes.push_back({"H", "folder", {40, 60, 300, 200}, {{"name", "inner"}}});
    g.nodes.push_back({"a", "box", {60, 100, 100, 60}});
    g.nodes.push_back({"b", "box", {400, 100, 100, 60}});
    g.nodes.push_back({"c", "box", {800, 100, 100, 60}});
    grapher::Canvas canvas(g, s);   // identity camera: screen = world
    canvas.SetViewport({1200, 800});
    int snapshots = 0, changes = 0;
    canvas.onBeforeChange = [&](std::string_view) { ++snapshots; };
    canvas.onChanged      = [&] { ++changes; };
    auto frame = [&](const char* id) { return g.FindNode(id)->frame; };
    auto drag  = [&](grapher::Vec2 from, grapher::Vec2 to) {
        canvas.PointerDown(from, grapher::Button::Left, {});
        canvas.PointerMove({(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f});
        canvas.PointerMove(to);
        canvas.PointerUp(to, grapher::Button::Left);
    };

    // A group is hit by its header; its empty interior is empty canvas, an inner group's header is the inner group.
    CHECK(canvas.HitTest({300, 10}).kind == grapher::HitKind::Node && canvas.HitTest({300, 10}).id == "G");
    CHECK(canvas.HitTest({300, 350}).kind == grapher::HitKind::None);
    CHECK(canvas.HitTest({200, 70}).id == "H");
    CHECK(canvas.HitTest({200, 240}).kind == grapher::HitKind::None);
    CHECK(canvas.HitTest({590, 390}).kind == grapher::HitKind::Resize);   // the grip still works
    CHECK(canvas.HitTest({100, 130}).id == "a");                          // a member is over its group

    // A press in the interior starts a marquee: it picks what it touches inside, and a group only when it holds all of it.
    drag({380, 80}, {590, 390});
    CHECK((canvas.SelectedNodes() == grapher::NodeSet{"b"}));
    drag({30, 50}, {350, 270});
    CHECK((canvas.SelectedNodes() == grapher::NodeSet{"H", "a"}));
    CHECK(snapshots == 0);

    // Dragging a group by its header moves everything it holds, nested groups included, in one snapshot.
    drag({300, 10}, {350, 60});
    CHECK(snapshots == 1 && changes == 1);
    CHECK((canvas.SelectedNodes() == grapher::NodeSet{"G"}));
    CHECK(frame("G").x == 50.f && frame("G").y == 50.f);
    CHECK(frame("H").x == 90.f && frame("a").x == 110.f && frame("a").y == 150.f && frame("b").x == 450.f);
    CHECK(frame("c").x == 800.f);   // outside: left alone

    // Dropping a node with its centre in a group's frame puts it wholly inside, below the header.
    drag({820, 120}, {590, 120});   // c lands at x 570: its centre is in G, its right edge out
    CHECK(frame("c").x == 540.f && frame("c").y == 100.f);   // G's right 650, less the padding, less c's width
    CHECK(grapher::ParentGroup(g, s, *g.FindNode("c")) == "G");
    CHECK(snapshots == 2);

    // Moving the group now takes c along; dragging c out by its centre takes it out.
    drag({300, 60}, {300, 80});
    CHECK(frame("c").y == 120.f && frame("G").y == 70.f);
    drag({560, 140}, {960, 140});
    CHECK(frame("c").x == 940.f && grapher::ParentGroup(g, s, *g.FindNode("c")).empty());

    // A group dropped half into a bigger one goes in with what it holds.
    g.nodes.push_back({"F", "folder", {1000, 500, 200, 150}});
    g.nodes.push_back({"f", "box", {1020, 540, 100, 60}});
    drag({1100, 510}, {600, 360});   // F to (500, 350): its centre in G (50..650, 70..470), its corner out
    CHECK(frame("F").x == 440.f && frame("F").y == 310.f);   // G's right less padding less F's width; bottom likewise
    CHECK(frame("f").x == 460.f && frame("f").y == 350.f);   // f keeps its place in F
    CHECK(grapher::ParentGroup(g, s, *g.FindNode("F")) == "G" && grapher::ParentGroup(g, s, *g.FindNode("f")) == "F");
    g.RemoveNodes({"F", "f"});

    // A node added with its centre in a group lands inside it (the smallest one).
    const grapher::Rect h = frame("H");   // {90, 130, 300, 200}
    const std::string   n = canvas.AddNode("box", {h.Right() - 10.f, h.y + 100.f});
    CHECK(grapher::ParentGroup(g, s, *g.FindNode(n)) == "H");
    CHECK(frame(n.c_str()).Right() == h.Right() - canvas.style.padding);

    // A connection dropped in a group's interior ends on the group.
    const grapher::Rect rc = frame("c");
    canvas.PointerDown({rc.Right(), rc.y + rc.h * 0.5f}, grapher::Button::Left, {});
    canvas.PointerMove({600, 440});
    canvas.PointerUp({600, 440}, grapher::Button::Left);
    CHECK(g.edges.size() == 1 && g.edges[0].from == "c" && g.edges[0].to == "G");
}

void TestGroupDraw()
{
    const grapher::Schema s = GroupSchema();
    grapher::Graph        g;
    g.nodes.push_back({"a", "box", {60, 100, 100, 60}});
    g.nodes.push_back({"G", "folder", {0, 0, 600, 400}, {{"name", "src"}, {"path", "D:/x/src"}}});
    g.nodes.push_back({"b", "box", {400, 100, 100, 60}});
    g.edges.push_back({"e", "a", "b", "curve"});
    grapher::Canvas canvas(g, s);
    canvas.SetViewport({800, 600});
    canvas.SelectNodes({"G"});   // selected nodes draw last; a selected group still stays behind

    const grapher::DrawList l = canvas.Draw();
    std::size_t group = 0, edge = 0, nodeA = 0, title = 0, path = 0, grip = 0;
    for (std::size_t i = 0; i < l.cmds.size(); ++i) {
        if (const auto* r = std::get_if<grapher::FillRect>(&l.cmds[i])) {
            if (r->rect == g.nodes[1].frame && !group) group = i;
            if (r->rect == g.nodes[0].frame && !nodeA) nodeA = i;
        }
        if (const auto* p = std::get_if<grapher::Polyline>(&l.cmds[i]); p && p->color == s.edgeTypes[0].color && !edge) edge = i;
        if (const auto* p = std::get_if<grapher::Polygon>(&l.cmds[i]); p && p->fill == canvas.style.selection) grip = i;
        if (const auto* t = std::get_if<grapher::Text>(&l.cmds[i])) {
            if (t->text == "src" && t->bold && t->box.y == 0.f && t->box.h == canvas.style.groupHeader) title = i;
            if (t->text == "D:/x/src") path = i;
        }
    }
    CHECK(group > 0 && edge > group && nodeA > edge);   // the frame under the wires, the wires under the cards
    CHECK(title > group && title < edge && path > title);   // a header label on the frame
    CHECK(grip > nodeA);                                    // its handles over what it holds
    const auto& frameCmd = std::get<grapher::FillRect>(l.cmds[group]);
    CHECK(frameCmd.fill.a == canvas.style.groupFill && frameCmd.stroke == canvas.style.selection);
}

void TestGroupRouting()
{
    // Groups are no obstacles: a wire runs over one, into what it holds, and onto the group itself.
    const grapher::Schema s = GroupSchema();
    grapher::Graph        g;
    g.nodes.push_back({"a", "box", {0, 0, 100, 60}});
    g.nodes.push_back({"b", "box", {400, 0, 100, 60}});
    g.nodes.push_back({"G", "folder", {150, -100, 200, 260}});
    g.nodes.push_back({"m", "box", {200, 300, 100, 60}});
    g.nodes.push_back({"M", "folder", {180, 250, 140, 140}});   // holds m
    g.edges.push_back({"ab", "a", "b", "curve"});
    g.edges.push_back({"am", "a", "m", "curve"});
    g.edges.push_back({"aG", "a", "G", "curve"});
    const auto w = grapher::RouteWires(g, s, {});
    CHECK(w.at("ab").size() == 25);   // its plain bezier, straight over G
    CHECK(Crosses(w.at("ab"), g.nodes[2].frame));
    CHECK(w.at("am").size() == 25);   // into a member, over its group's outline
    CHECK(w.contains("aG") && w.at("aG").back().x == 150.f);   // onto the group's facing side

    // The same frame as a card is an obstacle again.
    g.nodes[2].type = "box";
    CHECK(!Crosses(grapher::RouteWires(g, s, {}).at("ab"), g.nodes[2].frame));
}

// ── Ports ────────────────────────────────────────────────────────────────────

bool Near(grapher::Vec2 a, grapher::Vec2 b) { return grapher::Distance(a, b) < 0.01f; }

constexpr grapher::Color kFloat{200, 200, 200};
constexpr grapher::Color kRgba{230, 120, 40};

/// @brief A small material-like library: Value (a float out on its title line), Mix (two colours and a number in, a
///        colour out) and Flag (a bool in). Floats are numbers, and a float converts to a colour.
grapher::Schema PortSchema()
{
    grapher::Schema s;
    s.portTypes = {{"number", "Number"},
                   {"float", "Float", kFloat, grapher::PortGlyph::Circle, {"number"}},
                   {"rgba", "Colour", kRgba, grapher::PortGlyph::Diamond},
                   {"bool", "Bool"}};
    s.conversions = {{"float", "rgba"}};
    grapher::NodeType mix{"mix", "Mix"};
    mix.width  = 200.f;
    mix.height = 120.f;
    mix.ports  = {{"a", "A", "rgba"}, {"b", "B", "rgba"}, {"t", "T", "number"}, {"out", "Out", "rgba", grapher::PortDir::Out}};
    grapher::NodeType value{"value", "Value"};
    value.width  = 160.f;
    value.height = 80.f;
    value.ports  = {{"out", "", "float", grapher::PortDir::Out, grapher::PortSide::Auto, -1, "", true}};
    grapher::NodeType flag{"flag", "Flag"};
    flag.ports = {{"in", "In", "bool"}};
    s.nodeTypes = {mix, value, flag};
    s.edgeTypes = {{"data", "data"}};
    return s;
}

void TestPortTypes()
{
    grapher::Schema s = PortSchema();
    CHECK(s.IsA("float", "number"));
    CHECK(!s.IsA("number", "float"));
    CHECK(s.CanConnect("float", "number"));    // a float flows into a number input
    CHECK(!s.CanConnect("number", "float"));
    CHECK(s.CanConnect("float", "rgba"));      // by the listed conversion
    CHECK(!s.CanConnect("rgba", "float"));     // conversions run one way
    CHECK(!s.CanConnect("bool", "rgba"));
    CHECK(s.CanConnect("", "bool") && s.CanConnect("bool", ""));   // an untyped end takes anything

    s.subtypeFlow = grapher::SubtypeFlow::InIsA;   // a reference graph: a pointer to a number takes a float
    CHECK(s.CanConnect("number", "float"));
    CHECK(!s.CanConnect("float", "number"));

    s.portTypes.push_back({"x", "X", {}, grapher::PortGlyph::Circle, {"y"}});
    s.portTypes.push_back({"y", "Y", {}, grapher::PortGlyph::Circle, {"x"}});
    CHECK(s.IsA("x", "y") && !s.IsA("x", "number"));   // a cycle in the bases ends

    const auto before = s.revision;
    grapher::NodeType mix = *s.FindNode("mix");
    mix.label = "Blend";
    s.PutNodeType(mix);
    CHECK(s.nodeTypes.size() == 3 && s.FindNode("mix")->label == "Blend" && s.revision == before + 1);
    s.PutNodeType({"noise", "Noise"});
    CHECK(s.nodeTypes.size() == 4 && s.revision == before + 2);
}

void TestPortLayout()
{
    const grapher::Schema      s = PortSchema();
    const grapher::CardMetrics m;   // Style's defaults: padding 10, title 15, text 12
    const float header = 8.f + 15.f * 1.35f + 12.f * 1.5f, row = 12.f * 1.4f;
    CHECK(std::abs(m.Header() - header) < 0.001f && std::abs(m.Row() - row) < 0.001f);

    grapher::Node n{"m", "mix", {0, 0, 200, 120}};
    auto slots = grapher::LayoutPorts(n, s, m);
    CHECK(slots.size() == 4);
    if (slots.size() != 4) return;
    CHECK(slots[0].side == grapher::PortSide::Left && Near(slots[0].at, {0, header + row * 0.5f}));
    CHECK(Near(slots[1].at, {0, header + row * 1.5f}));
    CHECK(Near(slots[2].at, {0, header + row * 2.5f}));
    CHECK(slots[3].side == grapher::PortSide::Right && Near(slots[3].at, {200, header + row * 0.5f}));   // pairs with A
    CHECK(Near(slots[3].normal, {1, 0}) && Near(slots[0].normal, {-1, 0}));
    CHECK(slots[0].row == (grapher::Rect{0, header, 200, row}));
    CHECK(grapher::PortRows(n, s) == 3);
    CHECK(std::abs(grapher::CardHeight(n, s, m) - (header + row * 3 + 10.f)) < 0.001f);

    grapher::Node v{"v", "value", {50, 20, 160, 80}};
    slots = grapher::LayoutPorts(v, s, m);
    CHECK(slots.size() == 1 && Near(slots[0].at, {210, 20 + 8.f + 15.f * 0.65f}) && slots[0].row.h == 0.f);   // the title line
    CHECK(grapher::PortRows(v, s) == 0);

    // A node's own ports follow its type's; one with a type port's id replaces it in place.
    n.ports = {{"c", "C", "rgba"}, {"a", "Alpha", "float"}, {"top", "", "", grapher::PortDir::In, grapher::PortSide::Top}};
    slots   = grapher::LayoutPorts(n, s, m);
    CHECK(slots.size() == 6);
    if (slots.size() != 6) return;
    CHECK(slots[0].def.id == "a" && slots[0].def.label == "Alpha" && slots[0].def.type == "float");
    CHECK(slots[4].def.id == "c" && Near(slots[4].at, {0, header + row * 3.5f}));
    CHECK(slots[5].def.id == "top" && Near(slots[5].at, {100, 0}) && Near(slots[5].normal, {0, -1}));
    CHECK(grapher::PortRows(n, s) == 4);

    // Off a card, a side's ports spread along it.
    grapher::Schema pill = s;
    pill.nodeTypes[0].shape = grapher::NodeShape::Pill;
    n.ports.clear();
    slots = grapher::LayoutPorts(n, pill, m);
    CHECK(slots.size() == 4 && Near(slots[0].at, {0, 30}) && Near(slots[1].at, {0, 60}) && Near(slots[3].at, {200, 60}));
}

int CountEdges(const grapher::Graph& g, std::string_view to, std::string_view port)
{
    int n = 0;
    for (const grapher::Edge& e : g.edges) n += e.to == to && e.toPort == port;
    return n;
}

const grapher::Edge* EdgeInto(const grapher::Graph& g, std::string_view to, std::string_view port)
{
    for (const grapher::Edge& e : g.edges)
        if (e.to == to && e.toPort == port) return &e;
    return nullptr;
}

void Drag(grapher::Canvas& c, grapher::Vec2 from, grapher::Vec2 to)
{
    c.PointerDown(from, grapher::Button::Left, {});
    c.PointerMove(from + (to - from) * 0.5f);
    c.PointerMove(to);
    c.PointerUp(to, grapher::Button::Left);
}

void TestPortGestures()
{
    const grapher::Schema s = PortSchema();
    grapher::Graph g;
    g.nodes.push_back({"v1", "value", {0, 0, 160, 80}});
    g.nodes.push_back({"v2", "value", {0, 200, 160, 80}});
    g.nodes.push_back({"m", "mix", {400, 0, 200, 120}});
    g.nodes.push_back({"m2", "mix", {400, 300, 200, 120}});
    g.nodes.push_back({"f", "flag", {800, 0, 160, 100}});
    grapher::Canvas c(g, s);
    c.SetViewport({1200, 800});   // identity camera: screen = world
    int snapshots = 0;
    c.onBeforeChange = [&](std::string_view) { ++snapshots; };

    const auto v1out = c.PortScreenPos("v1", "out"), ma = c.PortScreenPos("m", "a");
    CHECK(v1out && ma && !c.PortScreenPos("v1", "nope"));
    if (!v1out || !ma) return;
    const grapher::Hit hit = c.HitTest(*v1out);
    CHECK(hit.kind == grapher::HitKind::Port && hit.id == "v1" && hit.port == "out");
    CHECK(c.HitTest(c.ScreenRectOf("m").Center()).kind == grapher::HitKind::Node);
    CHECK(c.HitTest({c.ScreenRectOf("m").Right(), c.ScreenRectOf("m").Center().y}).kind != grapher::HitKind::Connect);   // no handle

    // Port to port: a float into a colour input, through the conversion.
    Drag(c, *v1out, *ma);
    CHECK(g.edges.size() == 1 && snapshots == 1);
    if (g.edges.size() != 1) return;
    CHECK(g.edges[0].from == "v1" && g.edges[0].fromPort == "out" && g.edges[0].to == "m" && g.edges[0].toPort == "a");
    CHECK(g.edges[0].type == "data");

    CHECK(!c.CanWire("v1", "out", "m", "a"));    // the same wire twice
    CHECK(!c.CanWire("m", "out", "v1", "out"));   // two outputs
    CHECK(!c.CanWire("v1", "out", "f", "in"));    // a float is no bool
    CHECK(!c.CanWire("m", "a", "m", "out"));      // a node to itself
    CHECK(c.CanWire("m", "t", "v1", "out"));      // either order: the output end leads

    // A full input gives its wire up to the new one; an output feeds any number.
    CHECK(!c.Connect("v2", "out", "m", "a").empty());
    CHECK(CountEdges(g, "m", "a") == 1 && EdgeInto(g, "m", "a")->from == "v2");
    CHECK(!c.Connect("m", "b", "v2", "out").empty());   // dragged the other way round
    const grapher::Edge* mb = EdgeInto(g, "m", "b");
    CHECK(mb && mb->from == "v2" && mb->fromPort == "out");
    CHECK(c.Connect("v1", "out", "f", "in").empty() && g.edges.size() == 2);

    // Dropped on a node as a whole: the first input that takes it, one with room before a full one.
    const grapher::Vec2 body = c.ScreenRectOf("m2").Center();
    const auto v2out = c.PortScreenPos("v2", "out");
    Drag(c, *v2out, body);
    CHECK(CountEdges(g, "m2", "a") == 1);
    Drag(c, *v1out, body);
    CHECK(CountEdges(g, "m2", "b") == 1 && EdgeInto(g, "m2", "b")->from == "v1");
    Drag(c, *v1out, c.ScreenRectOf("f").Center());   // nothing on Flag takes a float
    CHECK(CountEdges(g, "f", "in") == 0);

    // From an input to an output, ringing the ports it may land on while it drags.
    const auto m2t = c.PortScreenPos("m2", "t");
    c.PointerDown(*m2t, grapher::Button::Left, {});
    c.PointerMove(*m2t - grapher::Vec2{40, 0});
    c.PointerMove(*v1out);
    int rings = 0;
    for (const auto& cmd : c.Draw().cmds)
        if (const auto* ring = std::get_if<grapher::Circle>(&cmd); ring && ring->stroke == c.style.selection && ring->fill.a == 0) ++rings;
    CHECK(rings == 2);   // the two Values' outputs
    c.PointerUp(*v1out, grapher::Button::Left);
    const grapher::Edge* m2t_edge = EdgeInto(g, "m2", "t");
    CHECK(m2t_edge && m2t_edge->from == "v1" && m2t_edge->fromPort == "out");

    // A new card is tall enough for its ports.
    const std::string added = c.AddNode("mix", {700, 600});
    const grapher::Node* n  = g.FindNode(added);
    CHECK(n && n->frame.h >= grapher::CardHeight(*n, s, grapher::MetricsOf(c.style)));
}

void TestPortDrawAndRouting()
{
    const grapher::Schema s = PortSchema();
    grapher::Graph g;
    g.nodes.push_back({"v", "value", {0, 0, 160, 80}});
    g.nodes.push_back({"m", "mix", {400, 100, 200, 120}});
    g.edges.push_back({"e", "v", "m", "data", "", "out", "a"});
    grapher::Canvas c(g, s);
    c.SetViewport({800, 600});
    const auto out = c.PortScreenPos("v", "out"), a = c.PortScreenPos("m", "a"), b = c.PortScreenPos("m", "b");

    // The wire runs from pin to pin, whatever side the nodes face each other on.
    const auto w = grapher::RouteWires(g, s, {});
    CHECK(w.contains("e"));
    if (!w.contains("e") || !out || !a || !b) return;
    CHECK(Near(w.at("e").front(), *out) && Near(w.at("e").back(), *a));
    g.nodes[1].frame.y = -300;   // now above: a node-level end would take the top side, a port end stays put
    const auto w2 = grapher::RouteWires(g, s, {});
    CHECK(Near(w2.at("e").back(), *c.PortScreenPos("m", "a")));
    g.nodes[1].frame.y = 100;

    // Pins: filled in their type's colour while wired, hollow otherwise; an rgba port is a diamond.
    bool wiredFloat = false, wiredDiamond = false, hollowDiamond = false;
    for (const auto& cmd : c.Draw().cmds) {
        if (const auto* p = std::get_if<grapher::Circle>(&cmd); p && Near(p->center, *out)) wiredFloat = p->fill == kFloat;
        if (const auto* p = std::get_if<grapher::Polygon>(&cmd); p && p->points.size() == 4) {
            const grapher::Vec2 mid = (p->points[0] + p->points[2]) * 0.5f;
            if (Near(mid, *a)) wiredDiamond = p->fill == kRgba;
            if (Near(mid, *b)) hollowDiamond = p->fill == c.style.nodeFill;
        }
    }
    CHECK(wiredFloat && wiredDiamond && hollowDiamond);

    // A row shows its port's label; a port bound to a field shows the value while unwired.
    grapher::Schema lit = s;
    lit.nodeTypes[0].fields = {{"amount", "Amount"}};
    lit.nodeTypes[0].ports[2].field = "amount";
    g.nodes[1].fields["amount"] = "0.5";
    c.SetSchema(lit);
    bool label = false, literal = false, repeated = false;
    for (const auto& cmd : c.Draw().cmds)
        if (const auto* t = std::get_if<grapher::Text>(&cmd)) {
            label    = label || t->text == "B";
            literal  = literal || t->text == "T  0.5";
            repeated = repeated || t->text == "Amount";   // not listed again as a field
        }
    CHECK(label && literal && !repeated);
}

void TestRerouteAndWireColour()
{
    // A reroute dot carries whatever is wired into it: a float through it reaches a number input but not a bool one,
    // and a wire whose type says so takes its port type's colour, through the reroute too.
    grapher::Schema s = PortSchema();
    grapher::NodeType dot{"reroute", "Reroute"};
    dot.shape  = grapher::NodeShape::Dot;
    dot.width  = dot.height = 16.f;
    dot.ports  = {{"in", ""}, {"out", "", "", grapher::PortDir::Out}};
    dot.ports[1].typeFrom = "in";
    s.nodeTypes.push_back(dot);
    s.edgeTypes[0].portColor = true;
    grapher::Graph g;
    g.nodes.push_back({"v", "value", {0, 0, 160, 80}});
    g.nodes.push_back({"r", "reroute", {250, 100, 16, 16}});
    g.nodes.push_back({"m", "mix", {400, 0, 200, 120}});
    g.nodes.push_back({"f", "flag", {400, 300, 160, 100}});
    grapher::Canvas c(g, s);
    c.SetViewport({800, 600});

    CHECK(grapher::PortTypeOf(g, s, g.nodes[1], "out").empty());   // nothing in yet: it takes anything
    CHECK(c.CanWire("r", "out", "f", "in"));
    CHECK(!c.Connect("v", "out", "r", "in").empty());
    CHECK(grapher::PortTypeOf(g, s, g.nodes[1], "out") == "float");
    CHECK(!c.CanWire("r", "out", "f", "in"));   // a float is no bool
    CHECK(c.CanWire("r", "out", "m", "t"));
    const std::string e = c.Connect("r", "out", "m", "t");
    CHECK(!e.empty());

    // Two wires between the same two nodes, on different ports.
    CHECK(!c.Connect("r", "out", "m", "a").empty());
    int between = 0;
    for (const grapher::Edge& x : g.edges) between += x.from == "r" && x.to == "m";
    CHECK(between == 2);

    // Both wires out of the reroute draw in the float's colour; the dot is a disc in its band colour.
    int floatWires = 0;
    bool disc = false;
    for (const auto& cmd : c.Draw().cmds) {
        if (const auto* p = std::get_if<grapher::Polyline>(&cmd); p && p->color == kFloat && p->points.size() > 2) ++floatWires;
        if (const auto* d = std::get_if<grapher::Circle>(&cmd); d && grapher::Distance(d->center, {258, 108}) < 0.01f && d->radius == 8.f)
            disc = true;
    }
    CHECK(floatWires == 3 && disc);   // v -> r, r -> m.t, r -> m.a
}

grapher::Schema RowSchema();

void TestSchemaJson()
{
    const char* text = R"({
        "subtypeFlow": "in-is-a",
        "portTypes": [ { "id": "float", "color": "#a0a0a0", "bases": ["number"], "edge": "data" },
                       { "id": "number", "label": "Number" } ],
        "conversions": [ { "from": "float", "to": "rgba" } ],
        "nodes": [ { "id": "mix", "label": "Mix é", "band": "#4f8cff", "width": 220, "shape": "card",
                     "fields": [ { "id": "amount", "kind": "tag", "choices": ["a", "b"], "onCard": false } ],
                     "ports": [ { "id": "a", "type": "float" },
                                { "id": "out", "label": "", "dir": "out", "side": "bottom", "maxWires": 0, "onHeader": true,
                                  "typeFrom": "a" } ] },
                   { "id": "dot", "shape": "dot" } ],
        "edges": [ { "id": "data", "color": "#112233", "line": "elbow", "dashed": true, "arrow": false, "portColor": true } ]
    })";
    const auto s = grapher::ParseSchema(text);
    CHECK(s.has_value());
    if (!s) {
        std::fprintf(stderr, "  %s\n", s.error().c_str());
        return;
    }
    CHECK(s->subtypeFlow == grapher::SubtypeFlow::InIsA);
    CHECK(s->portTypes.size() == 2 && s->portTypes[0].label == "float" && s->portTypes[0].bases == std::vector<std::string>{"number"});
    CHECK(s->portTypes[0].color == (grapher::Color{0xa0, 0xa0, 0xa0}) && s->portTypes[0].edge == "data");
    CHECK(s->conversions.size() == 1 && s->conversions[0].to == "rgba");
    const grapher::NodeType* mix = s->FindNode("mix");
    CHECK(mix && mix->label == "Mix \xc3\xa9" && mix->width == 220.f && mix->height == 120.f);
    if (!mix) return;
    CHECK(mix->fields.size() == 1 && mix->fields[0].kind == grapher::FieldKind::Tag && !mix->fields[0].onCard && mix->fields[0].label == "amount");
    CHECK(mix->ports.size() == 2 && mix->ports[0].label == "a" && mix->ports[0].dir == grapher::PortDir::In && mix->ports[0].maxWires == -1);
    CHECK(mix->ports[1].label.empty() && mix->ports[1].dir == grapher::PortDir::Out && mix->ports[1].side == grapher::PortSide::Bottom);
    CHECK(mix->ports[1].maxWires == 0 && mix->ports[1].onHeader && mix->ports[1].typeFrom == "a");
    CHECK(s->FindNode("dot") && s->FindNode("dot")->shape == grapher::NodeShape::Dot);
    CHECK(s->edgeTypes.size() == 1 && s->edgeTypes[0].line == grapher::EdgeLine::Elbow && s->edgeTypes[0].dashed && !s->edgeTypes[0].arrow);
    CHECK(s->edgeTypes[0].portColor);

    // Written and read back, a schema is the same; defaults are left out.
    for (const grapher::Schema& each : {*s, PortSchema(), MakeSchema(), GroupSchema(), RowSchema()}) {
        const std::string written = grapher::WriteSchema(each);
        const auto        back    = grapher::ParseSchema(written);
        CHECK(back.has_value() && *back == each);
    }
    CHECK(grapher::WriteSchema(MakeSchema()).find("\"width\"") == std::string::npos);

    // What can't be read says where and why.
    auto bad = grapher::ParseSchema("{\n  \"nodes\": [ }");
    CHECK(!bad && bad.error().find("line 2") != std::string::npos);
    bad = grapher::ParseSchema(R"({"nodes": [ {"id": "x", "shape": "blob"} ]})");
    CHECK(!bad && bad.error().find("blob") != std::string::npos && bad.error().find("node type x") != std::string::npos);
    bad = grapher::ParseSchema(R"({"nodes": [ {"label": "x"} ]})");
    CHECK(!bad && bad.error().find("has no id") != std::string::npos);
    bad = grapher::ParseSchema(R"({"nodes": [ {"id": "x", "ports": [ {"id": "p", "dir": "sideways"} ]} ]})");
    CHECK(!bad && bad.error().find("sideways") != std::string::npos);
    bad = grapher::ParseSchema(R"({"nodes": [ {"id": "x", "width": "wide"} ]})");
    CHECK(!bad && bad.error().find("width") != std::string::npos);
    CHECK(!grapher::ParseSchema("[]"));

    // A node type defined at runtime, from data.
    grapher::Schema lib = PortSchema();
    const auto noise = grapher::ParseNodeType(R"({"id": "noise", "ports": [ {"id": "out", "dir": "out", "type": "float"} ]})");
    CHECK(noise.has_value());
    if (!noise) return;
    lib.PutNodeType(*noise);
    grapher::Graph g;
    g.nodes.push_back({"n", "noise", {0, 0, 160, 80}});
    g.nodes.push_back({"m", "mix", {300, 0, 200, 120}});
    grapher::Canvas c(g, lib);
    CHECK(!c.Connect("n", "out", "m", "t").empty());
}

// ── Rows, proposals, navigation ─────────────────────────────────────────────

int CountText(const grapher::DrawList& list, std::string_view text)
{
    int n = 0;
    for (const auto& cmd : list.cmds)
        if (const auto* t = std::get_if<grapher::Text>(&cmd); t && t->text == text) ++n;
    return n;
}

grapher::Schema RowSchema()
{
    grapher::Schema s;
    grapher::NodeType fact{"fact", "Fact"};
    fact.width       = 200.f;
    fact.height      = 160.f;
    fact.imageHeight = 40.f;
    s.nodeTypes.push_back(fact);
    s.rowStyles.push_back({"key", grapher::Color{220, 180, 60}, std::nullopt, "+ ", true});
    s.edgeTypes.push_back({"data", "data"});
    return s;
}

grapher::Node RowNode()
{
    grapher::Node n{"n", "fact", {0, 0, 200, 170}};
    n.ports   = {{"out", "", "", grapher::PortDir::Out}};
    n.tooltip = "node tip";
    n.image   = "thumb";
    grapher::Row speed{"r1", "speed", "1.0"};
    speed.tooltip = "speed tip";
    grapher::Row mode{"c", "mode", "a", "key"};
    mode.action  = grapher::RowAction::Choice;
    mode.choices = {"a", "b"};
    mode.field   = "mode";
    grapher::Row play{"b", "play", "\xe2\x96\xb6"};
    play.action = grapher::RowAction::Button;
    grapher::Row child{"p", "child"};
    child.port      = "out";
    child.separator = true;
    n.rows = {speed, mode, play, child};
    return n;
}

void TestRows()
{
    const grapher::Schema s = RowSchema();
    grapher::Graph g;
    g.nodes.push_back(RowNode());
    grapher::Canvas c(g, s);
    c.SetViewport({800, 600});
    int changed = 0;
    c.onChanged = [&] { ++changed; };
    const grapher::CardMetrics m;
    const float header = m.Header(), row = m.Row();

    // Rows follow the free port rows (none: the only port is claimed by a row) and carry their port's pin.
    CHECK(grapher::PortRows(g.nodes[0], s) == 0 && std::abs(grapher::RowsTop(g.nodes[0], s, m) - header) < 0.001f);
    const auto out = c.PortScreenPos("n", "out");
    CHECK(out && Near(*out, {200, header + row * 3.5f}));
    CHECK(std::abs(grapher::CardHeight(g.nodes[0], s, m) - (header + row * 4 + 40 + 5 + 10)) < 0.001f);
    const auto line = c.RowScreenRect("n", "c");
    CHECK(line && line->y == header + row);

    // What is under the pointer, and what it says.
    const grapher::Hit hit = c.HitTest({100, header + row * 1.5f});
    CHECK(hit.kind == grapher::HitKind::Row && hit.id == "n" && hit.row == "c");
    CHECK(c.TooltipAt({100, header + row * 0.5f}) == "speed tip");
    CHECK(c.TooltipAt({100, header + row * 2.5f}) == "node tip");
    CHECK(c.TooltipAt({100, 10}) == "node tip");

    // A click on a plain row selects its node; a click on a choice is handed over, and a pick sets its field.
    c.PointerDown({100, header + row * 0.5f}, grapher::Button::Left, {});
    c.PointerUp({100, header + row * 0.5f}, grapher::Button::Left);
    CHECK(c.SelectedNodes() == grapher::NodeSet{"n"} && !c.TakeRowClick());
    c.PointerDown({100, header + row * 1.5f}, grapher::Button::Left, {});
    c.PointerUp({100, header + row * 1.5f}, grapher::Button::Left);
    const auto click = c.TakeRowClick();
    CHECK(click && click->row == "c" && click->action == grapher::RowAction::Choice && !c.TakeRowClick());
    CHECK(c.PickChoice("n", "c", "b") && g.nodes[0].Field("mode") == "b" && g.nodes[0].rows[1].value == "b" && changed == 1);
    CHECK(!c.PickChoice("n", "r1", "x"));
    // Dragged, a row moves its node like any press.
    c.PointerDown({100, header + row * 2.5f}, grapher::Button::Left, {});
    c.PointerMove({130, header + row * 2.5f});
    c.PointerUp({150, header + row * 2.5f}, grapher::Button::Left);
    CHECK(g.nodes[0].frame.x == 50.f && !c.TakeRowClick());

    // Drawn: styled labels, values, the choice's mark, the button, one label for the port's row, the picture.
    const auto list = c.Draw();
    CHECK(CountText(list, "speed") == 1 && CountText(list, "1.0") == 1 && CountText(list, "+ mode") == 1);
    CHECK(CountText(list, "b \xe2\x96\xbe") == 1 && CountText(list, "\xe2\x96\xb6") == 1 && CountText(list, "child") == 1);
    bool picture = false, gold = false;
    for (const auto& cmd : list.cmds) {
        if (const auto* i = std::get_if<grapher::Image>(&cmd)) picture = i->key == "thumb" && i->box.h == 40.f;
        if (const auto* t = std::get_if<grapher::Text>(&cmd); t && t->text == "+ mode") gold = t->bold && t->color == (grapher::Color{220, 180, 60});
    }
    CHECK(picture && gold);

    // Far out, only shapes.
    c.camera.zoom = 0.2f;
    bool anyText = false;
    for (const auto& cmd : c.Draw().cmds) anyText = anyText || std::holds_alternative<grapher::Text>(cmd);
    CHECK(!anyText);
}

void TestPropose()
{
    const grapher::Schema s = PortSchema();
    grapher::Graph g;
    g.nodes.push_back({"v1", "value", {0, 0, 160, 80}});
    g.nodes.push_back({"v2", "value", {0, 200, 160, 80}});
    g.nodes.push_back({"m", "mix", {400, 0, 200, 120}});
    g.edges.push_back({"old", "v2", "m", "data", "", "out", "a"});
    const grapher::Graph before = g;
    grapher::Canvas c(g, s);
    c.SetViewport({1000, 700});
    c.editMode = grapher::EditMode::Propose;
    int snapshots = 0, changes = 0;
    std::vector<grapher::Intent> intents;
    c.onBeforeChange = [&](std::string_view) { ++snapshots; };
    c.onChanged      = [&] { ++changes; };
    c.onIntent       = [&](const grapher::Intent& i) { intents.push_back(i); };

    // A move plays out on a copy and arrives as the frames that changed.
    c.PointerDown({80, 60}, grapher::Button::Left, {});
    c.PointerMove({130, 85});
    CHECK(c.Proposing() && &c.GetGraph() != &g && c.GetGraph().FindNode("v1")->frame.x == 50.f);
    c.PointerUp({180, 110}, grapher::Button::Left);
    CHECK(g == before && snapshots == 0 && changes == 0 && intents.size() == 1);
    if (intents.size() != 1) return;
    const auto* move = std::get_if<grapher::MoveNodes>(&intents[0].change);
    CHECK(intents[0].label == "Move" && move && move->frames.size() == 1);
    CHECK(move && move->frames[0].first == "v1" && move->frames[0].second == (grapher::Rect{100, 50, 160, 80}));
    CHECK(c.GetGraph().FindNode("v1")->frame.x == 100.f);   // the proposal shows until the host refreshes
    g.nodes[0].frame = move->frames[0].second;   // the host takes it
    c.Refresh();
    CHECK(!c.Proposing() && &c.GetGraph() == &g);

    // A wire into a full input names the wire it replaces.
    intents.clear();
    const std::string id = c.Connect("v1", "out", "m", "a");
    const auto* wire = intents.size() == 1 ? std::get_if<grapher::ConnectWire>(&intents[0].change) : nullptr;
    CHECK(!id.empty() && wire && wire->edge.id == id && wire->edge.from == "v1" && wire->replaced == std::vector<std::string>{"old"});
    CHECK(g.edges.size() == 1 && g.edges[0].id == "old");
    c.Refresh();

    // Removing a node names every wire going with it; an added node arrives whole.
    intents.clear();
    c.SelectNodes({"m"});
    CHECK(c.DeleteSelection());
    const auto* gone = intents.size() == 1 ? std::get_if<grapher::RemoveItems>(&intents[0].change) : nullptr;
    CHECK(gone && gone->nodes == grapher::NodeSet{"m"} && gone->edges == std::vector<std::string>{"old"} && g.nodes.size() == 3);
    c.Refresh();
    CHECK(g.FindNode("m") && c.SelectedNodes().empty());   // still there (the host never removed it), no longer selected
    const std::string added = c.AddNode("mix", {700, 400});
    const auto* made = std::get_if<grapher::CreateNode>(&intents.back().change);
    CHECK(made && made->node.id == added && made->node.type == "mix" && !g.FindNode(added));
    c.Refresh();
    CHECK(c.SelectedNodes().empty());   // the host refused it: nothing is left selected

    // Esc mid-move drops the proposal.
    intents.clear();
    c.PointerDown({180, 110}, grapher::Button::Left, {});
    c.PointerMove({260, 160});
    c.CancelGesture();
    CHECK(!c.Busy() && !c.Proposing() && intents.empty());

    // Direct: Esc mid-move puts the node back and ends the snapshot it began; a wire let go on nothing is handed over.
    c.editMode = grapher::EditMode::Direct;
    c.PointerDown({180, 110}, grapher::Button::Left, {});
    c.PointerMove({260, 160});
    CHECK(g.nodes[0].frame.x != 100.f);
    c.CancelGesture();
    CHECK(g.nodes[0].frame.x == 100.f && snapshots == 1 && changes == 1);
    const auto pin = c.PortScreenPos("v1", "out");
    c.PointerDown(*pin, grapher::Button::Left, {});
    c.PointerMove(*pin + grapher::Vec2{100, 200});
    c.PointerUp(*pin + grapher::Vec2{200, 400}, grapher::Button::Left);
    const auto* loose = intents.size() == 1 ? std::get_if<grapher::ConnectToNothing>(&intents[0].change) : nullptr;
    CHECK(loose && loose->node == "v1" && loose->port == "out" && Near(loose->at, *pin + grapher::Vec2{200, 400}));
}

void TestNavigation()
{
    const grapher::Schema s = PortSchema();
    grapher::Graph g;
    g.nodes.push_back({"v1", "value", {0, 0, 160, 80}});
    g.nodes.push_back({"m", "mix", {400, 0, 200, 120}});
    grapher::Canvas c(g, s);
    c.SetViewport({1000, 600});

    c.PanBy({10, 20});
    CHECK(c.camera.pan == (grapher::Vec2{10, 20}));
    CHECK(c.ZoomAbout(2.f, {10, 20}) && c.camera.zoom == 2.f && c.camera.pan == (grapher::Vec2{10, 20}));   // the anchor stays put
    CHECK(c.ZoomAbout(100.f, {0, 0}) && c.camera.zoom == c.maxZoom && !c.ZoomAbout(2.f, {0, 0}));

    c.Frame({"m"}, 40.f, 1.f);
    CHECK(Near(c.ScreenRectOf("m").Center(), {500, 300}) && c.camera.zoom == 1.f);
    c.CenterOn("v1", 0.5f);
    CHECK(c.camera.zoom == 0.5f && Near(c.ScreenRectOf("v1").Center(), {500, 300}));
    c.camera = {{-3000, -3000}, 1.f};
    c.EnsureVisible("v1", 60.f);
    const grapher::Rect r = c.ScreenRectOf("v1");
    CHECK(r.x >= 59.9f && r.y >= 59.9f && r.Right() <= 940.1f && r.Bottom() <= 540.1f);
    CHECK(Near(*c.HeaderScreenPos("v1"), {r.Center().x, r.y + 8.f + 15.f * 0.65f}));

    // The host navigates: the right button no longer pans.
    c.panWithButtons = false;
    const grapher::Camera at = c.camera;
    c.PointerDown({900, 500}, grapher::Button::Right, {});
    c.PointerMove({950, 550});
    c.PointerUp({950, 550}, grapher::Button::Right);
    CHECK(c.camera.pan == at.pan);

    // Shift adds, Ctrl takes away.
    c.selectKeys = grapher::SelectKeys::ExtendSubtract;
    c.Frame();
    const grapher::Vec2 v = c.ScreenRectOf("v1").Center(), mm = c.ScreenRectOf("m").Center();
    c.PointerDown(v, grapher::Button::Left, {});
    c.PointerUp(v, grapher::Button::Left);
    c.PointerDown(mm, grapher::Button::Left, {true, false, false});
    c.PointerUp(mm, grapher::Button::Left);
    CHECK((c.SelectedNodes() == grapher::NodeSet{"m", "v1"}));
    c.PointerDown(mm, grapher::Button::Left, {true, false, false});   // Shift again keeps it
    c.PointerUp(mm, grapher::Button::Left);
    c.PointerDown(v, grapher::Button::Left, {false, true, false});
    c.PointerUp(v, grapher::Button::Left);
    CHECK((c.SelectedNodes() == grapher::NodeSet{"m"}));
    const grapher::Rect mr = c.ScreenRectOf("m");
    c.PointerDown({mr.x - 20, mr.y - 20}, grapher::Button::Left, {false, true, false});
    c.PointerMove({mr.Right() + 10, mr.Bottom() + 10});
    c.PointerUp({mr.Right() + 20, mr.Bottom() + 20}, grapher::Button::Left);
    CHECK(c.SelectedNodes().empty());
}

/// @brief A router that only counts its calls, to tell a reroute from a cached frame.
struct CountingRouter : grapher::WireRouter {
    int calls = 0;
    grapher::WirePaths Route(const grapher::Graph&, const grapher::Schema&, const grapher::RoutingOptions&) override
    {
        ++calls;
        return {};
    }
};

void TestLargeGraph()
{
    // 20,000 cards in 100 rows, each wired to its neighbour, one group around each row, and 300 other types in the
    // schema. The budgets hold in an optimised build; a debug build prints its times.
    grapher::Schema s;
    grapher::NodeType card{"card", "Card"};
    card.width = 160.f;
    card.height = 80.f;
    card.ports = {{"in", "In"}, {"out", "Out", "", grapher::PortDir::Out}};
    s.nodeTypes.push_back(card);
    grapher::NodeType folder{"folder", "Folder"};
    folder.shape = grapher::NodeShape::Group;
    s.nodeTypes.push_back(folder);
    for (int k = 0; k < 300; ++k) s.nodeTypes.push_back({"type" + std::to_string(k), "Type"});
    s.edgeTypes.push_back({"data", "data"});
    grapher::Graph g;
    g.nodes.reserve(20100);
    constexpr int cols = 200, rows = 100;
    for (int y = 0; y < rows; ++y) {
        g.nodes.push_back({"g" + std::to_string(y), "folder", {-20.f, y * 140.f - 30.f, cols * 240.f, 120.f}});
        for (int x = 0; x < cols; ++x) {
            g.nodes.push_back({"n" + std::to_string(y * cols + x), "card", {x * 240.f, y * 140.f, 160.f, 80.f}});
            if (x > 0)
                g.edges.push_back({"e" + std::to_string(y * cols + x), "n" + std::to_string(y * cols + x - 1),
                                   "n" + std::to_string(y * cols + x), "data", "", "out", "in"});
        }
    }
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); };

    grapher::Canvas c(g, s);
    c.SetViewport({1600, 900});
    auto t0 = Clock::now();
    c.CenterOn("n10100", 0.35f);   // the first route runs here
    const auto first = c.Draw();
    const double routeMs = ms(t0);
    int visible = 0;
    for (const auto& cmd : first.cmds)
        if (const auto* t = std::get_if<grapher::Text>(&cmd); t && t->text == "Card") ++visible;
    t0 = Clock::now();
    for (int k = 0; k < 10; ++k) (void)c.Draw();
    const double drawMs = ms(t0) / 10.0;
    t0 = Clock::now();
    int hits = 0;
    for (int k = 0; k < 200; ++k) hits += c.HitTest({static_cast<float>(k * 8 % 1600), static_cast<float>(k * 37 % 900)}).kind != grapher::HitKind::None;
    const double hitMs = ms(t0) / 200.0;
    const grapher::Vec2 grab = c.ScreenRectOf("n10100").Center();
    c.PointerDown(grab, grapher::Button::Left, {});
    c.PointerMove(grab + grapher::Vec2{10, 0});
    (void)c.Draw();
    t0 = Clock::now();
    c.PointerMove(grab + grapher::Vec2{30, 15});
    (void)c.Draw();
    const double dragMs = ms(t0);
    c.PointerUp(grab + grapher::Vec2{30, 15}, grapher::Button::Left);
    std::printf("  20k graph: %d cards in view; first route %.1f ms, draw %.2f ms, hit test %.3f ms, drag step %.2f ms\n", visible,
                routeMs, drawMs, hitMs, dragMs);
    CHECK(visible > 200 && visible < 600 && hits > 0);
#ifdef NDEBUG
    CHECK(drawMs <= 4.0);
    CHECK(hitMs <= 0.2);
    CHECK(dragMs <= 8.0);
#endif

    // The same graph with docks: in each run of four cards, the last three dock right of the one before (a wire runs
    // between). Dragging a docked card undocks it while it moves, the card docked to it following.
    grapher::Graph dg = g;
    for (grapher::Node& n : dg.nodes)
        if (n.id.starts_with('n'))
            if (const int k = std::stoi(n.id.substr(1)); k % 4 != 0) n.dock = {"n" + std::to_string(k - 1), grapher::PortSide::Right};
    grapher::Canvas dc(dg, s);
    dc.SetViewport({1600, 900});
    dc.docking = true;
    t0 = Clock::now();
    dc.CenterOn("n10101", 0.35f);
    (void)dc.Draw();
    const double dockRouteMs = ms(t0);
    t0 = Clock::now();
    for (int k = 0; k < 10; ++k) (void)dc.Draw();
    const double dockDrawMs = ms(t0) / 10.0;
    const grapher::Vec2 docked = dc.ScreenRectOf("n10102").Center();
    const float         follower = dc.FrameOf("n10103").x - dc.FrameOf("n10102").x;
    dc.PointerDown(docked, grapher::Button::Left, {});
    dc.PointerMove(docked + grapher::Vec2{10, 0});
    (void)dc.Draw();
    t0 = Clock::now();
    dc.PointerMove(docked + grapher::Vec2{30, 60});
    (void)dc.Draw();
    const double dockDragMs = ms(t0);
    CHECK(std::abs(dc.FrameOf("n10103").x - dc.FrameOf("n10102").x - follower) < 0.01f);   // it follows the dragged card
    dc.PointerUp(docked + grapher::Vec2{30, 60}, grapher::Button::Left);
    std::printf("  20k graph, docked: first route %.1f ms, draw %.2f ms, drag step %.2f ms\n", dockRouteMs, dockDrawMs, dockDragMs);
#ifdef NDEBUG
    CHECK(dockDrawMs <= 4.0);
    CHECK(dockDragMs <= 8.0);
#endif

    // Panning and zooming never reroute.
    grapher::Canvas counted(g, s);
    auto router    = std::make_shared<CountingRouter>();
    counted.router = router;
    counted.SetViewport({1600, 900});
    (void)counted.Draw();
    counted.PanBy({300, 200});
    (void)counted.Draw();
    counted.ZoomAbout(1.5f, {800, 450});
    (void)counted.Draw();
    (void)counted.HitTest({800, 450});
    CHECK(router->calls == 1);
}

// ── Member groups and layout ────────────────────────────────────────────────

void TestLayoutColumns()
{
    using grapher::Vec2;
    const std::vector<Vec2> items(4, Vec2{100, 40});
    // Columns follow the wires, and a cycle still gets a place.
    const auto placed = grapher::LayoutColumns(items, {{0, 1}, {0, 2}, {1, 3}, {3, 1}, {2, 2}});
    CHECK(placed.size() == 4 && placed[0].x == 0 && placed[1].x > placed[0].x && placed[3].x > placed[1].x);
    const auto loose = grapher::LayoutColumns(items, {});
    for (const Vec2& p : loose) CHECK(p.x == 0);
    CHECK(loose[1].y >= loose[0].y + 40);
    const auto ring = grapher::LayoutColumns(items, {{0, 1}, {1, 2}, {2, 3}, {3, 0}});
    CHECK(ring[1].x > ring[0].x);
    CHECK(grapher::LayoutColumns({}, {}).empty());
    // Crossed wires untangle, and a parent sits level with the middle of its children.
    const auto crossed = grapher::LayoutColumns(items, {{0, 3}, {1, 2}});
    CHECK(crossed[0].y < crossed[1].y && crossed[3].y < crossed[2].y);
    const auto fan = grapher::LayoutColumns(items, {{0, 1}, {0, 2}, {0, 3}});
    CHECK(std::abs(fan[0].y - fan[2].y) < 0.01f && fan[1].y + 40 <= fan[2].y && fan[2].y + 40 <= fan[3].y);
    // A large box lays out: a tree, and a tangle where every sweep runs.
    std::vector<Vec2>                big(20000, Vec2{160, 60});
    std::vector<std::pair<int, int>> tree, tangle;
    for (int i = 1; i < 20000; ++i) tree.emplace_back((i - 1) / 4, i);
    for (int i = 0; i < 20000; ++i) tangle.emplace_back(i, (i * 7919 + 13) % 20000);
    CHECK(grapher::LayoutColumns(big, tree).size() == 20000);
    CHECK(grapher::LayoutColumns(big, tangle).size() == 20000);
}

grapher::Schema FolderSchema()
{
    grapher::Schema s;
    grapher::NodeType box{"box", "Box"};
    box.width  = 100.f;
    box.height = 60.f;
    s.nodeTypes.push_back(box);
    grapher::NodeType folder{"folder", "Folder"};
    folder.shape     = grapher::NodeShape::Group;
    folder.groupMode = grapher::GroupMode::Members;
    folder.showCount = true;
    s.nodeTypes.push_back(folder);
    grapher::EdgeType curve{"curve", "curve"};
    curve.color = {1, 2, 3};
    s.edgeTypes.push_back(curve);
    return s;
}

grapher::Graph FolderGraph()
{
    // F1 holds a and F2; F2 holds b; c stands alone. c wires to a and b, a to b.
    grapher::Graph g;
    g.nodes.push_back({"F1", "folder", {0, 0, 10, 10}});
    g.nodes.push_back({"F2", "folder", {0, 0, 10, 10}});
    g.nodes.push_back({"a", "box", {0, 0, 100, 60}});
    g.nodes.push_back({"b", "box", {200, 0, 100, 60}});
    g.nodes.push_back({"c", "box", {600, 0, 100, 60}});
    g.nodes[1].parent = "F1";
    g.nodes[2].parent = "F1";
    g.nodes[3].parent = "F2";
    g.edges.push_back({"ab", "a", "b", "curve"});
    g.edges.push_back({"ca", "c", "a", "curve"});
    g.edges.push_back({"cb", "c", "b", "curve"});
    return g;
}

void TestMemberGroups()
{
    const grapher::Schema s = FolderSchema();
    grapher::Graph        g = FolderGraph();
    grapher::Canvas       c(g, s);
    c.SetViewport({1000, 600});
    c.camera.pan = {100, 100};
    std::vector<std::pair<std::string, bool>> toggled;
    c.onCollapseChanged = [&](std::string_view id, bool on) { toggled.emplace_back(std::string(id), on); };
    const float pad = c.style.groupPadding, head = c.style.groupHeader;

    CHECK(grapher::IsMemberGroup(g.nodes[0], s) && grapher::ParentGroup(g, s, g.nodes[3]) == "F2");
    CHECK((grapher::GroupMembers(g, s, "F1") == std::vector<std::string>{"F2", "a", "b"}));

    // Each group wraps what it holds, below its header; the outer one wraps the inner.
    const grapher::Rect f2 = c.FrameOf("F2"), f1 = c.FrameOf("F1");
    CHECK(f2 == (grapher::Rect{200 - pad, -head, 100 + 2 * pad, 60 + head + pad}));
    CHECK(f1.x == -pad && f1.y == f2.y - head && f1.Right() == f2.Right() + pad);
    CHECK(CountText(c.Draw(), "\xe2\x96\xbe Untitled Folder (2)") == 1);

    // Closed: what it holds goes, and the wires into it end on it, merged into one dashed wire.
    c.SetCollapsed("F1", true);
    CHECK((toggled.size() == 1 && toggled[0] == std::pair<std::string, bool>{"F1", true}));
    CHECK(c.IsHidden("a") && c.IsHidden("F2") && !c.IsHidden("F1") && c.FrameOf("F1").h == head && c.FrameOf("a") == grapher::Rect{});
    int wires = 0, dashed = 0;
    for (const auto& cmd : c.Draw().cmds)
        if (const auto* p = std::get_if<grapher::Polyline>(&cmd); p && p->color == (grapher::Color{1, 2, 3})) ++wires, dashed += p->dashed;
    CHECK(wires == 1 && dashed == 1);
    CHECK(c.HitTest(c.ScreenRectOf("F1").Center()).id == "F1");

    // A press on its arrow opens it; Reveal opens every group around a node; all can close at once.
    const grapher::Rect r = c.ScreenRectOf("F1");
    c.PointerDown({r.x + 5, r.y + 5}, grapher::Button::Left, {});
    c.PointerUp({r.x + 5, r.y + 5}, grapher::Button::Left);
    CHECK(!c.IsCollapsed("F1") && !c.IsHidden("a"));
    c.SetAllCollapsed(true);
    CHECK(c.IsCollapsed("F1") && c.IsCollapsed("F2"));
    c.Reveal("b");
    CHECK(c.CollapsedGroups().empty() && !c.IsHidden("b"));

    // Dragging a group carries what it holds; a node dropped on a group joins it, and dropped on none, leaves.
    const grapher::Rect head1 = c.ScreenRectOf("F1");
    const grapher::Vec2 grip{head1.x + head + 20, head1.y + 5};
    c.PointerDown(grip, grapher::Button::Left, {});
    c.PointerMove(grip + grapher::Vec2{20, 0});
    c.PointerUp(grip + grapher::Vec2{40, 0}, grapher::Button::Left);
    CHECK(g.nodes[2].frame.x == 40.f && g.nodes[3].frame.x == 240.f && g.nodes[4].frame.x == 600.f);
    const grapher::Vec2 cAt = c.ScreenRectOf("c").Center(), into = c.ScreenRectOf("F2").Center();
    c.PointerDown(cAt, grapher::Button::Left, {});
    c.PointerMove(cAt + (into - cAt) * 0.5f);
    c.PointerUp(into, grapher::Button::Left);
    CHECK(g.nodes[4].parent == "F2");
    const grapher::Vec2 aAt = c.ScreenRectOf("a").Center();
    c.PointerDown(aAt, grapher::Button::Left, {});
    c.PointerMove(aAt + grapher::Vec2{0, 200});
    c.PointerUp(aAt + grapher::Vec2{0, 400}, grapher::Button::Left);
    CHECK(g.nodes[2].parent.empty());

    // Proposed, the new parent arrives with the frames.
    c.editMode = grapher::EditMode::Propose;
    std::vector<grapher::Intent> intents;
    c.onIntent = [&](const grapher::Intent& i) { intents.push_back(i); };
    const grapher::Vec2 aNow = c.ScreenRectOf("a").Center(), back = c.ScreenRectOf("F1").Center();
    c.PointerDown(aNow, grapher::Button::Left, {});
    c.PointerMove(aNow + (back - aNow) * 0.5f);
    c.PointerUp(back, grapher::Button::Left);
    const auto* move = intents.size() == 1 ? std::get_if<grapher::MoveNodes>(&intents[0].change) : nullptr;
    CHECK(move && move->parents.size() == 1 && move->parents[0].first == "a" && !move->parents[0].second.empty());
    CHECK(g.nodes[2].parent.empty());
    c.Refresh();
}

void TestAliasesAndHolding()
{
    const grapher::Schema s = FolderSchema();
    grapher::Graph        g = FolderGraph();
    g.nodes.push_back({"b2", "box", {600, 300, 100, 60}});
    g.nodes.back().aliasOf = "b";
    grapher::Canvas c(g, s);
    c.SetViewport({1000, 600});
    c.SetCollapsed("F1", true);

    // An alias draws dimmed with a dashed outline; a click on it selects the original, opening the groups around it.
    bool outline = false;
    for (const auto& cmd : c.Draw().cmds)
        if (const auto* f = std::get_if<grapher::FillRect>(&cmd); f && f->dashed && f->rect == c.ScreenRectOf("b2")) outline = true;
    CHECK(outline);
    const grapher::Vec2 at = c.ScreenRectOf("b2").Center();
    c.PointerDown(at, grapher::Button::Left, {});
    c.PointerUp(at, grapher::Button::Left);
    CHECK(c.SelectedNodes() == grapher::NodeSet{"b"} && !c.IsCollapsed("F1") && !c.IsHidden("b"));
    const grapher::Rect shown = c.ScreenRectOf("b");
    CHECK(shown.x >= 0 && shown.Right() <= 1000 && shown.y >= 0 && shown.Bottom() <= 600);

    // Held in place: the host lays the graph out again, and the node stays where it was on screen.
    const auto before = c.HeaderScreenPos("c");
    c.HoldInPlace("c");
    g.nodes[4].frame.x += 300;
    g.nodes[4].frame.y -= 120;
    c.Refresh();
    CHECK(before && Near(*c.HeaderScreenPos("c"), *before));
}

void TestLayoutGraph()
{
    const grapher::Schema s = FolderSchema();
    grapher::Graph        g = FolderGraph();
    grapher::LayoutOptions o;
    auto frameOf = [](const std::vector<std::pair<std::string, grapher::Rect>>& f, std::string_view id) {
        for (const auto& [k, r] : f)
            if (k == id) return r;
        return grapher::Rect{};
    };

    // Inside out: each group wraps its laid-out content; nothing overlaps at the top level; wires run left to right.
    auto laid = grapher::LayoutGraph(g, s, {}, {}, o);
    CHECK(laid.size() == 5);
    const grapher::Rect F1 = frameOf(laid, "F1"), F2 = frameOf(laid, "F2"), a = frameOf(laid, "a"), b = frameOf(laid, "b"), cc = frameOf(laid, "c");
    CHECK(F1.Holds(a) && F1.Holds(F2) && F2.Holds(b) && !F1.Intersects(cc));
    CHECK(a.x < F2.x && cc.x < F1.x);   // c wires into F1's content, a into F2's
    CHECK(std::abs(F2.y + o.header - (b.y - 0.f)) <= o.header + 0.01f && b.x == F2.x + o.padding);

    // A closed group is one header-high item, as wide as its title needs.
    laid = grapher::LayoutGraph(g, s, {}, {"F2"}, o);
    CHECK((frameOf(laid, "F2").h == o.header && frameOf(laid, "F2").w == o.closedWidth));   // its title, not what it holds

    // A kept node stays put; one placed by the layout slides clear of it; an open group pushes what it would cover.
    g.nodes[4].frame = {0, 0, 100, 60};
    laid = grapher::LayoutGraph(g, s, {"c"}, {}, o);
    const grapher::Rect kept = frameOf(laid, "c"), F1b = frameOf(laid, "F1");
    CHECK(kept == (grapher::Rect{0, 0, 100, 60}) || !kept.Intersects(F1b));
    CHECK(!kept.Inflated(-1.f).Intersects(F1b));
}

void TestLayoutDocks()
{
    // A parent and what is docked to it lay out as one item: the docked nodes by the dock rules, the columns clear of the
    // whole cluster.
    const grapher::Schema s = FolderSchema();
    grapher::Graph        g;
    grapher::Node         p{"p", "box", {0, 0, 100, 60}};
    p.rows.push_back({"x", "line"});
    g.nodes.push_back(p);
    g.nodes.push_back({"k1", "box", {0, 0, 100, 60}});
    g.nodes.push_back({"k2", "box", {0, 0, 100, 40}});
    g.nodes.push_back({"r", "box", {0, 0, 80, 20}});
    g.nodes.push_back({"q", "box", {0, 0, 100, 60}});
    g.nodes[1].dock = {"p", grapher::PortSide::Bottom, 0};
    g.nodes[2].dock = {"p", grapher::PortSide::Bottom, 1};
    g.nodes[3].dock = {"p", grapher::PortSide::Right, 0, "x"};
    g.edges.push_back({"e1", "p", "k1", "curve"});
    g.edges.push_back({"e2", "p", "q", "curve"});
    g.edges.push_back({"e3", "k2", "q", "curve"});
    grapher::LayoutOptions o;
    auto frameOf = [](const std::vector<std::pair<std::string, grapher::Rect>>& f, std::string_view id) {
        for (const auto& [k, r] : f)
            if (k == id) return r;
        return grapher::Rect{};
    };
    auto laid = grapher::LayoutGraph(g, s, {}, {}, o);
    CHECK(laid.size() == 5);
    const grapher::Rect P = frameOf(laid, "p"), K1 = frameOf(laid, "k1"), K2 = frameOf(laid, "k2"), R = frameOf(laid, "r"),
                        Q = frameOf(laid, "q");
    CHECK((K1.x == P.x && K1.y == P.Bottom() + o.dockGap && K2.x == P.x && K2.y == K1.Bottom() + o.dockGap));
    const grapher::Rect line = grapher::RowRect(g.nodes[0], s, o.metrics, 0, P);
    CHECK((R.x == P.Right() + o.dockSideGap && std::abs(R.Center().y - line.Center().y) < 0.01f));
    // q's column starts past the whole cluster, and overlaps none of it.
    CHECK(Q.x >= R.Right() + o.gaps.column - 0.01f);
    for (const grapher::Rect& r : {P, K1, K2, R}) CHECK(!Q.Intersects(r));

    // A kept parent stays put and its cluster comes with it; a dock across groups is left out.
    g.nodes[0].frame = {500, 400, 100, 60};
    laid             = grapher::LayoutGraph(g, s, {"p"}, {}, o);
    CHECK(frameOf(laid, "p") == g.nodes[0].frame);
    CHECK((frameOf(laid, "k1").x == 500.f && frameOf(laid, "k1").y == 400.f + 60.f + o.dockGap));
    grapher::Graph h = g;
    h.nodes.push_back({"F", "folder", {0, 0, 200, 200}});
    h.nodes[2].parent = "F";   // k2 lives in F: its dock to p is left out
    laid = grapher::LayoutGraph(h, s, {}, {}, o);
    CHECK((frameOf(laid, "F").Holds(frameOf(laid, "k2")) && frameOf(laid, "k1").y == frameOf(laid, "p").Bottom() + o.dockGap));

    // A kept group with a kept member keeps its corner: the member stays put though it lies left of the corner, and the
    // group's frame reaches out to wrap it; a member the layout places sits clear of it.
    grapher::Graph k;
    k.nodes.push_back({"G", "folder", {300, 300, 10, 10}});
    k.nodes.push_back({"m", "box", {300 + o.padding - 50, 300 + o.header + 20, 100, 60}});
    k.nodes.push_back({"n", "box", {0, 0, 100, 60}});
    k.nodes[1].parent = k.nodes[2].parent = "G";
    laid = grapher::LayoutGraph(k, s, {"G", "m"}, {}, o);
    const grapher::Rect G = frameOf(laid, "G"), M = frameOf(laid, "m"), N = frameOf(laid, "n");
    CHECK(M == k.nodes[1].frame);
    CHECK((G.Holds(M) && G.Holds(N) && G.x == M.x - o.padding && !N.Intersects(M)));

    // A kept item takes no room in the columns: a wide one wired in the middle doesn't push the next column away, and
    // what the layout places slides down clear of it.
    grapher::Graph w;
    w.nodes.push_back({"a", "box", {0, 0, 100, 60}});
    w.nodes.push_back({"W", "box", {200, 0, 2000, 80}});
    w.nodes.push_back({"b", "box", {0, 0, 100, 60}});
    w.edges.push_back({"e1", "a", "W", "curve"});
    w.edges.push_back({"e2", "W", "b", "curve"});
    w.edges.push_back({"e3", "a", "b", "curve"});
    laid = grapher::LayoutGraph(w, s, {"W"}, {}, o);
    const grapher::Rect A = frameOf(laid, "a"), W = frameOf(laid, "W"), B = frameOf(laid, "b");
    CHECK(W == w.nodes[1].frame);
    CHECK((B.x == A.Right() + o.gaps.column && !B.Intersects(W) && !A.Intersects(W)));
}

void TestFitBelowMinZoom()
{
    // A fit wins over the zoom-out limit: a graph too wide for it still fits whole. Zooming out then stops at that fit, and
    // a fit that needs less leaves the limit as it was.
    const grapher::Schema s = FolderSchema();
    grapher::Graph        g;
    g.nodes.push_back({"a", "box", {0, 0, 100, 60}});
    g.nodes.push_back({"b", "box", {50000, 0, 100, 60}});
    grapher::Canvas c(g, s);
    c.SetViewport({1000, 600});
    c.minZoom = 0.1f;
    c.Frame();
    const float fit = c.camera.zoom;
    CHECK(fit < c.minZoom);
    for (const char* id : {"a", "b"}) {
        const grapher::Rect r = c.ScreenRectOf(id);
        CHECK((r.x >= 0.f && r.Right() <= 1000.f && r.y >= 0.f && r.Bottom() <= 600.f));
    }
    CHECK(c.ZoomFloor() == fit);
    CHECK(!c.Wheel({500, 300}, -3.f));   // already as far out as it goes
    CHECK(c.Wheel({500, 300}, 5.f));
    CHECK(c.camera.zoom > fit);
    for (int k = 0; k < 40; ++k) c.Wheel({500, 300}, -1.f);
    CHECK(std::abs(c.camera.zoom - fit) < 1e-6f);   // back to the fit, and no further
    c.Frame({"a"});
    CHECK((c.camera.zoom >= c.minZoom && c.ZoomFloor() == c.minZoom));
}

void TestFixedGroupAndHover()
{
    // A fixed member group is the canvas: a press on its header neither picks, moves nor closes it, and "close all" passes
    // it by; what it holds is picked as ever. The host hears what the pointer is over.
    grapher::Schema s = FolderSchema();
    grapher::NodeType frame{"frame", "Frame"};
    frame.shape     = grapher::NodeShape::Group;
    frame.groupMode = grapher::GroupMode::Members;
    frame.fixed     = true;
    s.nodeTypes.push_back(frame);
    grapher::Graph g;
    g.nodes.push_back({"U", "frame", {0, 0, 400, 300}});
    g.nodes.push_back({"a", "box", {50, 60, 100, 60}});
    g.nodes.push_back({"F", "folder", {200, 60, 150, 150}});
    g.nodes.push_back({"b", "box", {220, 100, 100, 60}});
    g.nodes[1].parent = g.nodes[2].parent = "U";
    g.nodes[3].parent = "F";
    grapher::Canvas c(g, s);
    c.SetViewport({1000, 600});
    std::vector<std::string> hovers;
    c.onHoverChanged = [&](std::string_view id) { hovers.emplace_back(id); };

    const grapher::Rect U = c.ScreenRectOf("U");
    const grapher::Vec2 header{U.x + 8, U.y + 6};
    CHECK(c.HitTest(header).kind == grapher::HitKind::None);
    c.PointerDown(header, grapher::Button::Left, {});
    c.PointerUp(header, grapher::Button::Left);
    CHECK((!c.IsCollapsed("U") && c.SelectedNodes().empty() && g.nodes[0].frame == (grapher::Rect{0, 0, 400, 300})));
    c.SetAllCollapsed(true);
    CHECK((!c.IsCollapsed("U") && c.IsCollapsed("F")));
    c.SetAllCollapsed(false);

    const grapher::Vec2 onA = c.ScreenRectOf("a").Center();
    CHECK(c.HitTest(onA).id == "a");
    c.PointerMove(onA);
    c.PointerMove(onA + grapher::Vec2{1, 0});   // still over it: no news
    c.PointerMove({U.Right() + 300, U.Bottom() + 200});
    CHECK((hovers == std::vector<std::string>{"a", ""}));
}

void TestFrameGroupsUnchanged()
{
    // Architect Designer's folders are frame groups: geometric membership, no arrow, no closing, no parents set, and the
    // graph drawn as it is.
    const grapher::Schema s = GroupSchema();
    grapher::Graph        g;
    g.nodes.push_back({"G", "folder", {0, 0, 400, 300}});
    g.nodes.push_back({"m", "box", {50, 60, 100, 60}});
    g.nodes.push_back({"x", "box", {600, 60, 100, 60}});
    grapher::Canvas c(g, s);
    c.SetViewport({1000, 600});
    int toggles = 0;
    c.onCollapseChanged = [&](std::string_view, bool) { ++toggles; };
    CHECK(!grapher::IsMemberGroup(g.nodes[0], s) && grapher::ParentGroup(g, s, g.nodes[1]) == "G");
    CHECK(c.FrameOf("G") == g.nodes[0].frame);

    const grapher::Rect r = c.ScreenRectOf("G");
    c.PointerDown({r.x + 5, r.y + 5}, grapher::Button::Left, {});
    c.PointerUp({r.x + 5, r.y + 5}, grapher::Button::Left);
    c.SetAllCollapsed(true);
    CHECK(toggles == 0 && c.CollapsedGroups().empty() && !c.IsHidden("m"));
    bool arrow = false;
    for (const auto& cmd : c.Draw().cmds)
        if (const auto* t = std::get_if<grapher::Text>(&cmd); t && t->text.starts_with("\xe2\x96")) arrow = true;
    CHECK(!arrow);

    // Dropped inside, a node slides in by geometry and no parent is set.
    const grapher::Vec2 at = c.ScreenRectOf("x").Center(), into = r.Center();
    c.PointerDown(at, grapher::Button::Left, {});
    c.PointerMove(at + (into - at) * 0.5f);
    c.PointerUp(into, grapher::Button::Left);
    CHECK(g.nodes[2].parent.empty() && grapher::ParentGroup(g, s, g.nodes[2]) == "G" && g.nodes[0].frame.Holds(g.nodes[2].frame));
}

// ── Docks, dials, wire styles ────────────────────────────────────────────────

grapher::Schema DockSchema()
{
    grapher::Schema s = FolderSchema();   // box 100x60, a member folder, a "curve" edge
    grapher::NodeType card{"card", "Card"};
    card.width  = 200.f;
    card.height = 120.f;
    s.nodeTypes.push_back(card);
    return s;
}

void TestDocks()
{
    const grapher::Schema s = DockSchema();
    grapher::Graph        g;
    g.nodes.push_back({"P", "card", {0, 0, 200, 120}});
    g.nodes.push_back({"A", "box", {900, 900, 100, 60}});
    g.nodes.push_back({"B", "box", {900, 900, 100, 60}});
    g.nodes.push_back({"C", "box", {900, 900, 100, 60}});
    g.nodes[1].dock = {"P", grapher::PortSide::Bottom, 0};
    g.nodes[2].dock = {"P", grapher::PortSide::Bottom, 1};
    g.nodes[3].dock = {"P", grapher::PortSide::Right, 0};
    g.edges.push_back({"pa", "P", "A", "curve"});
    g.edges.push_back({"pb", "P", "B", "curve"});
    grapher::Canvas c(g, s);
    c.SetViewport({1600, 1200});
    c.camera.pan = {100, 100};
    c.docking    = true;
    const float gap = c.style.dockGap, side = c.style.dockSideGap;

    // Docked: stacked under their parent nearest first, or beside it; their place travels with the parent.
    CHECK(c.FrameOf("A") == (grapher::Rect{0, 120 + gap, 100, 60}));
    CHECK(c.FrameOf("B") == (grapher::Rect{0, 120 + gap + 60 + gap, 100, 60}));
    CHECK(c.FrameOf("C") == (grapher::Rect{200 + side, 0, 100, 60}));
    g.nodes[0].frame.x = 50;
    CHECK(c.FrameOf("B").x == 50.f && c.FrameOf("C").x == 250.f + side);

    // Dragged away, a docked node undocks where it is let go; the one behind it closes up.
    const grapher::Vec2 a = c.ScreenRectOf("A").Center();
    c.PointerDown(a, grapher::Button::Left, {});
    c.PointerMove(a + grapher::Vec2{300, 100});
    CHECK(c.FrameOf("B").y == 120 + gap);   // closed up while A is away
    c.PointerUp(a + grapher::Vec2{600, 200}, grapher::Button::Left);
    CHECK(g.nodes[1].dock.parent.empty() && g.nodes[1].frame.x == 650.f);

    // Dragged onto the slot under its parent (a wire runs to it), it docks there, and B moves out behind it.
    const auto slot = c.FindDockSlot("A", grapher::Rect{55, 120 + gap + 5, 100, 60});
    CHECK(slot && slot->parent == "P" && slot->side == grapher::PortSide::Bottom && slot->index == 0);
    const grapher::Vec2 from = c.ScreenRectOf("A").Center(), to = c.camera.ToScreen(grapher::Vec2{105, 120 + gap + 35});
    c.PointerDown(from, grapher::Button::Left, {});
    c.PointerMove(from + (to - from) * 0.5f);
    c.PointerMove(to);
    bool phantom = false;
    for (const auto& cmd : c.Draw().cmds)
        if (const auto* f = std::get_if<grapher::FillRect>(&cmd); f && f->dashed && f->stroke == c.style.selection) phantom = true;
    CHECK(phantom);
    c.PointerUp(to, grapher::Button::Left);
    CHECK(g.nodes[1].dock.parent == "P" && g.nodes[1].dock.order == 0 && g.nodes[2].dock.order == 1);
    CHECK(!c.FindDockSlot("C", grapher::Rect{55, 400, 100, 60}));   // nothing wires to C: no slot takes it

    // A card beside a row sits level with that line; a member group docked flush hangs right under its card.
    g.nodes[0].rows = {{"r0", "zero"}, {"r1", "one"}};
    g.nodes.push_back({"E", "box", {0, 0, 100, 30}});
    g.nodes.back().dock = {"P", grapher::PortSide::Right, 0, "r1"};
    g.nodes.push_back({"G", "folder", {0, 0, 10, 10}});
    g.nodes.back().dock = {"P", grapher::PortSide::Bottom, 2, {}, true};
    g.nodes.push_back({"m", "box", {500, 500, 100, 60}});
    g.nodes.back().parent = "G";
    const grapher::Rect line = *c.RowScreenRect("P", "r1");
    const grapher::Rect e    = c.ScreenRectOf("E");
    CHECK(std::abs(e.Center().y - line.Center().y) < 0.01f && e.x > c.ScreenRectOf("P").Right());
    const grapher::Rect b = c.FrameOf("B"), grp = c.FrameOf("G");
    CHECK(std::abs(grp.y - b.Bottom()) < 0.01f && grp.Holds(c.FrameOf("m")));

    // A dock cycle ends.
    g.nodes.push_back({"X", "box", {0, 900, 100, 60}});
    g.nodes.push_back({"Y", "box", {0, 900, 100, 60}});
    g.nodes[g.nodes.size() - 2].dock = {"Y", grapher::PortSide::Bottom};
    g.nodes.back().dock              = {"X", grapher::PortSide::Bottom};
    (void)c.Draw();

    // Proposed, the new dock arrives with the frames.
    c.editMode = grapher::EditMode::Propose;
    std::vector<grapher::Intent> intents;
    c.onIntent = [&](const grapher::Intent& i) { intents.push_back(i); };
    const grapher::Vec2 bAt = c.ScreenRectOf("B").Center();
    c.PointerDown(bAt, grapher::Button::Left, {});
    c.PointerMove(bAt + grapher::Vec2{400, 300});
    c.PointerUp(bAt + grapher::Vec2{800, 600}, grapher::Button::Left);
    const auto* move = intents.size() == 1 ? std::get_if<grapher::MoveNodes>(&intents[0].change) : nullptr;
    CHECK(move && move->docks.size() == 1 && move->docks[0].first == "B" && move->docks[0].second.parent.empty());
    c.Refresh();
}

void TestShowWithAndWireStyles()
{
    grapher::Schema s = DockSchema();
    s.edgeTypes[0].dotted = true;
    s.edgeTypes[0].width  = 3.f;
    grapher::Graph g;
    g.nodes.push_back({"S", "box", {0, 0, 100, 60}});
    g.nodes.push_back({"W", "box", {300, 0, 100, 60}});
    g.nodes.push_back({"T", "box", {300, 200, 100, 60}});
    g.nodes[1].showWith = {"S"};
    g.edges.push_back({"sw", "S", "W", "curve"});
    g.edges.push_back({"st", "S", "T", "curve"});
    grapher::Canvas c(g, s);
    c.SetViewport({800, 600});

    // Hidden until what it belongs to is selected: not drawn, not hit, its wires not drawn.
    auto wires = [&] {
        int n = 0;
        for (const auto& cmd : c.Draw().cmds)
            if (const auto* p = std::get_if<grapher::Polyline>(&cmd); p && p->color == (grapher::Color{1, 2, 3})) {
                ++n;
                CHECK(p->dotted && !p->dashed && p->width == 3.f);
            }
        return n;
    };
    CHECK(wires() == 1 && c.HitTest(c.ScreenRectOf("W").Center()).id != "W");
    c.SelectNodes({"S"});
    CHECK(wires() == 2 && c.HitTest(c.ScreenRectOf("W").Center()).id == "W");

    // A wire keeps its sides near the turn, with a memory; without one it turns.
    grapher::Graph h;
    h.nodes.push_back({"a", "box", {0, 0, 100, 60}});
    h.nodes.push_back({"b", "box", {200, 0, 100, 60}});
    h.edges.push_back({"ab", "a", "b", "curve"});
    std::unordered_map<std::string, std::uint8_t> sides;
    CHECK(grapher::RouteWires(h, s, {}, &sides).at("ab").front().x == 100.f);   // a's right side
    h.nodes[1].frame = {130, 80, 100, 60};   // just past the turn
    CHECK(grapher::RouteWires(h, s, {}).at("ab").front().y == 60.f);            // a's bottom
    CHECK(grapher::RouteWires(h, s, {}, &sides).at("ab").front().x == 100.f);   // held
    h.nodes[1].frame = {0, 300, 100, 60};   // well past it
    CHECK(grapher::RouteWires(h, s, {}, &sides).at("ab").front().y == 60.f);
}

void TestDial()
{
    using grapher::DialAxis;
    constexpr double kPi = 3.14159265358979323846;
    // A cyclic axis is at its true angle from the top, and wraps.
    const DialAxis cyc = DialAxis::Make(true, 0.f, 4.f, {});
    CHECK(std::abs(cyc.Angle(1.f) - kPi / 2) < 1e-6 && std::abs(cyc.Value(-kPi / 2) - 3.f) < 1e-4);
    // An open one spreads its values evenly over 270 degrees, linear between neighbours, clamped at its ends.
    const std::vector<float> speeds{5.3f, 52.8f, 351.f, 3510.f};
    const DialAxis           open = DialAxis::Make(false, 0.f, 0.f, speeds);
    CHECK(std::abs(open.Angle(5.3f) + 0.75 * kPi) < 1e-6 && std::abs(open.Angle(3510.f) - 0.75 * kPi) < 1e-6);
    CHECK(std::abs(open.Angle(52.8f) - (-0.25 * kPi)) < 1e-6 && std::abs(open.Value(open.Angle(200.f)) - 200.f) < 0.01f);
    CHECK(open.Value(kPi) == 5.3f || open.Value(kPi) == 3510.f);
    CHECK(std::abs(open.Snap(60.f) - open.Value((-0.75 + 1.5 * 6.0 / 16.0) * kPi)) < 0.01f || open.Snap(60.f) > 0.f);
    const DialAxis one = DialAxis::Make(false, 0.f, 0.f, std::vector<float>{2.f});
    CHECK(one.knots.size() == 2 && one.Angle(2.f) < one.Angle(3.f));

    // A dial node: its rows' pins on the ring at their values; a handle drags a row's value round, Shift snapping.
    grapher::Schema s = DockSchema();
    grapher::NodeType dial{"dial", "Blend"};
    dial.shape = grapher::NodeShape::Dial;
    s.nodeTypes.push_back(dial);
    grapher::Graph g;
    g.nodes.push_back({"d", "dial", {0, 0, 300, 300}});
    grapher::Node& d = g.nodes[0];
    d.ports = {{"p0", "", "", grapher::PortDir::Out}, {"p1", "", "", grapher::PortDir::Out}, {"p2", "", "", grapher::PortDir::Out}};
    d.rows  = {{"r0", "walk", "0"}, {"r1", "jog", "1"}, {"r2", "run", "2"}};
    for (int k = 0; k < 3; ++k) d.rows[static_cast<std::size_t>(k)].port = "p" + std::to_string(k), d.rows[static_cast<std::size_t>(k)].field = "pos" + std::to_string(k);
    d.dial = {false, 0.f, 1.f, 1.f, "speed"};
    grapher::Canvas c(g, s);
    c.SetViewport({800, 600});
    const grapher::Ring     ring = grapher::RingOf(d.frame);
    const grapher::DialAxis axis = grapher::AxisOf(d);
    CHECK(Near(*c.PortScreenPos("d", "p1"), ring.At(axis.Angle(1.f))) && Near(*c.PortScreenPos("d", "p0"), ring.At(-0.75 * kPi)));
    const grapher::Vec2 handle = ring.At(axis.Angle(1.f), 0.8f);
    const grapher::Hit  hit    = c.HitTest(handle);
    CHECK(hit.kind == grapher::HitKind::Handle && hit.row == "r1");
    const grapher::Vec2 there = ring.At(0.3 * kPi, 0.8f);   // past "run"'s value: clamped to the arc's end
    c.PointerDown(handle, grapher::Button::Left, {});
    c.PointerMove(handle + (there - handle) * 0.5f);
    c.PointerMove(ring.At(0.1 * kPi, 0.8f), {true, false, false});
    c.PointerUp(ring.At(0.1 * kPi, 0.8f), grapher::Button::Left);
    const float v = std::stof(g.nodes[0].Field("pos1"));
    CHECK(v == axis.Snap(axis.Value(0.1 * kPi)) && g.nodes[0].rows[1].value == g.nodes[0].Field("pos1"));
    bool needle = false;
    for (const auto& cmd : c.Draw().cmds)
        if (const auto* t = std::get_if<grapher::Text>(&cmd); t && t->text.starts_with("speed")) needle = true;
    CHECK(needle);

    // The format: a dial shape, dotted and wide edges, chip rows.
    grapher::Schema styled = s;
    styled.edgeTypes[0].dotted = true;
    styled.edgeTypes[0].width  = 2.5f;
    styled.rowStyles.push_back({"chip", grapher::Color{1, 2, 3}});
    styled.rowStyles.back().chip = true;
    const auto back = grapher::ParseSchema(grapher::WriteSchema(styled));
    CHECK(back && *back == styled);
}

// K4b: an octagon dial. The rim is an octagon (a kit's rim part), a row stands where the ray at its value's angle meets it,
// its handle drags along the edges; the node is the octagon, a header band on its top edge, its other rows listed under
// it with their pins on its edges, and an open dial's gap is the flat bottom edge, where the needle's reading goes.
void TestOctagonDial()
{
    constexpr double kPi = 3.14159265358979323846;
    // The rim: rays meet the flat sides and the cut corners; outward is square to the edge met.
    const grapher::Ring oct{{0, 0}, 100.f, 0.4f};
    CHECK((Near(oct.At(0.0), {0, -100}) && Near(oct.At(kPi / 2), {100, 0}) && Near(oct.At(kPi / 4), {80, -80})));
    CHECK((Near(oct.At(kPi / 4, 0.5f), {40, -40}) && Near(oct.Normal(0.0), {0, -1}) && Near(oct.Normal(kPi / 2), {1, 0})));
    CHECK((std::abs(oct.Normal(kPi / 4).x - 0.7071f) < 1e-3f && std::abs(oct.Normal(kPi / 4).y + 0.7071f) < 1e-3f));
    CHECK((oct.Holds({0, 0}) && oct.Holds({79, -79}) && !oct.Holds({95, 95}) && !oct.Holds({0, 101})));
    // Open, it reaches round to the flat bottom edge's ends; its outline keeps the corners between.
    const double reach = oct.OpenReach();
    CHECK(std::abs(reach - (1.0 - std::atan2(60.0, 100.0) / kPi)) < 1e-6);   // the ring's sizes are floats
    CHECK((Near(oct.At(reach * kPi), {60, 100}) && Near(oct.At(-reach * kPi), {-60, 100})));
    const auto edge = oct.Outline(-reach * kPi, reach * kPi);
    CHECK((edge.size() == 2 + 6 && Near(edge.front(), {-60, 100}) && Near(edge.back(), {60, 100}) && Near(edge[1], {-100, 60})));
    // A circle is as it was: 270 degrees.
    CHECK((grapher::Ring{{0, 0}, 10.f}.OpenReach() == 0.75 && Near(grapher::Ring{{0, 0}, 10.f}.At(kPi / 2), {10, 0})));
    // The axis spreads over that reach.
    const grapher::DialAxis wide = grapher::DialAxis::Make(false, 0.f, 0.f, std::vector<float>{0.f, 1.f}, reach);
    CHECK((std::abs(wide.Angle(0.f) + reach * kPi) < 1e-9 && std::abs(wide.Value(wide.Angle(0.25f)) - 0.25f) < 1e-4f));
    // A header strip clipped to an octagon keeps its top and the cut corners' upper parts.
    const auto band = grapher::ClipAbove({{-60, -100}, {60, -100}, {100, -60}, {100, 60}, {60, 100}, {-60, 100}, {-100, 60}, {-100, -60}}, -80.f);
    CHECK((band.size() == 4 && Near(band[2], {80, -80}) && Near(band[3], {-80, -80})));

    // A dial node drawn as an octagon by its kit.
    grapher::Schema s;
    grapher::NodeType blend{"blend", "Blend"};
    blend.shape      = grapher::NodeShape::Dial;
    blend.titleField = "title";
    s.nodeTypes      = {blend};
    std::string error;
    const auto  kit = grapher::ParseKit(R"K({"nodes": {"blend": {"fill": "#202020", "stroke": "#000000", "metrics": {"header": 40, "row": 18},
        "parts": [{"part": "header", "fill": "#ff0000", "height": 40, "content": [{"part": "text", "id": "title", "from": "field:title"}]},
                  {"part": "rim", "shape": "octagon", "chamfer": 0.4, "stroke": "#ffffff",
                   "content": [{"part": "text", "id": "reading", "color": "#00ff00"}]}]}}})K", error);
    CHECK((kit && error.empty()));
    if (!kit) return;
    CHECK((grapher::ParseKit(grapher::WriteKit(*kit), error) == *kit));
    for (const char* bad : {R"({"nodes": {"x": {"parts": [{"part": "rim", "shape": "hexagon"}]}}})",
                            R"({"nodes": {"x": {"parts": [{"part": "rim", "chamfer": 1.5}]}}})"}) {
        error.clear();
        CHECK((!grapher::ParseKit(bad, error) && !error.empty()));
    }
    grapher::Graph g;
    g.nodes.push_back({"d", "blend", {0, 0, 400, 500}});
    grapher::Node& d = g.nodes[0];
    d.fields      = {{"title", "Locomotion"}};
    d.ports       = {{"in", "", "", grapher::PortDir::In}, {"p0", "", "", grapher::PortDir::Out}, {"p1", "", "", grapher::PortDir::Out},
                     {"p2", "", "", grapher::PortDir::Out}, {"pb", "", "", grapher::PortDir::In}};
    d.ports[0].onHeader = true;
    d.rows = {{"r0", "walk", "0"}, {"r1", "jog", "1"}, {"r2", "run", "2"}, {"b", "blendParameter"}, {"add", "+ children"}};
    for (int k = 0; k < 3; ++k) d.rows[static_cast<std::size_t>(k)].port = "p" + std::to_string(k), d.rows[static_cast<std::size_t>(k)].field = "pos" + std::to_string(k);
    d.rows[3].port = "pb";
    d.dial         = {false, 0.f, 1.f, 1.f, "speed", {"r0", "r1", "r2"}};
    grapher::Canvas c(g, s);
    c.SetKit(&*kit);
    c.SetViewport({800, 600});
    grapher::CardMetrics m;
    m.header      = 40.f;
    m.row         = 18.f;
    m.dialChamfer = 0.4f;
    const grapher::DialShape shape = grapher::DialOf(d, m, d.frame);
    const grapher::Ring&     ring  = shape.rim;
    const grapher::DialAxis  axis  = grapher::AxisOf(d, ring.OpenReach());
    CHECK((shape.Octagon() && shape.rowsTop == 40.f && shape.rowsBottom == 76.f && ring.chamfer == 0.4f));
    // Pins: the rim's rows on the rim at their values (the lowest at the gap's end), the listed binding on the left edge at
    // its line, the input on the left edge at the header's middle.
    CHECK((Near(*c.PortScreenPos("d", "p1"), ring.At(axis.Angle(1.f))) && Near(*c.PortScreenPos("d", "p0"), ring.At(-ring.OpenReach() * kPi))));
    CHECK(Near(*c.PortScreenPos("d", "pb"), {shape.EdgeX(49.f, false), 49.f}));
    CHECK(Near(*c.PortScreenPos("d", "in"), {shape.EdgeX(20.f, false), 20.f}));
    CHECK(shape.EdgeX(20.f, false) > 0.f);   // the cut corner narrows the top
    // Hits follow the octagon: a cut corner is not the node; the listed rows hit as rows; the handles sit inside the rim.
    CHECK(c.HitTest({3, 3}).kind == grapher::HitKind::None);
    CHECK((c.HitTest({200, 45}).row == "b" && c.HitTest({200, 60}).row == "add" && c.HitTest({200, 60}).kind == grapher::HitKind::Row));
    const grapher::Vec2 handle = ring.At(axis.Angle(1.f), 0.8f);
    const grapher::Hit  hit    = c.HitTest(handle);
    CHECK((hit.kind == grapher::HitKind::Handle && hit.row == "r1"));
    // A handle drags along the edges: the value is the pointer's angle about the rim's centre.
    const grapher::Vec2 to = ring.At(0.2 * kPi, 0.8f);
    c.PointerDown(handle, grapher::Button::Left, {});
    c.PointerMove(handle + (to - handle) * 0.5f);
    c.PointerMove(to);
    c.PointerUp(to, grapher::Button::Left);
    CHECK(std::abs(std::stof(g.nodes[0].Field("pos1")) - axis.Value(0.2 * kPi)) < 1e-3f);
    // Drawn: the header band inside the octagon's top, the reading centred on the flat bottom edge.
    bool headerBand = false, readingOnGap = false;
    for (const auto& cmd : c.Draw().cmds) {
        if (const auto* p = std::get_if<grapher::Polygon>(&cmd); p && p->fill == grapher::Color{255, 0, 0})
            headerBand = std::ranges::all_of(p->points, [](grapher::Vec2 v) { return v.y <= 40.01f; }) && p->points.size() >= 4;
        if (const auto* t = std::get_if<grapher::Text>(&cmd); t && t->text.starts_with("speed"))
            readingOnGap = std::abs(t->box.Center().y - (ring.center.y + ring.radius)) < 1.f && std::abs(t->box.Center().x - ring.center.x) < 1.f &&
                           t->color == grapher::Color{0, 255, 0};
    }
    CHECK((headerBand && readingOnGap));
}

// ── K5: the kit's JSON Schema, the example material kit, a rim label's width ─────────────────────────────────

std::string ReadSource(const char* relative)
{
    std::ifstream in(std::filesystem::path(GRAPHER_SOURCE_DIR) / relative, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// @brief `v` checked against JSON Schema node `s`, in the parts of the language schema/kit.schema.json uses: $ref, anyOf,
///        type, enum, pattern, minimum and maximum, properties, additionalProperties (false or a schema), propertyNames'
///        enum, required, items. Problems go to `errors`, each with where it is.
void Validate(const grapher::json::Value& v, const grapher::json::Value& s, const grapher::json::Value& root, const std::string& at,
              std::vector<std::string>& errors)
{
    using grapher::json::Value;
    if (const Value* ref = s.Find("$ref")) {
        const std::string& r      = *ref->String();
        const Value*       target = r.starts_with("#/$defs/") ? root.Find("$defs")->Find(r.substr(8)) : nullptr;
        if (!target) return errors.push_back(at + ": no " + r);
        return Validate(v, *target, root, at, errors);
    }
    if (const Value* any = s.Find("anyOf")) {
        for (const Value& branch : *any->Items()) {
            std::vector<std::string> mine;
            Validate(v, branch, root, at, mine);
            if (mine.empty()) return;
        }
        return errors.push_back(at + ": matches no branch of anyOf");
    }
    if (const Value* type = s.Find("type")) {
        const std::string& t  = *type->String();
        const bool         ok = t == "object" ? v.Members() != nullptr : t == "array" ? v.Items() != nullptr : t == "string" ? v.String() != nullptr
                              : t == "number" ? v.Number() != nullptr : t == "boolean" ? v.Bool() != nullptr : true;
        if (!ok) return errors.push_back(at + ": is not " + t);
    }
    if (const Value* e = s.Find("enum"); e && v.String())
        if (std::ranges::none_of(*e->Items(), [&](const Value& w) { return w.String() && *w.String() == *v.String(); }))
            errors.push_back(at + ": \"" + *v.String() + "\" is not one of its words");
    if (const Value* p = s.Find("pattern"); p && v.String() && !std::regex_search(*v.String(), std::regex(*p->String())))
        errors.push_back(at + ": \"" + *v.String() + "\" does not match " + *p->String());
    if (const Value* lo = s.Find("minimum"); lo && v.Number() && *v.Number() < *lo->Number()) errors.push_back(at + ": below its minimum");
    if (const Value* hi = s.Find("maximum"); hi && v.Number() && *v.Number() > *hi->Number()) errors.push_back(at + ": above its maximum");
    if (const grapher::json::Object* o = v.Members()) {
        const Value* props = s.Find("properties");
        const Value* extra = s.Find("additionalProperties");
        const Value* names = s.Find("propertyNames");
        if (const Value* req = s.Find("required"))
            for (const Value& k : *req->Items())
                if (!v.Find(*k.String())) errors.push_back(at + ": lacks \"" + *k.String() + "\"");
        for (const auto& [key, value] : *o) {
            if (names) Validate(Value(key), *names, root, at + " key " + key, errors);
            if (const Value* p = props ? props->Find(key) : nullptr) {
                Validate(value, *p, root, at + "." + key, errors);
            } else if (extra && extra->Members()) {
                Validate(value, *extra, root, at + "." + key, errors);
            } else if (extra && extra->Bool() && !*extra->Bool()) {
                errors.push_back(at + ": has no member \"" + key + "\"");
            }
        }
    }
    if (const grapher::json::Array* a = v.Items())
        if (const Value* items = s.Find("items"))
            for (std::size_t k = 0; k < a->size(); ++k) Validate((*a)[k], *items, root, at + "[" + std::to_string(k) + "]", errors);
}

std::vector<std::string> AgainstKitSchema(std::string_view json)
{
    std::string error;
    const auto  schema = grapher::json::Parse(ReadSource("schema/kit.schema.json"), error);
    const auto  doc    = grapher::json::Parse(json, error);
    if (!schema || !doc) return {"unreadable: " + error};
    std::vector<std::string> errors;
    Validate(*doc, *schema, *schema, "kit", errors);
    return errors;
}

/// @brief A part with every field a kit can state.
grapher::PartSpec FullPart(grapher::PartKind kind, std::string id)
{
    grapher::PartSpec p{kind, std::move(id)};
    p.visible = true, p.dash = true, p.bold = true, p.italic = true, p.hollow = true;
    p.fill = "#102030", p.stroke = "$a", p.color = "band", p.placeholderColor = "none", p.band = "darker(band, 120)";
    p.strokeWidth = 1.f, p.corner = 2.f, p.opacity = 0.5f, p.width = 3.f, p.top = 0.5f, p.height = 20.f;
    p.size = 12.f, p.scale = 0.9f, p.box = 1.3f, p.advance = 1.4f, p.length = 14.f, p.chamfer = 0.3f;
    p.from = "field:title", p.placeholder = "{type}", p.font = "mono", p.align = "center", p.condition = "box[hot]:selected";
    p.on = {{"click", "act"}, {"dragToNothing", "make"}};
    p.editor = "text", p.shape = "pill", p.format = "{arrow}{value}", p.pin = "end", p.glyph = "square", p.side = "right", p.direction = "out";
    return p;
}

const char* SampleKit();   // the sample kit, defined with the kit tests below

void TestKitSchema()
{
    // A kit stating every field there is writes JSON the schema takes, member by member.
    grapher::Kit kit;
    kit.tokens   = {{"a", "#ffffff"}};
    kit.palettes = {{"groups", {"#ff0000", "$a"}}};
    kit.fonts    = {{"mono", {"Consolas", 11.f, true, false}}};
    kit.metrics  = {8.f, 12.f, 11.f, 40.f, 18.f, 24.f};
    grapher::NodeKind card;
    card.id      = "card:*";
    card.card    = FullPart(grapher::PartKind::Card, "card");
    grapher::PartSpec head = FullPart(grapher::PartKind::Header, "header");
    head.content = {FullPart(grapher::PartKind::Text, "title"), FullPart(grapher::PartKind::Rim, "rim")};
    card.card.content = {head, FullPart(grapher::PartKind::Pins, "ends")};
    card.metrics  = kit.metrics;
    card.minWidth = 100.f, card.maxWidth = 300.f, card.fitWidth = true;
    kit.nodes     = {card};
    grapher::PinSpec pin{"diamond", 4.5f, "always", "#fff", "none", 6.f, false, {{"dragToNothing", "make"}}};
    kit.pins      = {{"role:*", pin}};
    grapher::RowSpec row;
    row.color = row.valueColor = "#abc", row.prefix = "+ ", row.suffix = " s", row.align = "right", row.valueAlign = "left";
    row.bold = row.italic = row.chip = true, row.editor = "slider", row.min = 0.f, row.max = 1.f, row.step = 0.1f, row.reorder = "list";
    row.on   = {{"doubleClick", "edit"}};
    kit.rows = {{"fact", row}};
    grapher::EdgeSpec edge;
    edge.line = "elbow", edge.width = 2.f, edge.widen = 1.f, edge.dash = edge.dotted = edge.onTop = true, edge.arrow = "both", edge.arrowSize = 8.f;
    edge.color = "$a", edge.pinColor = true, edge.opacity = 0.8f, edge.labelColor = "#fff", edge.labelFill = "none", edge.labelScale = 0.9f;
    kit.edges = {{"wire:*", edge}};
    grapher::EdgeSpec lit = edge;
    lit.line.reset();   // a rule never reroutes
    kit.rules = {{*grapher::ParseSelector("card:*[hot]:selected > header"), FullPart(grapher::PartKind::Header, "header"), {}, {}, {}},
                 {*grapher::ParseSelector("*:unwired > pin:role:*"), {}, pin, {}, {}},
                 {*grapher::ParseSelector("* > row:fact"), {}, {}, row, {}},
                 {*grapher::ParseSelector("*:lit > edge:wire:*"), {}, {}, {}, lit}};
    kit.reserved = {{"actions", R"({"make": "node.newFrom"})"}};
    const std::string written = grapher::WriteKit(kit);
    const auto        errors  = AgainstKitSchema(written);
    for (const std::string& e : errors) std::printf("  kit schema: %s\n", e.c_str());
    CHECK(errors.empty());
    // It finds what it should: a member no kit has, a word no field takes, a pin rule's colour where it takes a fill mode.
    CHECK(!AgainstKitSchema(R"({"nodes": {"x": {"colour": "#fff"}}})").empty());
    CHECK(!AgainstKitSchema(R"({"pins": {"x": {"glyph": "star"}}})").empty());
    CHECK(!AgainstKitSchema(R"({"rules": [{"select": "* > pin:x", "fill": "#fff"}]})").empty());
    CHECK(!AgainstKitSchema(R"({"nodes": {"x": {"parts": [{"part": "rim", "chamfer": 2}]}}})").empty());
    CHECK(AgainstKitSchema(R"({"rules": [{"select": "* > pin:x", "fill": "always"}, {"select": "box:selected", "fill": "#fff"}]})").empty());
    // The sample kit and the example material kit are kits the schema takes; a kit may name its schema.
    CHECK(AgainstKitSchema(SampleKit()).empty());
    const std::string material = ReadSource("examples/material/material.kit.json");
    CHECK(AgainstKitSchema(material).empty());
    std::string error;
    CHECK((grapher::ParseKit(material, error) && error.empty()));
}

void TestMaterialExample()
{
    // examples/material: a material graph's schema (value types and conversions) and its kit.
    const auto schema = grapher::ParseSchema(ReadSource("examples/material/material.schema.json"));
    std::string error;
    const auto  kit = grapher::ParseKit(ReadSource("examples/material/material.kit.json"), error);
    CHECK((schema.has_value() && kit.has_value()));
    if (!schema || !kit) return;
    const grapher::Schema& s = *schema;
    CHECK((s.CanConnect("float", "vec3") && s.CanConnect("vec3", "color") && !s.CanConnect("texture", "float") && !s.CanConnect("vec3", "float")));

    grapher::Graph g;
    auto node = [&](std::string id, std::string type, grapher::Rect frame, std::string title) -> grapher::Node& {
        g.nodes.push_back({std::move(id), std::move(type), frame});
        g.nodes.back().fields = {{"title", std::move(title)}};
        return g.nodes.back();
    };
    node("F", "frame", {0, 0, 700, 400}, "Shading").tags = {"frame:shading"};
    grapher::Node& value = node("v", "input:float", {20, 40, 160, 60}, "Roughness");
    value.parent         = "F";
    value.rows           = {{"value", "Value", "0.5", "number"}};
    grapher::Node& tint  = node("c", "input:color", {20, 140, 160, 60}, "Tint");
    tint.parent          = "F";
    tint.rows            = {{"value", "Colour", "#c08040", "colour"}};
    node("add", "math:add", {240, 40, 160, 90}, "Add").parent = "F";
    node("out", "output:surface", {460, 40, 200, 130}, "Surface").parent = "F";
    g.edges = {{"e1", "v", "add", "wire", {}, "out", "a"}, {"e2", "add", "out", "wire", {}, "out", "roughness"},
               {"e3", "c", "out", "wire", {}, "out", "baseColor"}};
    grapher::Canvas c(g, s);
    c.SetKit(&*kit);
    c.SetViewport({900, 600});
    // Kinds by prefix and by id: the inputs' band, the maths', the output's heavier outline; a frame's colour from the
    // palette by its tag.
    CHECK((c.LookOf(g.nodes[1]).band == grapher::Color{0x7a, 0x4a, 0x2e} && c.LookOf(g.nodes[3]).band == grapher::Color{0x3d, 0x5a, 0x80}));
    CHECK(c.LookOf(g.nodes[4]).strokeWidth == 1.5f);
    CHECK(c.LookOf(g.nodes[0]).band == *grapher::ResolveColor("palette(frames, tag:frame)", {}, *kit, g.nodes[0].tags));
    // Pins by value type; a value's row edited with a slider over 0..1.
    const grapher::NodeLook& add = c.LookOf(g.nodes[3]);
    CHECK((add.pins.at("float")[1].color == grapher::Color{0xa1, 0xa1, 0xa1} && add.pins.at("texture")[1].glyph == grapher::PortGlyph::Square));
    const grapher::RowLook& slider = c.LookOf(g.nodes[1]).rows.at("number");
    CHECK((slider.editor == "slider" && slider.min == 0.f && slider.max == 1.f));
    CHECK(c.LookOf(g.nodes[2]).rows.at("colour").editor == "color");
    // Each wire in the colour of the type its source carries.
    bool floatWire = false, colourWire = false;
    for (const auto& cmd : c.Draw().cmds)
        if (const auto* p = std::get_if<grapher::Polyline>(&cmd); p && p->width == 2.f) {
            floatWire  = floatWire || p->color == grapher::Color{0xa1, 0xa1, 0xa1};
            colourWire = colourWire || p->color == grapher::Color{0xc7, 0xc7, 0x29};
        }
    CHECK((floatWire && colourWire));
}

void TestRimLabelWidth()
{
    // An octagon dial's rim label is as wide as it reads: a long name keeps its value.
    grapher::Schema s;
    grapher::NodeType blend{"blend", "Blend"};
    blend.shape = grapher::NodeShape::Dial;
    s.nodeTypes = {blend};
    std::string error;
    const auto  kit = grapher::ParseKit(R"({"nodes": {"blend": {"parts": [{"part": "rim", "shape": "octagon"}]}}})", error);
    CHECK(kit.has_value());
    if (!kit) return;
    grapher::Graph g;
    g.nodes.push_back({"d", "blend", {0, 0, 400, 400}});
    g.nodes[0].ports = {{"p0", "", "", grapher::PortDir::Out}, {"p1", "", "", grapher::PortDir::Out}};
    g.nodes[0].rows  = {{"r0", "Tor_1stP_Walk_And_Then_Some", "82.4"}, {"r1", "Run", "351"}};
    g.nodes[0].rows[0].port = "p0", g.nodes[0].rows[1].port = "p1";
    grapher::Canvas c(g, s);
    c.SetKit(&*kit);
    c.SetViewport({800, 600});
    bool whole = false;
    for (const auto& cmd : c.Draw().cmds)
        if (const auto* t = std::get_if<grapher::Text>(&cmd); t && t->text == "Tor_1stP_Walk_And_Then_Some 82.4")
            whole = t->box.w >= t->size * 0.62f * static_cast<float>(t->text.size());
    CHECK(whole);
}

// ── Highlights, minimap, search ─────────────────────────────────────────────

bool HasFill(const grapher::DrawList& l, auto&& pred)
{
    for (const auto& cmd : l.cmds)
        if (const auto* f = std::get_if<grapher::FillRect>(&cmd); f && pred(*f)) return true;
    return false;
}

void TestHighlights()
{
    grapher::Schema s = FolderSchema();
    grapher::EdgeType top{"top", "top"};
    top.color = {9, 9, 9};
    top.onTop = true;
    s.edgeTypes.push_back(top);
    grapher::Graph g = FolderGraph();   // F1 holds a and F2; F2 holds b; c alone; ab, ca, cb
    g.edges.push_back({"cF", "c", "F1", "top"});
    grapher::Canvas c(g, s);
    c.SetViewport({1000, 600});
    c.camera.pan = {100, 100};
    const grapher::Color glow = c.style.glow;
    auto glowAround = [&](std::string_view id) {
        const grapher::Rect r = c.ScreenRectOf(id);
        return HasFill(c.Draw(), [&](const grapher::FillRect& f) { return f.fill.r == glow.r && f.fill.g == glow.g && f.rect.Holds(r) && f.rect.w > r.w; });
    };

    // A node glows; closed, its group glows for it; a merged wire glows for the wires it stands for.
    c.highlights.glow["a"] = 0.8f;
    CHECK(glowAround("a") && !glowAround("c"));
    c.SetCollapsed("F1", true);
    CHECK(glowAround("F1"));
    c.highlights.edgeGlow["cb"] = 1.f;   // merged into the wire drawn for "ca"
    bool halo = false;
    for (const auto& cmd : c.Draw().cmds)
        if (const auto* p = std::get_if<grapher::Polyline>(&cmd); p && p->color.r == glow.r && p->color.g == glow.g && p->width > 5.f) halo = true;
    CHECK(halo);
    c.SetCollapsed("F1", false);

    // Rings and dimming draw over the node; wires of a selected node take the selection's colour.
    c.highlights.rings["c"] = grapher::Color{10, 200, 30};
    c.highlights.dimmed.insert("b");
    const grapher::Rect rc = c.ScreenRectOf("c"), rb = c.ScreenRectOf("b");
    const auto list = c.Draw();
    CHECK(HasFill(list, [&](const grapher::FillRect& f) { return f.stroke == (grapher::Color{10, 200, 30}) && f.rect.Holds(rc); }));
    CHECK(HasFill(list, [&](const grapher::FillRect& f) { return f.rect == rb && f.fill.a == 150; }));
    c.style.highlightSelectedWires = true;
    c.SelectNodes({"c"});
    int selectedWires = 0;
    for (const auto& cmd : c.Draw().cmds)
        if (const auto* p = std::get_if<grapher::Polyline>(&cmd); p && p->color == c.style.selection && p->points.size() > 2) ++selectedWires;
    CHECK(selectedWires == 3);   // ca, cb, and the on-top cF

    // A wire on top draws after every node; an arrow is a dashed arc with a head, and a loop on one node.
    c.ClearSelection();
    const auto drawn = c.Draw();
    std::size_t lastNode = 0, topWire = 0;
    for (std::size_t k = 0; k < drawn.cmds.size(); ++k) {
        if (const auto* f = std::get_if<grapher::FillRect>(&drawn.cmds[k]); f && f->rect == c.ScreenRectOf("c")) lastNode = k;
        if (const auto* p = std::get_if<grapher::Polyline>(&drawn.cmds[k]); p && p->color == (grapher::Color{9, 9, 9})) topWire = k;
    }
    CHECK(topWire > lastNode && lastNode > 0);
    c.highlights.arrows = {{"a", "c", 0.5f, 1.f}, {"c", "c", 0.f, 0.f}};
    int arcs = 0, heads = 0, flashes = 0;
    for (const auto& cmd : c.Draw().cmds) {
        if (const auto* p = std::get_if<grapher::Polyline>(&cmd); p && p->color == glow && p->dashed) ++arcs;
        if (const auto* p = std::get_if<grapher::Polygon>(&cmd); p && p->fill.r == glow.r && p->points.size() == 3) ++heads;
        if (const auto* f = std::get_if<grapher::Circle>(&cmd); f && f->fill == (grapher::Color{255, 255, 255, 200})) ++flashes;
    }
    CHECK(arcs == 2 && heads == 2 && flashes == 1);
}

void TestMinimapAndSearch()
{
    const grapher::Schema s = FolderSchema();
    grapher::Graph        g = FolderGraph();
    grapher::Canvas       c(g, s);
    c.SetViewport({1000, 600});
    c.style.highlightSelectedWires = true;
    c.SelectNodes({"c"});

    // The whole graph drawn small, the selection in its colour, the view framed; a point on it maps back to the world.
    const grapher::Vec2 size{240, 180};
    const auto          map = c.DrawMinimap(size);
    CHECK(map.size == size && HasFill(map, [&](const grapher::FillRect& f) { return f.fill == c.style.selection; }));
    const grapher::Vec2 w = c.MinimapToWorld({120, 90}, size);
    CHECK(c.FrameOf("F1").x - 60 < w.x && w.x < c.FrameOf("c").Right() + 60);
    c.CenterOnWorld(c.FrameOf("c").Center());
    CHECK(Near(c.ScreenRectOf("c").Center(), {500, 300}));

    // A graph too long to read whole keeps a readable scale and follows the view.
    grapher::Graph tall;
    for (int k = 0; k < 2000; ++k) tall.nodes.push_back({"t" + std::to_string(k), "box", {0, k * 100.f, 100, 60}});
    grapher::Canvas tc(tall, s);
    tc.SetViewport({1000, 600});
    tc.CenterOn("t1500");
    const grapher::Vec2 middle = tc.MinimapToWorld(size * 0.5f, size);
    CHECK(std::abs(middle.y - tc.FrameOf("t1500").Center().y) < 200.f);

    // Search: titles before ids before fields; the start and the whole of a word count most; case doesn't matter.
    grapher::Schema named = s;
    named.nodeTypes[0].fields = {{"name", "Name"}, {"note", "Note"}};
    grapher::Graph h;
    h.nodes.push_back({"x1", "box", {}, {{"name", "Running"}}});
    h.nodes.push_back({"x2", "box", {}, {{"name", "Run"}}});
    h.nodes.push_back({"x3", "box", {}, {{"name", "Walk"}, {"note", "runs late"}}});
    h.nodes.push_back({"run", "box", {}, {{"name", "Idle"}}});
    h.nodes.push_back({"x5", "box", {}, {{"name", "Sit"}}});
    const auto found = grapher::Search(h, named, "RUN");
    CHECK(found.total == 4 && found.hits.size() == 4);
    if (found.hits.size() == 4) {
        CHECK(found.hits[0].node == "x2" && found.hits[0].where == "title");   // the whole title
        CHECK(found.hits[1].node == "run" && found.hits[1].where == "id");      // the whole id
        CHECK(found.hits[2].node == "x1" && found.hits[3].node == "x3" && found.hits[3].where == "Note");
    }
    CHECK(grapher::Search(h, named, "run", 2).hits.size() == 2 && grapher::Search(h, named, "run", 2).total == 4);
    CHECK(grapher::Search(h, named, "").hits.empty() && grapher::Search(h, named, "zzz").total == 0);
}

void TestLiftWires()
{
    // A full input lifted: the wire moves to the input it is dropped on, comes off when dropped on nothing, and stays
    // when put back.
    const grapher::Schema s = PortSchema();
    grapher::Graph g;
    g.nodes.push_back({"v", "value", {0, 0, 160, 80}});
    g.nodes.push_back({"m", "mix", {400, 0, 200, 120}});
    g.edges.push_back({"e", "v", "m", "data", "", "out", "a"});
    grapher::Canvas c(g, s);
    c.SetViewport({800, 600});
    c.liftWires = true;
    int snapshots = 0;
    c.onBeforeChange = [&](std::string_view) { ++snapshots; };
    const grapher::Vec2 a = *c.PortScreenPos("m", "a"), b = *c.PortScreenPos("m", "b");

    c.PointerDown(a, grapher::Button::Left, {});
    c.PointerMove(a + grapher::Vec2{-30, 0});
    c.PointerMove(a);
    c.PointerUp(a, grapher::Button::Left);
    CHECK(g.edges.size() == 1 && g.edges[0].id == "e" && snapshots == 0);   // put back: nothing changed

    Drag(c, a, b);
    CHECK(g.edges.size() == 1 && g.edges[0].from == "v" && g.edges[0].fromPort == "out" && g.edges[0].toPort == "b" && snapshots == 1);
    Drag(c, b, {700, 500});
    CHECK(g.edges.empty() && snapshots == 2);

    // Off, a press on a full input starts a new wire from it instead.
    g.edges.push_back({"e", "v", "m", "data", "", "out", "a"});
    c.liftWires = false;
    Drag(c, a, b);
    CHECK(g.edges.size() == 1 && g.edges[0].toPort == "a");   // an input can't wire to an input

    // Proposed, the move names the wire it replaces.
    c.liftWires = true;
    c.editMode  = grapher::EditMode::Propose;
    std::vector<grapher::Intent> intents;
    c.onIntent = [&](const grapher::Intent& i) { intents.push_back(i); };
    Drag(c, a, b);
    const auto* moved = intents.size() == 1 ? std::get_if<grapher::ConnectWire>(&intents[0].change) : nullptr;
    CHECK(moved && moved->edge.toPort == "b" && moved->replaced == std::vector<std::string>{"e"} && intents[0].label == "Move wire");
}

void TestHostChangesMidDrag()
{
    // A large graph drags on a shortcut cache key; the host moving a node mid-drag (and saying so) is still picked up.
    grapher::Schema s;
    s.nodeTypes.push_back({"box", "Box"});
    grapher::EdgeType line{"line", "line"};
    line.color = {1, 2, 3};
    line.arrow = false;
    s.edgeTypes.push_back(line);
    grapher::Graph g;
    for (int k = 0; k < 2100; ++k) g.nodes.push_back({"n" + std::to_string(k), "box", {k * 200.f, 0, 100, 60}});
    for (int k = 0; k + 1 < 2100; ++k) g.edges.push_back({"e" + std::to_string(k), "n" + std::to_string(k), "n" + std::to_string(k + 1), "line"});
    grapher::Canvas c(g, s);
    c.SetViewport({800, 600});
    (void)c.Draw();
    const grapher::Vec2 grab = c.ScreenRectOf("n0").Center();
    c.PointerDown(grab, grapher::Button::Left, {});
    c.PointerMove(grab + grapher::Vec2{10, 10});
    (void)c.Draw();
    c.PointerMove(grab + grapher::Vec2{20, 20});
    (void)c.Draw();   // the drag runs on the shortcut key now

    g.nodes[1000].frame.y += 300.f;   // the host moves a node the drag doesn't touch, and says so
    c.GraphChanged();
    c.camera.pan = {400.f - 1000 * 200.f, 0.f};   // n999 and n1000 in view
    const grapher::Rect moved{g.nodes[1000].frame.x + c.camera.pan.x, g.nodes[1000].frame.y + c.camera.pan.y, 100, 60};
    bool followed = false;
    for (const auto& cmd : c.Draw().cmds)
        if (const auto* p = std::get_if<grapher::Polyline>(&cmd); p && p->color == line.color && moved.Inflated(1.f).Contains(p->points.back())) followed = true;
    CHECK(followed);   // the wire into it ends on its new frame
    c.PointerUp(grab + grapher::Vec2{20, 20}, grapher::Button::Left);
}
void TestMaterialGaps()
{
    // (c) Content edits draw at once, with a view (a member group) too; a row's port claim moves its pin and wire.
    grapher::Schema s = PortSchema();
    for (const grapher::NodeType& t : FolderSchema().nodeTypes)
        if (t.id == "folder") s.nodeTypes.push_back(t);
    grapher::Graph g;
    g.nodes.push_back({"F", "folder", {0, 0, 10, 10}});
    g.nodes.push_back({"v", "value", {0, 0, 160, 80}});
    g.nodes.push_back({"m", "mix", {400, 0, 200, 160}});
    g.nodes[2].parent = "F";
    g.nodes[2].rows   = {{"r0", "old"}};
    g.edges.push_back({"e", "v", "m", "data", "", "out", "a"});
    grapher::Canvas c(g, s);
    c.SetViewport({1000, 600});
    CHECK(CountText(c.Draw(), "old") == 1);
    g.nodes[2].rows[0].label = "new";
    CHECK(CountText(c.Draw(), "new") == 1 && CountText(c.Draw(), "old") == 0);
    const grapher::Vec2 before = *c.PortScreenPos("m", "a");
    g.nodes[2].rows[0].port = "a";   // the row claims port a: the pin moves onto it
    const grapher::Vec2 after = *c.PortScreenPos("m", "a");
    CHECK(!Near(before, after));
    bool onRow = false;
    for (const auto& cmd : c.Draw().cmds)
        if (const auto* p = std::get_if<grapher::Polyline>(&cmd); p && p->points.size() > 2 && Near(p->points.back(), after)) onRow = true;
    CHECK(onRow);

    // (a) The host refuses a wire, or marks one.
    grapher::Schema marked = PortSchema();
    marked.edgeTypes.push_back({"converted", "converted"});
    grapher::Graph h;
    h.nodes.push_back({"v", "value", {0, 0, 160, 80}});
    h.nodes.push_back({"m", "mix", {400, 0, 200, 120}});
    grapher::Canvas d(h, marked);
    d.checkWire = [](const grapher::Edge& w) {
        if (w.toPort == "b") return grapher::WireCheck{false};
        return grapher::WireCheck{true, "converted", "float to colour"};
    };
    CHECK(!d.CanWire("v", "out", "m", "b") && d.CanWire("v", "out", "m", "a"));
    CHECK(!d.Connect("v", "out", "m", "a").empty() && h.edges.back().type == "converted" && h.edges.back().label == "float to colour");

    // (b) Off, nothing wires to a node as a whole.
    grapher::Schema plain = MakeSchema();
    grapher::Graph  k;
    k.nodes.push_back({"p", "repo", {0, 0, 200, 100}});
    k.nodes.push_back({"q", "repo", {400, 0, 200, 100}});
    grapher::Canvas e(k, plain);
    e.SetViewport({800, 600});
    const grapher::Vec2 handle{200, 50};
    CHECK(e.HitTest(handle).kind == grapher::HitKind::Connect && e.CanWire("p", "", "q", ""));
    e.wholeNodeWires = false;
    CHECK(e.HitTest(handle).kind != grapher::HitKind::Connect && !e.CanWire("p", "", "q", ""));

    // (d) A fitted card grows to show its ports; (f) a port's own tooltip.
    grapher::Schema fit = PortSchema();
    fit.nodeTypes[0].fitHeight = true;   // mix
    fit.nodeTypes[0].ports[2].tooltip = "how much of B";
    grapher::Graph f;
    f.nodes.push_back({"m", "mix", {0, 0, 200, 40}});
    grapher::Canvas fc(f, fit);
    fc.SetViewport({800, 600});
    CHECK(fc.FrameOf("m").h >= grapher::CardHeight(f.nodes[0], fit, {}) && f.nodes[0].frame.h == 40.f);
    CHECK(fc.TooltipAt(*fc.PortScreenPos("m", "t")) == "how much of B");

    // (e) A reroute dot's middle grabs the dot, not its pins; it has no resize grip.
    grapher::Schema dots = PortSchema();
    grapher::NodeType dot{"reroute", "Reroute"};
    dot.shape = grapher::NodeShape::Dot;
    dot.ports = {{"in", ""}, {"out", "", "", grapher::PortDir::Out}};
    dots.nodeTypes.push_back(dot);
    grapher::Graph r;
    r.nodes.push_back({"d", "reroute", {100, 100, 16, 16}});
    grapher::Canvas rc(r, dots);
    rc.SetViewport({800, 600});
    CHECK(rc.HitTest({108, 108}).kind == grapher::HitKind::Node);
    rc.SelectNodes({"d"});
    bool grip = false;
    for (const auto& cmd : rc.Draw().cmds)
        if (const auto* p = std::get_if<grapher::Polygon>(&cmd); p && p->fill == rc.style.selection) grip = true;
    CHECK(!grip);
}

// ── The picture, pinned: every feature drawn and hit, hashed ─────────────────
/// @brief A graph that uses every feature the canvas draws: cards with ports on each side, rows of every style and action,
///        fields, a body, a picture, a drill mark, a note, a pill, a dot, a dial, frame and member groups open and closed,
///        an alias, docked cards, highlights, the selection and a hover.
grapher::Graph GoldenGraph(grapher::Schema& s)
{
    grapher::PortType num{"num", "Number", {90, 200, 200}, grapher::PortGlyph::Circle};
    grapher::PortType sq{"sq", "Square", {200, 120, 90}, grapher::PortGlyph::Square};
    grapher::PortType di{"di", "Diamond", {200, 200, 90}, grapher::PortGlyph::Diamond, {}, "dotted"};
    grapher::PortType ar{"ar", "Arrow", {150, 150, 240}, grapher::PortGlyph::Arrow};
    s.portTypes = {num, sq, di, ar};
    s.rowStyles = {{"key", grapher::Color{220, 180, 60}},
                   {"loud", grapher::Color{240, 120, 120}, grapher::Color{120, 200, 240}, "+ ", true, true},
                   {"chip", grapher::Color{140, 200, 120}, std::nullopt, "", false, false, true}};
    grapher::NodeType box{"box", "Box", {80, 140, 220}};
    box.width       = 220.f;
    box.height      = 140.f;
    box.drillable   = true;
    box.imageHeight = 40.f;
    box.titleField  = "name";
    box.fields      = {{"name", "Name"}, {"kind", "Kind", grapher::FieldKind::Tag, {"a", "b"}}, {"hidden", "Hidden"}};
    box.fields[2].onCard = false;
    box.ports = {{"in", "In", "num", grapher::PortDir::In, grapher::PortSide::Left},
                 {"out", "Out", "num", grapher::PortDir::Out, grapher::PortSide::Right, 0, "kind"},
                 {"top", "", "sq", grapher::PortDir::In, grapher::PortSide::Top},
                 {"bottom", "", "ar", grapher::PortDir::Out, grapher::PortSide::Bottom}};
    box.ports.push_back({"head", "", "di", grapher::PortDir::Out, grapher::PortSide::Right});
    box.ports.back().onHeader = true;
    grapher::NodeType tall = box;
    tall.id        = "tall";
    tall.fitHeight = true;
    tall.band      = {200, 90, 160};
    grapher::NodeType note{"note", "Note", {200, 190, 90}};
    note.shape = grapher::NodeShape::Note;
    grapher::NodeType pill{"pill", "Pill", {120, 200, 140}};
    pill.shape      = grapher::NodeShape::Pill;
    pill.titleField = "";
    pill.fields     = {{"a", "A"}, {"b", "B"}};
    pill.ports      = {{"in", "", "", grapher::PortDir::In}, {"out", "", "", grapher::PortDir::Out}};
    grapher::NodeType dot{"dot", "Dot", {180, 180, 190}};
    dot.shape = grapher::NodeShape::Dot;
    dot.ports = {{"in", "", "", grapher::PortDir::In}, {"out", "", "", grapher::PortDir::Out}};
    dot.ports[1].typeFrom = "in";
    grapher::NodeType dial{"dial", "Dial", {140, 100, 220}};
    dial.shape = grapher::NodeShape::Dial;
    grapher::NodeType frame{"frame", "Frame", {100, 100, 120}};
    frame.shape = grapher::NodeShape::Group;
    grapher::NodeType folder{"folder", "Folder", {160, 90, 200}};
    folder.shape     = grapher::NodeShape::Group;
    folder.groupMode = grapher::GroupMode::Members;
    folder.showCount = true;
    s.nodeTypes = {box, tall, note, pill, dot, dial, frame, folder};
    grapher::EdgeType curve{"curve", "curve", {140, 145, 155}};
    grapher::EdgeType straight{"straight", "straight", {200, 120, 120}, grapher::EdgeLine::Straight};
    straight.dashed = true;
    straight.arrow  = false;
    grapher::EdgeType elbow{"elbow", "elbow", {120, 200, 120}, grapher::EdgeLine::Elbow};
    elbow.portColor = true;
    elbow.width     = 2.f;
    grapher::EdgeType dotted{"dotted", "dotted", {200, 200, 90}};
    dotted.dotted = true;
    dotted.onTop  = true;
    s.edgeTypes = {curve, straight, elbow, dotted};

    grapher::Graph g;
    grapher::Node a{"a", "box", {0, 0, 220, 260}};
    a.fields  = {{"name", "Alpha"}, {"kind", "b"}, {"hidden", "x"}};
    a.body    = "A body that is long enough to wrap onto a second line of the card.";
    a.image   = "thumb";
    a.link    = "child";
    a.tooltip = "tip";
    a.ports.push_back({"extra", "Extra", "sq", grapher::PortDir::In, grapher::PortSide::Left});
    a.rows = {{"r0", "plain", "value"},
              {"r1", "keyed", "", "key"},
              {"r2", "loud", "v", "loud"},
              {"r3", "chip", "", "chip", "", grapher::RowAction::Click},
              {"r4", "pick", "a", "", "", grapher::RowAction::Choice, {"a", "b"}, "kind"},
              {"r5", "press", "Go", "", "", grapher::RowAction::Button},
              {"r6", "wired", "", "", "extra"}};
    a.rows[5].separator = true;
    g.nodes.push_back(a);
    grapher::Node b{"b", "tall", {400, 0, 200, 60}};
    b.fields = {{"name", ""}};   // untitled
    b.rows   = {{"t0", "one"}, {"t1", "two"}, {"t2", "three"}};
    g.nodes.push_back(b);
    grapher::Node alias{"alias", "box", {400, 300, 220, 120}};
    alias.fields  = {{"name", "Alpha"}};
    alias.aliasOf = "a";
    g.nodes.push_back(alias);
    grapher::Node under{"under", "box", {0, 0, 200, 100}};
    under.fields = {{"name", "Under"}};
    under.dock   = {"a", grapher::PortSide::Bottom, 0};
    g.nodes.push_back(under);
    grapher::Node beside{"beside", "pill", {0, 0, 120, 22}};
    beside.fields = {{"a", "one"}, {"b", "two"}};
    beside.dock   = {"a", grapher::PortSide::Right, 0, "r2"};
    g.nodes.push_back(beside);
    g.nodes.push_back({"n", "note", {700, 0, 200, 100}});
    g.nodes.back().body = "A note.";
    g.nodes.push_back({"d", "dot", {700, 160, 20, 20}});
    grapher::Node dl{"dl", "dial", {700, 220, 260, 260}};
    dl.fields = {{"name", "Blend"}};
    dl.dial   = {false, 0.f, 1.f, 0.3f, "speed"};
    dl.ports  = {{"c0", "", "", grapher::PortDir::Out, grapher::PortSide::Right}, {"c1", "", "", grapher::PortDir::Out, grapher::PortSide::Right}};
    dl.rows   = {{"c0", "Walk", "0", "", "c0"}, {"c1", "Run", "1", "", "c1"}};
    g.nodes.push_back(dl);
    g.nodes.push_back({"F", "frame", {1000, -40, 300, 220}});
    g.nodes.push_back({"inF", "box", {1040, 20, 200, 100}});
    g.nodes.back().fields = {{"name", "In frame"}};
    g.nodes.push_back({"M", "folder", {1000, 300, 10, 10}});
    g.nodes.push_back({"m1", "box", {1020, 340, 200, 100}});
    g.nodes.back().parent = "M";
    g.nodes.push_back({"C", "folder", {1000, 600, 10, 10}});
    g.nodes.back().parent = "M";
    g.nodes.push_back({"c1", "box", {1040, 640, 200, 100}});
    g.nodes.back().parent = "C";
    g.edges = {{"e1", "a", "b", "curve", "lbl", "out", ""},
               {"e2", "a", "under", "straight", "", "bottom", "top"},
               {"e3", "a", "n", "elbow", "", "head", ""},
               {"e4", "n", "d", "dotted", "", "", "in"},
               {"e5", "d", "inF", "curve", "", "out", "in"},
               {"e6", "dl", "m1", "curve", "", "c0", "in"},
               {"e7", "dl", "c1", "curve", "", "c1", "in"},
               {"e8", "a", "beside", "curve", "", "out", "in"}};
    return g;
}

void Hash(std::uint64_t& h, std::string_view text)
{
    for (const char ch : text) h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
}

std::string Num(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", static_cast<double>(v));
    return buf;
}
std::string Col(grapher::Color c) { return std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + "," + std::to_string(c.a); }
std::string Box(const grapher::Rect& r) { return Num(r.x) + " " + Num(r.y) + " " + Num(r.w) + " " + Num(r.h); }

/// @brief A draw list as text, one command a line: what changes in it changes the hash.
std::string Describe(const grapher::DrawList& list)
{
    std::string out;
    for (const grapher::DrawCmd& cmd : list.cmds) {
        if (const auto* f = std::get_if<grapher::FillRect>(&cmd))
            out += "R " + Box(f->rect) + " " + Col(f->fill) + " " + Col(f->stroke) + " " + Num(f->strokeWidth) + " " + Num(f->radius) + (f->dashed ? " d" : "");
        else if (const auto* p = std::get_if<grapher::Polyline>(&cmd)) {
            out += "L " + Col(p->color) + " " + Num(p->width) + (p->dashed ? " d" : "") + (p->dotted ? " o" : "");
            for (const grapher::Vec2 v : p->points) out += " " + Num(v.x) + "," + Num(v.y);
        } else if (const auto* g = std::get_if<grapher::Polygon>(&cmd)) {
            out += "P " + Col(g->fill);
            for (const grapher::Vec2 v : g->points) out += " " + Num(v.x) + "," + Num(v.y);
        } else if (const auto* c = std::get_if<grapher::Circle>(&cmd))
            out += "C " + Num(c->center.x) + "," + Num(c->center.y) + " " + Num(c->radius) + " " + Col(c->fill) + " " + Col(c->stroke) + " " + Num(c->strokeWidth);
        else if (const auto* t = std::get_if<grapher::Text>(&cmd))
            out += "T " + Box(t->box) + " " + Num(t->size) + " " + Col(t->color) + (t->bold ? " b" : "") + (t->wrap ? " w" : "") + (t->mono ? " m" : "") +
                   (t->italic ? " i" : "") + " a" + std::to_string(static_cast<int>(t->align)) + " " + t->text;
        else if (const auto* i = std::get_if<grapher::Image>(&cmd))
            out += "I " + Box(i->box) + " " + Num(i->radius) + " " + i->key;
        out += "\n";
    }
    return out;
}

/// @brief Everything the golden graph draws and hits: three zooms, the minimap, a grid of hit tests.
std::string GoldenPicture(const grapher::Kit* kit = nullptr)
{
    grapher::Schema s;
    grapher::Graph  g = GoldenGraph(s);
    grapher::Canvas c(g, s);
    c.SetKit(kit);
    c.SetViewport({1400, 900});
    c.SetCollapsed("C", true);
    c.Frame();
    c.SelectNodes({"a"});
    c.highlights.glow     = {{"a", 0.5f}, {"C", 0.8f}};
    c.highlights.edgeGlow = {{"e1", 0.6f}};
    c.highlights.rings    = {{"b", grapher::Color{255, 0, 0}}};
    c.highlights.dimmed   = {"inF"};
    c.highlights.arrows   = {{"a", "b", 0.5f, 0.2f}};
    c.PointerMove(c.ScreenRectOf("under").Center());   // hover
    std::string out = Describe(c.Draw());
    for (const float zoom : {0.4f, 0.25f}) {
        c.camera.zoom = zoom;
        out += "--- zoom " + Num(zoom) + "\n" + Describe(c.Draw());
    }
    c.Frame();
    out += "--- minimap\n" + Describe(c.DrawMinimap({240, 180}));
    out += "--- hits\n";
    for (int y = 0; y < 900; y += 15)
        for (int x = 0; x < 1400; x += 15) {
            const grapher::Hit h = c.HitTest({static_cast<float>(x), static_cast<float>(y)});
            if (h.kind != grapher::HitKind::None) out += std::to_string(x) + "," + std::to_string(y) + " " + std::to_string(static_cast<int>(h.kind)) + " " + h.id + " " + h.port + " " + h.row + "\n";
        }
    return out;
}

/// @brief The hash of GoldenPicture as the canvas drew it before kits (GRAPHER main 74eef96): a kit-less canvas must keep
///        drawing exactly that.
constexpr std::uint64_t kGoldenPicture = 0x6428cc89fdddaad2ull;

void TestGoldenPicture()
{
    const std::string   picture = GoldenPicture();
    std::uint64_t       h       = 1469598103934665603ull;
    Hash(h, picture);
    std::printf("  golden picture: %zu bytes, hash %016llx\n", picture.size(), static_cast<unsigned long long>(h));
    if (const char* dump = std::getenv("GRAPHER_GOLDEN_DUMP")) {
        if (std::FILE* f = std::fopen(dump, "wb")) {
            std::fwrite(picture.data(), 1, picture.size(), f);
            std::fclose(f);
        }
    }
    if (kGoldenPicture != 0) CHECK(h == kGoldenPicture);
    // The kit a Schema and Style make draws the same picture as no kit at all.
    grapher::Schema s;
    (void)GoldenGraph(s);
    const grapher::Kit same = grapher::KitFromSchema(s, grapher::Style{});
    CHECK(GoldenPicture(&same) == picture);
}

// ── Kits ─────────────────────────────────────────────────────────────────────
constexpr const char* kSampleKit = R"KIT({
    "tokens": { "body": "#2e3036", "hi": "#5ca0ff", "dim": "#ebebf0be" },
    "fonts": { "title": { "size": 13, "bold": true } },
    "metrics": { "padding": 8, "fontSize": 11, "row": 18 },
    "nodes": {
        "box": {
            "fill": "$body", "stroke": "#00000078", "corner": 6,
            "metrics": { "header": 40 },
            "minWidth": 170, "maxWidth": 340, "fitWidth": true,
            "on": { "doubleClick": "drill" },
            "parts": [
                { "part": "header", "fill": "darker(band, 130)", "height": 40, "top": 0.4,
                  "content": [ { "part": "text", "id": "title", "from": "field:name", "font": "title", "color": "#ffffff", "advance": 1.4 },
                               { "part": "text", "id": "sub", "from": "field:sub", "size": 10, "color": "$dim" },
                               { "part": "text", "id": "flag", "from": "field:kind", "if": "[flagged]", "size": 10 } ] },
                { "part": "body" }
            ]
        }
    },
    "rules": [
        { "select": "*:selected > header", "fill": "band" },
        { "select": "box[hot]", "stroke": "#ff0000" },
        { "select": "box[hot]:hovered", "stroke": "#00ff00" },
        { "select": "*:alias", "opacity": 0.5, "dash": true }
    ],
    "pins": { "num": { "glyph": "circle" } },
    "actions": { "drill": { "label": "Open" } }
})KIT";
const char* SampleKit() { return kSampleKit; }

grapher::Schema KitSchema()
{
    grapher::Schema s;
    grapher::NodeType box{"box", "Box", {100, 100, 200}};
    box.fields = {{"name", "Name"}, {"sub", "Sub"}, {"kind", "Kind"}};
    box.ports  = {{"in", "In", "", grapher::PortDir::In, grapher::PortSide::Left}, {"out", "Out", "", grapher::PortDir::Out, grapher::PortSide::Right}};
    s.nodeTypes.push_back(box);
    s.edgeTypes.push_back({"data", "data"});
    return s;
}

void TestKitJson()
{
    std::string error;
    const auto  kit = grapher::ParseKit(kSampleKit, error);
    CHECK(kit.has_value());
    if (!kit) return std::fprintf(stderr, "  kit: %s\n", error.c_str()), void();
    CHECK((kit->nodes.size() == 1 && kit->rules.size() == 4 && kit->pins.size() == 1 && kit->reserved.contains("actions")));
    CHECK((kit->nodes[0].card.on.at("doubleClick") == "drill" && kit->nodes[0].card.content[0].content[2].condition == "[flagged]"));
    // What it writes reads back the same, and writes the same again.
    const std::string text = grapher::WriteKit(*kit);
    const auto        back = grapher::ParseKit(text, error);
    CHECK((back.has_value() && *back == *kit && grapher::WriteKit(*back) == text));
    // The Schema's own kit round-trips too.
    const grapher::Kit schemaKit = grapher::KitFromSchema(KitSchema(), grapher::Style{});
    const auto         again     = grapher::ParseKit(grapher::WriteKit(schemaKit), error);
    CHECK((again && again->nodes == schemaKit.nodes && again->rules == schemaKit.rules && again->tokens == schemaKit.tokens));
    // Mistakes say where they are.
    for (const char* bad : {R"({"rules": [{"select": "box[", "fill": "#fff"}]})", R"({"nodes": {"x": {"fill": "$nope"}}})",
                            R"({"nodes": {"x": {"parts": [{"part": "wing"}]}}})", R"({"nodes": {"x": {"parts": [{"part": "text", "align": "up"}]}}})",
                            R"({"rules": [{"select": "box[t]:sleepy"}]})", R"([1, 2])"}) {
        error.clear();
        CHECK((!grapher::ParseKit(bad, error) && !error.empty()));
    }
}

void TestKitResolve()
{
    // Colours: Qt's lighter and darker, alpha, tokens, the band.
    grapher::Kit tokens;
    tokens.tokens = {{"hi", "#5ca0ff"}, {"ref", "$hi"}};
    const grapher::Color band{10, 20, 30};
    CHECK((grapher::ResolveColor("darker(#808080, 200)", band, tokens) == grapher::Color{64, 64, 64}));
    CHECK((grapher::ResolveColor("lighter(#404040, 200)", band, tokens) == grapher::Color{128, 128, 128}));
    CHECK((grapher::ResolveColor("alpha(#ffffff, 0.5)", band, tokens) == grapher::Color{255, 255, 255, 128}));
    CHECK((grapher::ResolveColor("$ref", band, tokens) == grapher::Color{92, 160, 255} && grapher::ResolveColor("band", band, tokens) == band));
    CHECK((!grapher::ResolveColor("$missing", band, tokens) && !grapher::ResolveColor("darker(#808080)", band, tokens)));
    // Selectors.
    const auto sel = grapher::ParseSelector("box[class:Blend][hot]:selected:hovered > header");
    CHECK((sel && sel->kind == "box" && sel->tags.size() == 2 && sel->states == (grapher::StateSelected | grapher::StateHovered) && sel->part == "header"));
    CHECK((grapher::ParseSelector("*")->kind.empty() && !grapher::ParseSelector("box > ") && !grapher::ParseSelector("box[]")));

    std::string           error;
    const auto            kit = grapher::ParseKit(kSampleKit, error);
    const grapher::Schema s   = KitSchema();
    grapher::Graph        g;
    g.nodes.push_back({"a", "box", {0, 0, 200, 140}});
    g.nodes[0].fields = {{"name", "Alpha"}, {"sub", "class · 0000x0001"}, {"kind", "K"}};
    g.nodes[0].rows   = {{"r0", "first"}, {"r1", "second"}};
    g.nodes[0].tags   = {"hot"};
    g.nodes.push_back({"b", "box", {400, 0, 200, 140}});
    g.nodes[1].fields = {{"name", "Beta"}, {"kind", "K"}};
    g.nodes[1].tags   = {"flagged"};
    g.nodes.push_back({"c", "box", {0, 300, 200, 140}});
    g.nodes[2].aliasOf = "a";
    grapher::Canvas c(g, s);
    c.SetViewport({1000, 600});
    c.SetKit(&*kit);
    c.measure = [](std::string_view text, float size, bool) { return static_cast<float>(text.size()) * size * 0.5f; };

    // The resolved looks: the kind's parts, then the rules by specificity.
    const grapher::NodeLook& a = c.LookOf(g.nodes[0]);
    CHECK((a.metrics.header == 40.f && a.metrics.Row() == 18.f && a.metrics.padding == 8.f && a.metrics.fontSize == 11.f));
    CHECK((a.parts.size() == 2 && a.parts[0].kind == grapher::PartKind::Header && a.parts[0].texts.size() == 2));   // no flag: not tagged
    CHECK((a.parts[0].texts[0].size == 13.f && a.parts[0].texts[0].bold && a.parts[0].texts[0].advance == 1.4f));
    CHECK((a.stroke == grapher::Color{255, 0, 0} && a.fill == grapher::Color{0x2e, 0x30, 0x36}));
    CHECK((c.LookOf(g.nodes[0], grapher::StateHovered).stroke == grapher::Color{0, 255, 0}));   // tag + state beats tag
    CHECK((c.LookOf(g.nodes[0], grapher::StateSelected).parts[0].fill == grapher::Color{100, 100, 200}));
    CHECK((c.LookOf(g.nodes[1]).parts[0].texts.size() == 3 && c.LookOf(g.nodes[1]).stroke == grapher::Color{0, 0, 0, 0x78}));
    const grapher::NodeLook& alias = c.LookOf(g.nodes[2], grapher::StateAlias);
    CHECK((alias.dash && alias.fill.a == 128));   // half as opaque
    // Geometry follows the look: rows start under the 40-unit header, ports pin to rows of 18.
    c.Frame();
    const float          z   = c.camera.zoom;
    const grapher::Rect  ra  = c.ScreenRectOf("a");
    const auto           row = c.RowScreenRect("a", "r1");
    CHECK((row && std::abs(row->y - (ra.y + (40.f + 18.f + 18.f) * z)) < 0.01f));   // one port row, then r0
    const auto pin = c.PortScreenPos("a", "in");
    CHECK((pin && std::abs(pin->y - (ra.y + (40.f + 9.f) * z)) < 0.01f));
    // It fits its width between the kind's bounds, measured.
    CHECK((ra.w >= 170.f * z - 0.01f && ra.w <= 340.f * z + 0.01f));
    // What it draws: the header band, the title and subtitle in it, the body's rows.
    const auto list = c.Draw();
    bool banded = false, title = false, sub = false;
    for (const auto& cmd : list.cmds) {
        if (const auto* f = std::get_if<grapher::FillRect>(&cmd); f && f->fill == a.parts[0].fill && std::abs(f->rect.y - ra.y) < 0.01f) banded = true;
        if (const auto* t = std::get_if<grapher::Text>(&cmd)) {
            title = title || (t->text == "Alpha" && t->bold && std::abs(t->size - 13.f * z) < 0.01f);
            sub   = sub || (t->text == "class · 0000x0001" && t->color == grapher::Color{0xeb, 0xeb, 0xf0, 0xbe});
        }
    }
    CHECK((banded && title && sub));
    // A hit says which part it is on.
    CHECK((c.HitTest({ra.Center().x, ra.y + 10.f * z}).part == "header" && c.HitTest({ra.Center().x, ra.Bottom() - 4.f * z}).part == "body"));
    // A kind defined at runtime takes effect at once.
    grapher::Kit live = *kit;
    c.SetKit(&live);
    grapher::NodeKind tall = live.nodes[0];
    tall.metrics.header    = 60.f;
    live.PutNodeKind(tall);
    CHECK(c.LookOf(g.nodes[0]).metrics.header == 60.f);
}

void TestKitCache()
{
    std::string           error;
    const auto            kit = grapher::ParseKit(kSampleKit, error);
    const grapher::Schema s   = KitSchema();
    grapher::Graph        g;
    for (int k = 0; k < 50; ++k) {
        g.nodes.push_back({"n" + std::to_string(k), "box", {k * 220.f, 0, 200, 140}});
        g.nodes.back().tags = {k % 2 ? "hot" : "cold"};
    }
    grapher::Canvas c(g, s);
    c.SetViewport({4000, 600});
    c.SetKit(&*kit);
    c.Frame();
    (void)c.Draw();
    const std::size_t first = c.ResolvedLooks();
    CHECK((first >= 2 && first <= 8));   // a handful of (tags, state) keys, not one per node
    (void)c.Draw();
    (void)c.Draw();
    CHECK(c.ResolvedLooks() == first);   // a steady frame resolves nothing
    grapher::Kit edited = *kit;
    c.SetKit(&edited);
    ++edited.revision;
    (void)c.Draw();
    CHECK(c.ResolvedLooks() > first);    // a changed kit resolves again
}

// K4a: kinds a host makes from data. A prefix kind stands for every type it starts; a port type names its pin kind; a
// card's band is an expression (a palette slot a tag picks, a colour a rule pins); a wire takes its pin kind's colour.
void TestKitPrefixAndPalette()
{
    // Selectors: a kind may hold `:`, its trailing states peel off, a prefix kind ends in `*`.
    const auto a = grapher::ParseSelector("card:*[t]:hovered > pin:val:*");
    CHECK((a && a->kind == "card:*" && a->tags == std::vector<std::string>{"t"} && a->states == grapher::StateHovered && a->part == "pin:val:*"));
    const auto b = grapher::ParseSelector("card:Combat:selected:hovered");
    CHECK((b && b->kind == "card:Combat" && b->states == (grapher::StateSelected | grapher::StateHovered)));
    const auto c = grapher::ParseSelector(":selected");
    CHECK((c && c->kind.empty() && c->states == grapher::StateSelected));
    CHECK((grapher::ParseSelector("x:bogus")->kind == "x:bogus" && !grapher::ParseSelector("card:") && !grapher::ParseSelector("card::selected")));
    CHECK((grapher::KindCovers("card:*", "card:Combat") && grapher::KindCovers("*", "x") && grapher::KindCovers("a", "a") &&
           !grapher::KindCovers("card:*", "box:A") && !grapher::KindCovers("card:A", "card:AB")));

    grapher::Schema s;
    s.portTypes = {{"cls:A", "A", {1, 2, 3}}, {"cls:B", "B", {4, 5, 6}}, {"val:float", "float", {7, 8, 9}}, {"plain", "plain", {11, 12, 13}}};
    s.portTypes[0].pin = s.portTypes[1].pin = "role:child";
    auto card = [](std::string id, grapher::Color band) {
        grapher::NodeType t{std::move(id), "Card", band};
        t.fields = {{"name", "Name"}};
        t.ports  = {{"p", "P", "cls:A", grapher::PortDir::Out, grapher::PortSide::Right},
                    {"v", "V", "val:float", grapher::PortDir::In, grapher::PortSide::Left},
                    {"q", "Q", "plain", grapher::PortDir::Out, grapher::PortSide::Right}};
        return t;
    };
    s.nodeTypes = {card("card:Combat", {10, 20, 30}), card("card:Idle", {40, 50, 60}), card("card:Pinned", {70, 80, 90})};
    s.edgeTypes = {{"wire:ref", "ref", {9, 9, 9}}, {"plainWire", "plain", {33, 33, 33}}};
    s.edgeTypes[1].portColor = true;

    const char* json = R"K({
        "palettes": {"groups": ["#ff0000", "#00ff00", "#0000ff"]},
        "nodes": {
            "card:*":      {"band": "palette(groups, tag:hue)", "fill": "darker(band, 200)", "stroke": "band"},
            "card:Pinned": {"band": "#123456", "stroke": "band"}
        },
        "pins": {"role:child": {"glyph": "diamond", "color": "#ffaa00"}, "val:*": {"glyph": "square", "size": 5}},
        "edges": {"wire:*": {"width": 4, "pinColor": true, "arrow": "none"}},
        "rules": [
            {"select": "card:*[pin:me]", "band": "#abcdef"},
            {"select": "card:*:selected", "stroke": "#ffffff"},
            {"select": "card:Combat:selected", "stroke": "#000000"}
        ]
    })K";
    std::string error;
    const auto  parsed = grapher::ParseKit(json, error);
    CHECK((parsed && error.empty()));
    if (!parsed) return;
    const grapher::Kit kit = *parsed;
    // It writes back the same, and broken palettes or bands fail.
    const auto back = grapher::ParseKit(grapher::WriteKit(kit), error);
    CHECK((back && *back == kit && kit.palettes.at("groups").size() == 3 && kit.FindNode("card:Pinned")->card.band == "#123456"));
    for (const char* bad : {R"({"palettes": {"p": ["#zz"]}})", R"({"palettes": {"p": "#fff"}})",
                            R"K({"nodes": {"x": {"band": "palette(nope, tag:hue)"}}})K", R"K({"nodes": {"x": {"band": "palette(p, hue)"}}})K"}) {
        error.clear();
        CHECK((!grapher::ParseKit(bad, error) && !error.empty()));
    }
    // Finding kinds: the exact one, else the longest prefix.
    CHECK((kit.FindNode("card:Pinned")->id == "card:Pinned" && kit.FindNode("card:Idle")->id == "card:*" && !kit.FindNode("box:A")));
    CHECK((kit.FindPin("val:float")->id == "val:*" && kit.FindEdge("wire:ref")->id == "wire:*" && !kit.FindPin("cls:A")));
    // Merged over the Schema's kit, the prefix kind replaces the per-type kinds it covers, and their rules.
    const grapher::Kit merged = grapher::MergeKits(grapher::KitFromSchema(s, grapher::Style{}), kit);
    CHECK((merged.FindNode("card:Combat")->id == "card:*" && merged.FindEdge("wire:ref")->id == "wire:*" &&
           merged.FindEdge("plainWire")->id == "plainWire"));

    grapher::Graph g;
    auto node = [&](std::string id, std::string type, float x, std::vector<std::string> tags) {
        g.nodes.push_back({std::move(id), std::move(type), {x, 0, 200, 120}});
        g.nodes.back().fields = {{"name", g.nodes.back().id}};
        g.nodes.back().tags   = std::move(tags);
    };
    node("n1", "card:Combat", 0, {"hue:1"});
    node("n2", "card:Idle", 400, {"hue:4"});
    node("n3", "card:Idle", 800, {"hue:2"});
    node("n4", "card:Idle", 1200, {"hue:Combat"});
    node("n5", "card:Combat", 1600, {"hue:Combat"});
    node("n6", "card:Idle", 2000, {});
    node("n7", "card:Pinned", 2400, {"hue:0"});
    node("n8", "card:Idle", 2800, {"hue:0", "pin:me"});
    g.edges.push_back({"w1", "n1", "n2", "wire:ref", "", "p", "v"});
    g.edges.push_back({"w2", "n3", "n6", "plainWire", "", "q", "v"});
    grapher::Canvas cv(g, s);
    cv.SetKit(&kit);
    cv.SetViewport({4000, 600});
    cv.camera.zoom = 1.f;
    cv.camera.pan = {20, 100};
    auto look = [&](std::size_t i, std::uint32_t st = 0) -> const grapher::NodeLook& { return cv.LookOf(g.nodes[i], st); };
    // The band: the palette entry the hue tag picks (a number modulo its length, else a stable hash of the text).
    const grapher::Color red{255, 0, 0}, green{0, 255, 0}, blue{0, 0, 255};
    CHECK((look(0).band == green && look(0).stroke == green && look(1).band == green && look(2).band == blue));
    std::uint64_t h = 14695981039346656037ull;
    for (const char ch : std::string("Combat")) h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
    const grapher::Color hashed = std::array{red, green, blue}[h % 3];
    CHECK((look(3).band == hashed && look(4).band == hashed));
    CHECK((look(5).band == grapher::Color{40, 50, 60}));    // no hue tag: the type's band
    CHECK((look(6).band == grapher::Color{0x12, 0x34, 0x56} && look(6).stroke == look(6).band));   // the exact kind wins
    CHECK((look(7).band == grapher::Color{0xab, 0xcd, 0xef}));   // a rule on a tag pins it
    CHECK((look(0).fill == grapher::Color{0, 128, 0}));   // the kind's colours read that band
    // Selected: an exact kind's rule over the prefix kind's, over the Schema's.
    CHECK((look(0, grapher::StateSelected).stroke == grapher::Color{0, 0, 0} && look(1, grapher::StateSelected).stroke == grapher::Color{255, 255, 255}));
    // Pins: a port type draws as the pin kind its `pin` names, else the one its id finds; with neither, none.
    CHECK((look(0).pins.at("role:child")[0].glyph == grapher::PortGlyph::Diamond && look(0).pins.contains("val:*") && !look(0).pins.contains("plain")));
    const auto list = cv.Draw();
    const auto v1   = cv.PortScreenPos("n2", "v");
    bool square = false, wire = false, plainWire = false;
    for (const auto& cmd : list.cmds) {
        if (const auto* f = std::get_if<grapher::FillRect>(&cmd); f && v1 && std::abs(f->rect.Center().x - v1->x) < 0.01f &&
                                                                   std::abs(f->rect.w - 10.f) < 0.01f)
            square = true;
        if (const auto* p = std::get_if<grapher::Polyline>(&cmd)) {
            wire      = wire || (p->width == 4.f && p->color == grapher::Color{255, 170, 0});   // its pin kind's colour
            plainWire = plainWire || p->color == grapher::Color{11, 12, 13};   // no pin kind: the port type's
        }
    }
    CHECK((square && wire && plainWire));
    // The Schema's edge rules went with its kind: selected, the kit's wire is as the kit says.
    CHECK((cv.EdgeLookOf(g.edges[0], grapher::StateSelected).width == 4.f));
    // A port type's `pin` reads and writes in the Schema's JSON.
    const auto again = grapher::ParseSchema(grapher::WriteSchema(s));
    CHECK((again && again->portTypes[0].pin == "role:child" && again->portTypes[2].pin.empty()));
}

void TestLargeGraphKit()
{
    // TestLargeGraph's 20,000 cards, with a kit: twelve kinds, forty rules, tags on every card; pin, row and edge kinds,
    // a group kind with a title bar, and every tenth card a pill with parts. Same budgets.
    grapher::Schema s;
    grapher::Kit    kit;
    kit.tokens = {{"body", "#2e3036"}, {"hi", "#5ca0ff"}};
    s.portTypes = {{"num", "Number", {90, 200, 200}}};
    kit.pins    = {{"num", {}}};
    kit.pins[0].spec.glyph = "square";
    kit.pins[0].spec.size  = 5.f;
    kit.pins[0].spec.on    = {{"dragToNothing", "createFrom"}};
    kit.rows               = {{"fact", {}}};
    kit.rows[0].spec.align      = "right";
    kit.rows[0].spec.valueColor = "$hi";
    kit.rows[0].spec.on         = {{"click", "edit"}};
    kit.rows[0].spec.editor     = "text";
    for (int k = 0; k < 12; ++k) {
        grapher::NodeType t{"card" + std::to_string(k), "Card", {static_cast<std::uint8_t>(40 + k * 15), 90, 160}};
        t.ports = {{"in", "In", "num"}, {"out", "Out", "num", grapher::PortDir::Out}};
        s.nodeTypes.push_back(t);
        grapher::NodeKind kind;
        kind.id               = t.id;
        kind.card.fill        = "$body";
        kind.card.stroke      = "#00000078";
        kind.metrics.header   = 40.f;
        grapher::PartSpec header{grapher::PartKind::Header, "header"};
        header.fill = "darker(band, 130)";
        grapher::PartSpec title{grapher::PartKind::Text, "title"};
        title.from = "field:name";
        title.bold = true;
        header.content = {title};
        kind.card.content = {header, grapher::PartSpec{grapher::PartKind::Body, "body"}};
        kit.nodes.push_back(kind);
    }
    for (int k = 0; k < 40; ++k) {
        grapher::Rule r{*grapher::ParseSelector("card" + std::to_string(k % 12) + "[t" + std::to_string(k % 5) + "]" + (k % 3 ? ":hovered" : "")), {}};
        r.patch.stroke = k % 2 ? "$hi" : "#ff8000";
        kit.rules.push_back(r);
    }
    grapher::NodeType folder{"folder", "Folder"};
    folder.shape     = grapher::NodeShape::Group;
    folder.showCount = true;
    s.nodeTypes.push_back(folder);
    grapher::NodeType trans{"trans", "Transition", {200, 120, 60}};
    trans.shape  = grapher::NodeShape::Pill;
    trans.fields = {{"event", "Event"}, {"cond", "Condition"}};
    trans.ports  = {{"in", "", "num"}, {"out", "", "num", grapher::PortDir::Out}};
    s.nodeTypes.push_back(trans);
    s.edgeTypes.push_back({"data", "data"});
    // K3: an edge kind with a rule, the folders' title bar, the pills' parts.
    kit.edges                 = {{"data", {}}};
    kit.edges[0].spec.width   = 2.f;
    kit.edges[0].spec.color   = "$hi";
    kit.edges[0].spec.arrow   = "end";
    grapher::Rule lit{*grapher::ParseSelector("*:lit > edge:data"), {}};
    lit.edge.widen = 1.f;
    kit.rules.push_back(lit);
    grapher::NodeKind folderKind;
    folderKind.id          = "folder";
    folderKind.card.fill   = "alpha(band, 0.1)";
    folderKind.card.stroke = "band";
    grapher::PartSpec titleBar{grapher::PartKind::Header, "header"};
    titleBar.fill = "darker(band, 150)";
    grapher::PartSpec folderTitle{grapher::PartKind::Text, "title"};
    folderTitle.from   = "field:name";
    folderTitle.format = "{value} ({count})";
    folderTitle.bold   = true;
    titleBar.content        = {folderTitle};
    folderKind.card.content = {titleBar};
    kit.nodes.push_back(folderKind);
    grapher::NodeKind transKind;
    transKind.id          = "trans";
    transKind.card.fill   = "$body";
    transKind.card.stroke = "band";
    grapher::PartSpec glyph{grapher::PartKind::Glyph, "event"};
    glyph.glyph = "diamond";
    glyph.color = "#d9733f";
    grapher::PartSpec name{grapher::PartKind::Text, "name"};
    name.from = "field:event";
    grapher::PartSpec cond{grapher::PartKind::Chip, "cond"};
    cond.from  = "field:cond";
    cond.color = "#8fd3e8";
    transKind.card.content = {glyph, name, cond};
    kit.nodes.push_back(transKind);
    grapher::Graph g;
    g.nodes.reserve(20100);
    constexpr int cols = 200, rows = 100;
    for (int y = 0; y < rows; ++y) {
        g.nodes.push_back({"g" + std::to_string(y), "folder", {-20.f, y * 140.f - 30.f, cols * 240.f, 120.f}});
        for (int x = 0; x < cols; ++x) {
            const int k = y * cols + x;
            if (k % 10 == 9) {
                g.nodes.push_back({"n" + std::to_string(k), "trans", {x * 240.f, y * 140.f, 160.f, 24.f}});
                g.nodes.back().fields = {{"event", "Jump"}, {"cond", "if x"}};
            } else {
                g.nodes.push_back({"n" + std::to_string(k), "card" + std::to_string(k % 12), {x * 240.f, y * 140.f, 160.f, 80.f}});
                g.nodes.back().fields = {{"name", "Card"}};
            }
            g.nodes.back().tags   = {"t" + std::to_string(k % 5), "class:C" + std::to_string(k % 7)};
            g.nodes.back().rows   = {{"r", "speed", "1.5", "fact"}};
            if (x > 0) g.edges.push_back({"e" + std::to_string(k), "n" + std::to_string(k - 1), "n" + std::to_string(k), "data", "", "out", "in"});
        }
    }
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); };
    grapher::Canvas c(g, s);
    c.SetViewport({1600, 900});
    c.SetKit(&kit);
    c.CenterOn("n10100", 0.35f);
    (void)c.Draw();
    const std::size_t looks = c.ResolvedLooks();
    auto t0 = Clock::now();
    for (int k = 0; k < 10; ++k) {
        c.PointerMove({static_cast<float>(100 + k * 40), 450.f});   // the hover moves between frames
        (void)c.Draw();
    }
    const double drawMs = ms(t0) / 10.0;
    t0 = Clock::now();
    int hits = 0;
    for (int k = 0; k < 200; ++k) hits += c.HitTest({static_cast<float>(k * 8 % 1600), static_cast<float>(k * 37 % 900)}).kind != grapher::HitKind::None;
    const double        hitMs = ms(t0) / 200.0;
    const grapher::Vec2 grab  = c.ScreenRectOf("n10100").Center();
    c.PointerDown(grab, grapher::Button::Left, {});
    c.PointerMove(grab + grapher::Vec2{10, 0});
    (void)c.Draw();
    t0 = Clock::now();
    c.PointerMove(grab + grapher::Vec2{30, 15});
    (void)c.Draw();
    const double dragMs = ms(t0);
    c.PointerUp(grab + grapher::Vec2{30, 15}, grapher::Button::Left);
    std::printf("  20k graph with a kit (pins, rows, edges, groups, pills): %zu looks resolved (%zu after the hover moved); draw %.2f ms, hit test %.3f ms, drag step %.2f ms\n",
                looks, c.ResolvedLooks(), drawMs, hitMs, dragMs);
    CHECK((looks > 0 && looks <= 14 * 5 * 7 && hits > 0));
    // Like for like (printed, not gated): the same graph with no kit, and the kit without its pin or its row kinds; then
    // nearer, where rows draw (at 0.35 a card shows its title only).
    auto timeDraw = [&](const grapher::Kit* k, float zoom) {
        c.SetKit(k);
        c.CenterOn("n10100", zoom);
        (void)c.Draw();
        const auto t = Clock::now();
        for (int i = 0; i < 10; ++i) {
            c.PointerMove({static_cast<float>(100 + i * 40), 450.f});
            (void)c.Draw();
        }
        return ms(t) / 10.0;
    };
    grapher::Kit noPins = kit, noRows = kit;
    noPins.pins.clear();
    noRows.rows.clear();
    const double plainMs = timeDraw(nullptr, 0.35f), noPinsMs = timeDraw(&noPins, 0.35f), noRowsMs = timeDraw(&noRows, 0.35f);
    const double nearPlainMs = timeDraw(nullptr, 0.6f), nearKitMs = timeDraw(&kit, 0.6f);
    std::printf("    like for like: no kit %.2f ms, the kit without pin kinds %.2f, without row kinds %.2f; detail view (zoom 0.6: rows, pills) no kit %.2f, the full kit %.2f\n",
                plainMs, noPinsMs, noRowsMs, nearPlainMs, nearKitMs);
#ifdef NDEBUG
    CHECK(drawMs <= 4.0);
    CHECK(hitMs <= 0.2);
    CHECK(dragMs <= 8.0);
#endif
}

// ── Kits: pins, rows, editors, actions ───────────────────────────────────────
constexpr const char* kActionKit = R"KIT({
    "pins": {
        "num":  { "glyph": "square", "size": 6, "fill": "always", "color": "#ff0000", "hit": 8,
                  "on": { "click": "pinClick", "dragToNothing": "createFrom" } },
        "flow": { "fill": "never", "label": false }
    },
    "rows": {
        "fact": { "color": "#a0a2ad", "valueColor": "#96c8ff", "prefix": "• ", "suffix": " s", "align": "right", "chip": true },
        "num":  { "editor": "number", "min": 0, "max": 10, "on": { "click": "edit" } },
        "pick": { "editor": "reference", "on": { "doubleClick": "edit" } },
        "item": { "reorder": "items", "on": { "contextMenu": "itemMenu" } },
        "adds": { "on": { "click": "addElement" } }
    },
    "nodes": {
        "box": { "on": { "doubleClick": "drill", "click": "cardClick" },
                 "parts": [ { "part": "header", "on": { "contextMenu": "headerMenu" },
                              "content": [ { "part": "text", "id": "title", "from": "field:name" } ] },
                            { "part": "body" } ] }
    },
    "rules": [
        { "select": "box > pin:num", "size": 5 },
        { "select": "*:wired > pin:num", "color": "#00ff00" },
        { "select": "box[hot] > row:fact", "valueColor": "#ffffff" }
    ]
})KIT";

struct ActionRig {
    grapher::Schema              s;
    grapher::Graph               g;
    grapher::Kit                 kit;
    std::unique_ptr<grapher::Canvas> c;
    std::vector<grapher::Intent> intents;
    ActionRig(grapher::EditMode mode = grapher::EditMode::Propose)
    {
        s.portTypes = {{"num", "Number", {90, 200, 200}}, {"flow", "Flow", {200, 200, 90}, grapher::PortGlyph::Diamond}};
        grapher::NodeType box{"box", "Box", {100, 100, 200}};
        box.fields = {{"name", "Name"}, {"count", "Count"}, {"target", "Target"}};
        box.ports  = {{"in", "In", "num", grapher::PortDir::In, grapher::PortSide::Left},
                      {"out", "Out", "num", grapher::PortDir::Out, grapher::PortSide::Right},
                      {"fl", "Flow", "flow", grapher::PortDir::Out, grapher::PortSide::Right}};
        s.nodeTypes.push_back(box);
        s.edgeTypes.push_back({"data", "data"});
        grapher::Node a{"a", "box", {0, 0, 260, 280}};
        a.fields = {{"name", "Alpha"}, {"count", "3"}, {"target", "Speed"}};
        a.rows   = {{"f", "facts", "1", "fact"}, {"n", "count", "3", "num", "", grapher::RowAction::None, {}, "count"},
                    {"p", "target", "Speed", "pick", "", grapher::RowAction::None, {}, "target"},
                    {"i0", "zero", "", "item"}, {"i1", "one", "", "item"}, {"i2", "two", "", "item"}, {"add", "item", "", "adds"}};
        a.tags   = {"hot"};
        g.nodes.push_back(a);
        g.nodes.push_back({"b", "box", {500, 0, 260, 280}});
        g.nodes[1].fields = {{"name", "Beta"}};
        g.edges.push_back({"w", "a", "b", "data", "", "out", "in"});
        std::string error;
        kit = *grapher::ParseKit(kActionKit, error);
        c   = std::make_unique<grapher::Canvas>(g, s);
        c->editMode = mode;
        c->SetViewport({1200, 700});
        c->SetKit(&kit);
        c->onIntent   = [this](const grapher::Intent& i) { intents.push_back(i); };
        c->choicesFor = [](const grapher::Node&, const grapher::Row&) { return std::vector<std::string>{"Speed", "Direction"}; };
        c->Frame();
    }
    template <class T> const T* Last() const { return intents.empty() ? nullptr : std::get_if<T>(&intents.back().change); }
    void Click(grapher::Vec2 at)
    {
        c->PointerDown(at, grapher::Button::Left, {});
        c->PointerUp(at, grapher::Button::Left);
    }
    grapher::Vec2 Row(std::string_view id) const { return c->RowScreenRect("a", id)->Center(); }
};

void TestKitPinsAndRows()
{
    ActionRig r;
    // The JSON reads pin and row kinds and their rules, and writes them back the same.
    std::string error;
    const auto  back = grapher::ParseKit(grapher::WriteKit(r.kit), error);
    CHECK((back && *back == r.kit && r.kit.pins.size() == 2 && r.kit.rows.size() == 5 && r.kit.rules[1].select.states == grapher::StateWired));
    for (const char* bad : {R"({"pins": {"x": {"glyph": "star"}}})", R"({"pins": {"x": {"fill": "half"}}})",
                            R"({"rows": {"x": {"editor": "knob"}}})", R"({"rows": {"x": {"valueAlign": "up"}}})"}) {
        error.clear();
        CHECK((!grapher::ParseKit(bad, error) && !error.empty()));
    }
    // Resolved: the kind, then the rules on `pin:` and `row:` (a wired pin in its own colour, a tagged node's row).
    const grapher::NodeLook& look = r.c->LookOf(r.g.nodes[0]);
    const auto& num = look.pins.at("num");
    CHECK((num[0].glyph == grapher::PortGlyph::Square && num[0].size == 5.f && num[0].fill == 1 && num[0].hit == 8.f));
    CHECK((num[0].color == grapher::Color{255, 0, 0} && num[1].color == grapher::Color{0, 255, 0}));
    CHECK((look.pins.at("flow")[0].fill == 2 && !look.pins.at("flow")[0].label));
    const grapher::RowLook& fact = look.rows.at("fact");
    CHECK((fact.align == 2 && fact.prefix == "• " && fact.suffix == " s" && fact.chip && fact.valueColor == grapher::Color{255, 255, 255}));
    CHECK((r.c->LookOf(r.g.nodes[1]).rows.at("fact").valueColor == grapher::Color{0x96, 0xc8, 0xff}));   // not tagged
    // Drawn: the square pin at its size, the hidden port label, the row's prefix, alignment and suffix.
    const float z    = r.c->camera.zoom;
    const auto  pin  = r.c->PortScreenPos("a", "in");
    const auto  list = r.c->Draw();
    bool square = false, flowLabel = false, factLine = false, factValue = false;
    for (const auto& cmd : list.cmds) {
        if (const auto* f = std::get_if<grapher::FillRect>(&cmd); f && pin && std::abs(f->rect.Center().x - pin->x) < 0.01f &&
                                                                   std::abs(f->rect.w - 10.f * z) < 0.01f && f->fill == grapher::Color{255, 0, 0})
            square = true;
        if (const auto* t = std::get_if<grapher::Text>(&cmd)) {
            flowLabel = flowLabel || t->text == "Flow";
            factLine  = factLine || (t->text == "• facts" && t->align == grapher::Align::Right);
            factValue = factValue || (t->text == "1 s" && t->color == grapher::Color{255, 255, 255});
        }
    }
    CHECK((square && !flowLabel && factLine && factValue));
    // A pin takes a press out to its size plus its hit reach.
    CHECK(r.c->HitTest(*pin + grapher::Vec2{5.f * z + 7.f, 0}).kind == grapher::HitKind::Port);
    CHECK(r.c->HitTest(*pin + grapher::Vec2{-(5.f * z + 9.f), 0}).kind != grapher::HitKind::Port);
}

void TestKitActions()
{
    ActionRig r;
    // A click on a pin runs its binding; on a row, the row's (over the card's); on the body, the card's.
    r.Click(*r.c->PortScreenPos("a", "in"));
    const auto* pinClick = r.Last<grapher::InvokeAction>();
    CHECK((pinClick && pinClick->action == "pinClick" && pinClick->node == "a" && pinClick->port == "in"));
    r.Click(r.Row("add"));
    const auto* add = r.Last<grapher::InvokeAction>();
    CHECK((add && add->action == "addElement" && add->row == "add" && add->part == "body"));
    const grapher::Rect ra = r.c->ScreenRectOf("a");
    r.Click({ra.Center().x, ra.Bottom() - 6.f});
    CHECK((r.Last<grapher::InvokeAction>() && r.Last<grapher::InvokeAction>()->action == "cardClick"));
    // Double-click: a row bound to edit opens its editor with the host's choices; elsewhere the card drills.
    CHECK(r.c->DoubleClick(r.Row("p")));
    const auto edit = r.c->TakeEditRequest();
    CHECK((edit && edit->editor == "reference" && edit->field == "target" && edit->value == "Speed" && edit->choices.size() == 2));
    CHECK(!r.c->TakeEditRequest());   // once
    CHECK((r.c->DoubleClick({ra.Center().x, ra.Bottom() - 6.f}) && r.Last<grapher::InvokeAction>()->action == "drill"));
    // Context menus: the header's, a row's; the body binds none, so the backend's own menu takes it.
    CHECK((r.c->ContextMenu({ra.Center().x, ra.y + 8.f}) && r.Last<grapher::InvokeAction>()->action == "headerMenu"));
    CHECK((r.c->ContextMenu(r.Row("i1")) && r.Last<grapher::InvokeAction>()->action == "itemMenu" && r.Last<grapher::InvokeAction>()->row == "i1"));
    CHECK(!r.c->ContextMenu({ra.Center().x, ra.Bottom() - 6.f}));
    // A click on a row bound to edit: a number between its bounds, at its line.
    const std::size_t before = r.intents.size();
    r.Click(r.Row("n"));
    const auto num = r.c->TakeEditRequest();
    CHECK((num && num->editor == "number" && num->min == 0.f && num->max == 10.f && num->value == "3"));
    CHECK((num && std::abs(num->at.y - r.c->RowScreenRect("a", "n")->y) < 0.01f && r.intents.size() == before));
    // Committed, it is one proposed step; the host's graph is untouched until it applies it.
    CHECK(r.c->CommitEdit("a", "n", "7"));
    const auto* set = r.Last<grapher::SetValue>();
    CHECK((set && set->node == "a" && set->row == "n" && set->field == "count" && set->value == "7" && r.g.nodes[0].Field("count") == "3"));
    r.c->Refresh();
    // A wire let go on nothing: the pin's binding runs; a pin without one tells the host, with its type.
    auto dragOff = [&](std::string_view port) {
        const grapher::Vec2 from = *r.c->PortScreenPos("a", port);
        r.c->PointerDown(from, grapher::Button::Left, {});
        r.c->PointerMove(from + grapher::Vec2{40, 300});
        r.c->PointerUp(from + grapher::Vec2{40, 300}, grapher::Button::Left);
    };
    dragOff("out");
    CHECK((r.Last<grapher::InvokeAction>() && r.Last<grapher::InvokeAction>()->action == "createFrom" && r.Last<grapher::InvokeAction>()->port == "out"));
    dragOff("fl");
    CHECK((r.Last<grapher::ConnectToNothing>() && r.Last<grapher::ConnectToNothing>()->type == "flow"));
}

void TestKitEditsAndReorder()
{
    // Direct mode: a commit sets the field and the row, as one change the host snapshots first.
    ActionRig d(grapher::EditMode::Direct);
    int befores = 0;
    d.c->onBeforeChange = [&](std::string_view) { ++befores; };
    CHECK((d.c->CommitEdit("a", "n", "9") && d.g.nodes[0].Field("count") == "9" && d.g.nodes[0].FindRow("n")->value == "9" && befores == 1));
    CHECK(!d.c->CommitEdit("a", "nope", "1"));
    // A row of a list drags among its list: dropped on the last item it goes there, and never past its list.
    auto drag = [&](ActionRig& r, std::string_view row, grapher::Vec2 to) {
        const grapher::Vec2 from = r.Row(row);
        r.c->PointerDown(from, grapher::Button::Left, {});
        r.c->PointerMove(from + grapher::Vec2{0, 12});
        r.c->PointerMove(to);
        r.c->PointerUp(to, grapher::Button::Left);
    };
    drag(d, "i0", d.Row("i2"));
    std::vector<std::string> order;
    for (const grapher::Row& row : d.g.nodes[0].rows) order.push_back(row.id);
    CHECK((order == std::vector<std::string>{"f", "n", "p", "i1", "i2", "i0", "add"}));
    drag(d, "i1", d.Row("add") + grapher::Vec2{0, 200});   // far below: the end of the list, not past it
    order.clear();
    for (const grapher::Row& row : d.g.nodes[0].rows) order.push_back(row.id);
    CHECK((order == std::vector<std::string>{"f", "n", "p", "i2", "i0", "i1", "add"}));
    // Proposed: an intent saying where it goes; the node doesn't move.
    ActionRig p;
    const grapher::Rect before = p.c->ScreenRectOf("a");
    drag(p, "i2", p.Row("i0"));
    const auto* moved = p.Last<grapher::ReorderRow>();
    CHECK((moved && moved->node == "a" && moved->row == "i2" && moved->to == 3 && p.c->ScreenRectOf("a") == before));
    // A plain click on a list row is still a click: no reorder.
    p.intents.clear();
    p.Click(p.Row("i1"));
    CHECK(!p.Last<grapher::ReorderRow>());
}

void TestRowRangeVetoAndWholeWires()
{
    // A row's own range wins over its kind's, bound by bound: min and step from the row, max from the kit.
    ActionRig r;
    r.g.nodes[0].rows[1].min  = 2.f;   // "n": the kit's num row is 0..10
    r.g.nodes[0].rows[1].step = 0.5f;
    r.Click(r.Row("n"));
    const auto num = r.c->TakeEditRequest();
    CHECK((num && num->min == 2.f && num->max == 10.f && num->step == 0.5f));

    // The host refuses an end live while the wire is dragged: ringed in the refused colour, the wire too, its reason
    // at the pointer; let go there, nothing is wired.
    const grapher::Schema s = PortSchema();
    grapher::Graph g;
    g.nodes.push_back({"v", "value", {0, 0, 160, 80}});
    g.nodes.push_back({"m", "mix", {400, 0, 200, 120}});
    grapher::Canvas c(g, s);
    c.SetViewport({800, 600});
    std::string seenType;
    c.checkWire = [&](const grapher::Edge& w) {
        seenType = w.type;
        if (w.toPort == "b") return grapher::WireCheck{false, {}, {}, "would make a loop"};
        return grapher::WireCheck{};
    };
    const grapher::Vec2 vo = *c.PortScreenPos("v", "out"), ma = *c.PortScreenPos("m", "a"), mb = *c.PortScreenPos("m", "b");
    c.PointerDown(vo, grapher::Button::Left, {});
    c.PointerMove(vo + grapher::Vec2{30, 30});
    c.PointerMove(mb);
    CHECK((c.Refused() == grapher::RefusedEnd{"m", "b", "would make a loop"} && seenType == "data"));
    CHECK(c.TooltipAt(mb) == "would make a loop");
    bool ring = false, redWire = false;
    for (const auto& cmd : c.Draw().cmds) {
        if (const auto* k = std::get_if<grapher::Circle>(&cmd); k && k->stroke == c.style.refused && Near(k->center, mb)) ring = true;
        if (const auto* p = std::get_if<grapher::Polyline>(&cmd); p && p->color == c.style.refused) redWire = true;
    }
    CHECK(ring && redWire);
    c.PointerMove(ma);   // an end it may take: nothing refused
    CHECK(c.Refused().node.empty());
    c.PointerMove(*c.PortScreenPos("m", "out"));   // the schema refuses Out to Out: shown, with no reason of the host's
    CHECK((c.Refused() == grapher::RefusedEnd{"m", "out", ""}));
    c.PointerMove(mb);
    c.PointerUp(mb, grapher::Button::Left);
    CHECK(g.edges.empty() && c.Refused().node.empty());

    // Dropped on the card as a whole, where no port takes it: the node is refused, with the host's reason.
    c.checkWire = [](const grapher::Edge& w) { return w.to == "m" ? grapher::WireCheck{false, {}, {}, "not into a mix"} : grapher::WireCheck{}; };
    const grapher::Vec2 body = c.ScreenRectOf("m").Center();
    c.PointerDown(vo, grapher::Button::Left, {});
    c.PointerMove(vo + grapher::Vec2{30, 30});
    c.PointerMove(body);
    CHECK((c.Refused() == grapher::RefusedEnd{"m", "", "not into a mix"}));
    bool nodeRing = false;
    for (const auto& cmd : c.Draw().cmds)
        if (const auto* f = std::get_if<grapher::FillRect>(&cmd); f && f->stroke == c.style.refused && f->fill.a == 0) nodeRing = true;
    CHECK(nodeRing);
    c.PointerUp(body, grapher::Button::Left);
    CHECK(g.edges.empty());

    // Whole-node wires off for one node: no connect handle on it, nothing wires to it, and a drop on it isn't shown
    // refused (it is simply no target).
    grapher::Graph k;
    k.nodes.push_back({"p", "repo", {0, 0, 200, 100}});
    k.nodes.push_back({"q", "repo", {400, 0, 200, 100}});
    k.nodes[1].wholeNodeWires = false;
    const grapher::Schema plain = MakeSchema();
    grapher::Canvas       e(k, plain);
    e.SetViewport({800, 600});
    CHECK(e.HitTest({200, 50}).kind == grapher::HitKind::Connect && e.HitTest({600, 50}).kind != grapher::HitKind::Connect);
    CHECK(!e.CanWire("p", "", "q", "") && e.TakesWholeWires(k.nodes[0]) && !e.TakesWholeWires(k.nodes[1]));
    e.PointerDown({200, 50}, grapher::Button::Left, {});
    e.PointerMove({300, 50});
    e.PointerMove({500, 50});
    CHECK(e.Refused().node.empty());
    e.PointerUp({500, 50}, grapher::Button::Left);
    CHECK(k.edges.empty());

    // Off for a whole type; a node turns it back on for itself, and both ends need it.
    grapher::Schema off = MakeSchema();
    off.nodeTypes[0].wholeNodeWires = false;   // repo
    grapher::Graph h;
    h.nodes.push_back({"p", "repo", {0, 0, 200, 100}});
    h.nodes.push_back({"q", "repo", {400, 0, 200, 100}});
    grapher::Canvas f(h, off);
    f.SetViewport({800, 600});
    CHECK(f.HitTest({200, 50}).kind != grapher::HitKind::Connect && !f.CanWire("p", "", "q", ""));
    h.nodes[1].wholeNodeWires = true;
    CHECK(!f.CanWire("p", "", "q", ""));
    h.nodes[0].wholeNodeWires = true;
    CHECK(f.CanWire("p", "", "q", "") && f.HitTest({200, 50}).kind == grapher::HitKind::Connect);
    f.wholeNodeWires = false;   // the canvas's switch still turns it off for every node
    CHECK(!f.CanWire("p", "", "q", ""));

    // The type's switch round-trips through the schema's JSON, written only when off.
    const auto back = grapher::ParseSchema(grapher::WriteSchema(off));
    CHECK((back && back->nodeTypes[0].wholeNodeWires == false && back->nodeTypes[1].wholeNodeWires == true));
    CHECK(grapher::WriteSchema(MakeSchema()).find("wholeNodeWires") == std::string::npos);
}


// ── Kits: groups, pills, the dial, edges ─────────────────────────────────────
constexpr const char* kPartKit = R"KIT({
    "tokens": { "hi": "#5ca0ff" },
    "edges": {
        "data": { "line": "elbow", "width": 3, "arrow": "both", "color": "#ff0000", "labelColor": "#00ff00" },
        "flow": { "pinColor": true, "dotted": true, "arrow": "none" }
    },
    "nodes": {
        "folder": { "fill": "#10203040", "stroke": "band",
                    "parts": [ { "part": "header", "fill": "#334455",
                                 "content": [ { "part": "text", "id": "title", "from": "field:name", "format": "{arrow}{value} · {count}",
                                                "color": "#ffffff", "bold": true },
                                              { "part": "text", "id": "detail", "from": "fields", "color": "#aaaaaa" } ] },
                               { "part": "pins", "id": "ends", "pin": "flow", "visible": false } ] },
        "trans": { "fill": "#202020", "stroke": "#808080",
                   "parts": [ { "part": "bar", "id": "bar", "width": 4, "color": "band" },
                              { "part": "glyph", "id": "event", "glyph": "diamond", "color": "#d9733f" },
                              { "part": "text", "id": "name", "from": "field:event", "color": "$hi" },
                              { "part": "chip", "id": "cond", "from": "field:cond", "color": "#8fd3e8", "on": { "click": "editCond" } },
                              { "part": "stub", "id": "stub", "side": "right", "length": 20, "direction": "out", "color": "#ffffff" } ] },
        "dial": { "parts": [ { "part": "rim", "id": "rim", "stroke": "#112233", "width": 5,
                               "content": [ { "part": "bar", "id": "needle", "color": "#00ffff", "width": 3 },
                                            { "part": "glyph", "id": "handle", "glyph": "square", "color": "#ff00ff", "size": 6 },
                                            { "part": "text", "id": "label", "color": "#ffff00" },
                                            { "part": "text", "id": "reading", "color": "#00ff00" } ] } ] }
    },
    "rules": [
        { "select": "folder:collapsed", "shape": "pill" },
        { "select": "folder:collapsed > ends", "visible": true },
        { "select": "trans[noEvent] > event", "hollow": true },
        { "select": "*[write] > edge:data", "color": "#0000ff" },
        { "select": "*:selected > edge:data", "widen": 2 },
        { "select": "*:merged > edge:flow", "dash": true, "dotted": false }
    ]
})KIT";

struct PartRig {
    grapher::Schema                  s;
    grapher::Graph                   g;
    grapher::Kit                     kit;
    std::unique_ptr<grapher::Canvas> c;
    std::vector<grapher::Intent>     intents;
    PartRig()
    {
        s.portTypes = {{"num", "Number", {90, 200, 200}}, {"flow", "Flow", {200, 200, 90}}};
        grapher::NodeType box{"box", "Box", {100, 100, 200}};
        box.fields = {{"name", "Name"}};
        box.ports  = {{"in", "In", "num", grapher::PortDir::In, grapher::PortSide::Left},
                      {"out", "Out", "num", grapher::PortDir::Out, grapher::PortSide::Right},
                      {"fl", "Flow", "flow", grapher::PortDir::Out, grapher::PortSide::Right}};
        grapher::NodeType folder{"folder", "Folder", {160, 90, 200}};
        folder.shape      = grapher::NodeShape::Group;
        folder.groupMode  = grapher::GroupMode::Members;
        folder.showCount  = true;
        folder.titleField = "name";
        folder.fields     = {{"name", "Name"}, {"note", "Note"}};
        grapher::NodeType trans{"trans", "Transition", {200, 120, 60}};
        trans.shape  = grapher::NodeShape::Pill;
        trans.fields = {{"event", "Event"}, {"cond", "Condition"}};
        grapher::NodeType dial{"dial", "Dial", {140, 100, 220}};
        dial.shape = grapher::NodeShape::Dial;
        s.nodeTypes = {box, folder, trans, dial};
        s.edgeTypes = {{"data", "data"}, {"flow", "flow"}};

        g.nodes.push_back({"a", "box", {0, 0, 200, 120}});
        g.nodes.back().fields = {{"name", "A"}};
        g.nodes.push_back({"G", "folder", {400, 0, 10, 10}});
        g.nodes.back().fields = {{"name", "G"}, {"note", "n"}};
        g.nodes.push_back({"b1", "box", {420, 60, 160, 80}});
        g.nodes.back().parent = "G";
        g.nodes.push_back({"b2", "box", {420, 200, 160, 80}});
        g.nodes.back().parent = "G";
        g.nodes.push_back({"t", "trans", {0, 300, 240, 24}});
        g.nodes.back().fields = {{"event", "Jump"}, {"cond", "if x"}};
        g.nodes.push_back({"t2", "trans", {0, 360, 240, 24}});
        g.nodes.back().fields = {{"cond", "if y"}};
        g.nodes.back().tags   = {"noEvent"};
        grapher::Node dl{"dl", "dial", {700, 300, 260, 260}};
        dl.dial  = {false, 0.f, 1.f, 0.3f, "speed"};
        dl.ports = {{"c0", "", "", grapher::PortDir::Out, grapher::PortSide::Right}};
        dl.rows  = {{"c0", "Walk", "0", "", "c0"}};
        g.nodes.push_back(dl);
        g.edges.push_back({"w1", "a", "b1", "data", "lbl", "out", "in"});
        g.edges.back().tags = {"write"};
        g.edges.push_back({"w2", "a", "b2", "flow", "", "fl", "in"});
        std::string error;
        kit = *grapher::ParseKit(kPartKit, error);
        c   = std::make_unique<grapher::Canvas>(g, s);
        c->editMode = grapher::EditMode::Propose;
        c->SetViewport({1400, 900});
        c->SetKit(&kit);
        c->onIntent = [this](const grapher::Intent& i) { intents.push_back(i); };
        c->Frame();
    }
    template <class T> std::vector<const T*> All(const grapher::DrawList& list) const
    {
        std::vector<const T*> out;
        for (const auto& cmd : list.cmds)
            if (const auto* x = std::get_if<T>(&cmd)) out.push_back(x);
        return out;
    }
};

void TestKitEdges()
{
    PartRig r;
    // The JSON reads edge kinds and edge rules and writes them back the same; a rule can't change a line, and bad words fail.
    std::string error;
    const auto  back = grapher::ParseKit(grapher::WriteKit(r.kit), error);
    CHECK((back && *back == r.kit && r.kit.edges.size() == 2 && r.kit.rules[3].select.part == "edge:data"));
    for (const char* bad : {R"({"rules": [{"select": "* > edge:data", "line": "curve"}]})", R"({"edges": {"x": {"arrow": "sideways"}}})",
                            R"({"edges": {"x": {"line": "wiggly"}}})", R"({"nodes": {"x": {"shape": "blob"}}})",
                            R"({"nodes": {"x": {"parts": [{"part": "stub", "side": "up"}]}}})"}) {
        error.clear();
        CHECK((!grapher::ParseKit(bad, error) && !error.empty()));
    }
    // Resolved: the kind, its tag rule (over the selection's colour), the selection's widening (the kit's, after the Schema's).
    const grapher::Edge& w1 = r.g.edges[0];
    const grapher::EdgeLook& plain = r.c->EdgeLookOf(w1);
    CHECK((plain.line == grapher::EdgeLine::Elbow && plain.width == 3.f && plain.arrow == 3 && plain.color == grapher::Color{0, 0, 255}));
    const grapher::EdgeLook& chosen = r.c->EdgeLookOf(w1, grapher::StateSelected);
    CHECK((chosen.width == 5.f && chosen.color == grapher::Color{0, 0, 255}));
    CHECK((r.c->EdgeLookOf(r.g.edges[1]).pinColor && r.c->EdgeLookOf(r.g.edges[1]).dotted && r.c->EdgeLookOf(r.g.edges[1]).arrow == 0));
    // The line is the kind's: routing reads it from the options, the Schema otherwise.
    grapher::RoutingOptions o;
    o.lines["data"] = grapher::EdgeLine::Elbow;
    CHECK((o.LineFor("data", r.s) == grapher::EdgeLine::Elbow && o.LineFor("flow", r.s) == grapher::EdgeLine::Curve));
    // Drawn: the wire in its colour and width, an arrowhead at each end, its label in the kind's colour; the flow wire in its
    // pin's colour, dotted, with no arrowhead.
    const auto list  = r.c->Draw();
    std::vector<grapher::Vec2> route;
    int heads = 0;
    bool label = false, flow = false;
    for (const auto* p : r.All<grapher::Polyline>(list)) {
        if (p->color == grapher::Color{0, 0, 255} && p->width == 3.f) route = p->points;
        flow = flow || (p->color == grapher::Color{200, 200, 90} && p->dotted && p->width == 1.5f);
    }
    for (const auto* p : r.All<grapher::Polygon>(list)) heads += p->fill == grapher::Color{0, 0, 255};
    for (const auto* t : r.All<grapher::Text>(list)) label = label || (t->text == "lbl" && t->color == grapher::Color{0, 255, 0});
    CHECK((route.size() >= 2 && heads == 2 && label && flow));
    // A rule never reroutes: selected, the same path, wider.
    r.c->SelectEdge("w1");
    bool same = false;
    const auto chosenList = r.c->Draw();
    for (const auto* p : r.All<grapher::Polyline>(chosenList)) same = same || (p->width == 5.f && p->points == route);
    CHECK(same);
}

void TestKitGroups()
{
    PartRig r;
    // Open: the body's tint, the title bar, the title by its format (arrow, name, count), the detail line.
    auto list = r.c->Draw();
    bool body = false, bar = false, title = false, detail = false;
    for (const auto* f : r.All<grapher::FillRect>(list)) {
        body = body || (f->fill == grapher::Color{0x10, 0x20, 0x30, 0x40} && f->stroke == grapher::Color{160, 90, 200});
        bar  = bar || f->fill == grapher::Color{0x33, 0x44, 0x55};
    }
    for (const auto* t : r.All<grapher::Text>(list)) {
        title  = title || (t->text == "\u25be G \u00b7 2" && t->color == grapher::Color{255, 255, 255} && t->bold);
        detail = detail || (t->text == "n" && t->color == grapher::Color{0xaa, 0xaa, 0xaa});
    }
    CHECK((body && bar && title && detail));
    CHECK(!r.c->PortScreenPos("G", grapher::kGroupIn));   // open, it shows no end pins
    // Closed: a pill (its corner half its height), the title's arrow turned, and two end pins its merged wires end on.
    r.c->SetCollapsed("G", true);
    list = r.c->Draw();
    const grapher::Rect gr = r.c->ScreenRectOf("G");
    bool pill = false, closedTitle = false;
    for (const auto* f : r.All<grapher::FillRect>(list))
        pill = pill || (f->rect == gr && std::abs(f->radius - gr.h * 0.5f) < 0.01f);
    for (const auto* t : r.All<grapher::Text>(list)) closedTitle = closedTitle || t->text == "\u25b8 G \u00b7 2";
    CHECK((pill && closedTitle));
    const auto in  = r.c->PortScreenPos("G", grapher::kGroupIn);
    const auto out = r.c->PortScreenPos("G", grapher::kGroupOut);
    CHECK((in && out && std::abs(in->x - gr.x) < 0.5f && std::abs(out->x - gr.Right()) < 0.5f));
    // Both wires into it end on its In pin: the data wire (merged: dashed by the Schema's rule) and the flow wire (dashed,
    // not dotted, by the kit's).
    bool dataEnd = false, flowEnd = false;
    for (const auto* p : r.All<grapher::Polyline>(list)) {
        if (p->points.size() < 2 || !in || grapher::Distance(p->points.back(), *in) > 10.f) continue;
        dataEnd = dataEnd || (p->color == grapher::Color{0, 0, 255} && p->dashed);
        flowEnd = flowEnd || (p->color == grapher::Color{200, 200, 90} && p->dashed && !p->dotted);
    }
    CHECK((dataEnd && flowEnd));
    // An end pin is shown, not edited: a press on it is a press on the group.
    CHECK((in && r.c->HitTest(*in).kind != grapher::HitKind::Port));
}

void TestKitPillsAndDial()
{
    PartRig r;
    const auto          list = r.c->Draw();
    const grapher::Rect t    = r.c->ScreenRectOf("t");
    const grapher::Rect t2   = r.c->ScreenRectOf("t2");
    // A pill lays its parts along its line: the bar in its band, the event diamond, the name, the condition chip, a stub out
    // of its right end.
    bool barIn = false, chip = false, filled = false, hollow = false, stub = false, name = false, cond = false;
    grapher::Rect chipBox;
    for (const auto* f : r.All<grapher::FillRect>(list)) {
        barIn = barIn || (f->fill == grapher::Color{200, 120, 60} && t.Contains(f->rect.Center()) && f->rect.Center().x < t.x + t.h);
        if (f->fill == grapher::Color{0x8f, 0xd3, 0xe8, 40} && t.Contains(f->rect.Center())) chip = true, chipBox = f->rect;
    }
    for (const auto* p : r.All<grapher::Polygon>(list)) {
        filled = filled || (p->fill == grapher::Color{0xd9, 0x73, 0x3f} && t.Contains(p->points.front()));
        hollow = hollow || (p->fill == grapher::Color{0x20, 0x20, 0x20} && t2.Contains(p->points[1]));   // noEvent: outlined
    }
    for (const auto* p : r.All<grapher::Polyline>(list))
        stub = stub || (p->color == grapher::Color{255, 255, 255} && p->points.size() == 2 && p->points[0].x >= t.Right() - 0.5f &&
                        p->points[1].x > p->points[0].x);
    for (const auto* x : r.All<grapher::Text>(list)) {
        name = name || (x->text == "Jump" && x->color == grapher::Color{0x5c, 0xa0, 0xff});
        cond = cond || (x->text == "if x" && x->align == grapher::Align::Center);
    }
    CHECK((barIn && chip && filled && hollow && stub && name && cond));
    // Its parts come in order, left to right, and a press on the chip runs the chip's binding.
    CHECK(chip && chipBox.x > t.x + t.h * 0.4f);
    r.c->PointerDown(chipBox.Center(), grapher::Button::Left, {});
    r.c->PointerUp(chipBox.Center(), grapher::Button::Left);
    const auto* edit = r.intents.empty() ? nullptr : std::get_if<grapher::InvokeAction>(&r.intents.back().change);
    CHECK((edit && edit->action == "editCond" && edit->part == "cond" && edit->node == "t"));
    // The dial's rim: the ring, the needle and hub, the square handles, the labels and the reading in the kit's colours.
    bool ring = false, needle = false, handle = false, labelText = false, reading = false;
    for (const auto* p : r.All<grapher::Polyline>(list)) {
        ring   = ring || (p->color == grapher::Color{0x11, 0x22, 0x33} && p->points.size() > 30);
        needle = needle || (p->color == grapher::Color{0, 255, 255} && p->width == 3.f);
    }
    for (const auto* f : r.All<grapher::FillRect>(list)) handle = handle || f->fill == grapher::Color{255, 0, 255};
    for (const auto* x : r.All<grapher::Text>(list)) {
        labelText = labelText || (x->text.starts_with("Walk") && x->color == grapher::Color{255, 255, 0});
        reading   = reading || (x->text.starts_with("speed") && x->color == grapher::Color{0, 255, 0});
    }
    CHECK((ring && needle && handle && labelText && reading));
}
} // namespace

int main()
{
#ifdef _MSC_VER
    // A failed debug assertion prints and aborts instead of opening a dialog nobody running the suite will see.
    for (int kind : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
    }
#endif
    TestColor();
    TestGraph();
    TestCamera();
    TestGestures();
    TestDraw();
    TestTransitionsAndDefaults();
    TestRouting();
    TestBackendDrag();
    TestGroupMembership();
    TestGroupGestures();
    TestGroupDraw();
    TestGroupRouting();
    TestPortTypes();
    TestPortLayout();
    TestPortGestures();
    TestPortDrawAndRouting();
    TestRerouteAndWireColour();
    TestSchemaJson();
    TestRows();
    TestPropose();
    TestNavigation();
    TestLargeGraph();
    TestLayoutColumns();
    TestMemberGroups();
    TestAliasesAndHolding();
    TestLayoutGraph();
    TestLayoutDocks();
    TestFitBelowMinZoom();
    TestGoldenPicture();
    TestKitJson();
    TestKitResolve();
    TestKitCache();
    TestKitPinsAndRows();
    TestKitActions();
    TestKitEditsAndReorder();
    TestKitEdges();
    TestKitGroups();
    TestKitPillsAndDial();
    TestKitPrefixAndPalette();
    TestLargeGraphKit();
    TestFixedGroupAndHover();
    TestFrameGroupsUnchanged();
    TestDocks();
    TestShowWithAndWireStyles();
    TestDial();
    TestOctagonDial();
    TestKitSchema();
    TestMaterialExample();
    TestRimLabelWidth();
    TestHighlights();
    TestMinimapAndSearch();
    TestLiftWires();
    TestHostChangesMidDrag();
    TestMaterialGaps();
    TestRowRangeVetoAndWholeWires();
    if (g_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failed);
        return EXIT_FAILURE;
    }
    std::puts("grapher-test: all checks passed");
    return EXIT_SUCCESS;
}
