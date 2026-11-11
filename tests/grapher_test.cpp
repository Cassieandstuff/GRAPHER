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

    // A closed group is one header-high item.
    laid = grapher::LayoutGraph(g, s, {}, {"F2"}, o);
    CHECK(frameOf(laid, "F2").h == o.header);

    // A kept node stays put; one placed by the layout slides clear of it; an open group pushes what it would cover.
    g.nodes[4].frame = {0, 0, 100, 60};
    laid = grapher::LayoutGraph(g, s, {"c"}, {}, o);
    const grapher::Rect kept = frameOf(laid, "c"), F1b = frameOf(laid, "F1");
    CHECK(kept == (grapher::Rect{0, 0, 100, 60}) || !kept.Intersects(F1b));
    CHECK(!kept.Inflated(-1.f).Intersects(F1b));
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
    TestFrameGroupsUnchanged();
    TestDocks();
    TestShowWithAndWireStyles();
    TestDial();
    TestHighlights();
    TestMinimapAndSearch();
    if (g_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failed);
        return EXIT_FAILURE;
    }
    std::puts("grapher-test: all checks passed");
    return EXIT_SUCCESS;
}
