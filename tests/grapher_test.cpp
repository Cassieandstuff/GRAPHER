// GRAPHER core tests: plain asserts, no framework (the TARGETER convention).
#include <grapher/Grapher.h>

#include <chrono>
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
    for (const grapher::Schema& each : {*s, PortSchema(), MakeSchema(), GroupSchema()}) {
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
    if (g_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failed);
        return EXIT_FAILURE;
    }
    std::puts("grapher-test: all checks passed");
    return EXIT_SUCCESS;
}
