#include <grapher/Layout.h>

#include <grapher/Groups.h>

#include <algorithm>
#include <deque>
#include <limits>
#include <unordered_map>

namespace grapher {

namespace {

/// @brief Crossings between two adjacent columns' wires, counted as inversions with a Fenwick tree: O(E log E).
/// @param wires (slot in the left column, slot in the right one)
std::size_t Crossings(std::vector<std::pair<int, int>>& wires, int right)
{
    std::ranges::sort(wires);
    std::vector<int> tree(static_cast<std::size_t>(right) + 1, 0);
    std::size_t      crossed = 0, seen = 0;
    for (const auto& [a, b] : wires) {
        int below = 0;   // wires so far ending at or above b
        for (int i = b + 1; i > 0; i -= i & -i) below += tree[static_cast<std::size_t>(i)];
        crossed += seen - static_cast<std::size_t>(below);
        for (int i = b + 1; i <= right; i += i & -i) ++tree[static_cast<std::size_t>(i)];
        ++seen;
    }
    return crossed;
}

/// @brief The tops nearest `wanted` (least squares) that keep a column's order and its gaps: pool-adjacent-violators over
///        the tops with each item's offset in a packed column taken out, which turns the gap rule into "never smaller
///        than the one above".
void Fit(const std::vector<int>& column, const std::vector<double>& wanted, const std::vector<Vec2>& sizes, double gap, std::vector<double>& top)
{
    struct Block {
        double sum;
        int    count;
        [[nodiscard]] double Mean() const { return sum / count; }
    };
    std::vector<double> offset(column.size());
    double              packed = 0;
    for (std::size_t k = 0; k < column.size(); ++k) {
        offset[k] = packed;
        packed += sizes[static_cast<std::size_t>(column[k])].y + gap;
    }
    std::vector<Block> blocks;
    for (std::size_t k = 0; k < column.size(); ++k) {
        blocks.push_back({wanted[k] - offset[k], 1});
        while (blocks.size() > 1 && blocks[blocks.size() - 2].Mean() > blocks.back().Mean()) {
            blocks[blocks.size() - 2].sum += blocks.back().sum;
            blocks[blocks.size() - 2].count += blocks.back().count;
            blocks.pop_back();
        }
    }
    std::size_t k = 0;
    for (const Block& b : blocks)
        for (int i = 0; i < b.count; ++i, ++k) top[static_cast<std::size_t>(column[k])] = b.Mean() + offset[k];
}

} // namespace

std::vector<Vec2> LayoutColumns(const std::vector<Vec2>& sizes, const std::vector<std::pair<int, int>>& edges, LayoutGaps gaps)
{
    const int n = static_cast<int>(sizes.size());
    std::vector<std::vector<int>>    out(static_cast<std::size_t>(n));
    std::vector<int>                 incoming(static_cast<std::size_t>(n), 0);
    std::vector<std::pair<int, int>> wires;   ///< every usable edge
    for (const auto& [from, to] : edges) {
        if (from == to || from < 0 || to < 0 || from >= n || to >= n) continue;
        out[static_cast<std::size_t>(from)].push_back(to);
        ++incoming[static_cast<std::size_t>(to)];
        wires.emplace_back(from, to);
    }
    auto at = [](auto& v, int i) -> auto& { return v[static_cast<std::size_t>(i)]; };

    // The first column is whatever nothing enters; breadth first from there. What that never reaches (a cycle) follows.
    std::vector<int> column(static_cast<std::size_t>(n), -1);
    std::deque<int>  queue;
    for (int i = 0; i < n; ++i)
        if (at(incoming, i) == 0) {
            at(column, i) = 0;
            queue.push_back(i);
        }
    const auto walk = [&] {
        while (!queue.empty()) {
            const int u = queue.front();
            queue.pop_front();
            for (const int v : at(out, u))
                if (at(column, v) < 0) {
                    at(column, v) = at(column, u) + 1;
                    queue.push_back(v);
                }
        }
    };
    walk();
    int deepest = 0;
    for (const int c : column) deepest = std::max(deepest, c);
    for (int i = 0; i < n; ++i)
        if (at(column, i) < 0) {
            at(column, i) = deepest + 1;
            queue.push_back(i);
            walk();
            for (const int c : column) deepest = std::max(deepest, c);
        }

    // Neighbours in other columns, whichever way the wire runs: a wire back into an earlier column still pulls its ends.
    std::vector<std::vector<int>> near(static_cast<std::size_t>(n));
    for (const auto& [from, to] : wires)
        if (at(column, from) != at(column, to)) {
            at(near, from).push_back(to);
            at(near, to).push_back(from);
        }

    // Order each column to cross as few wires as it can: barycentre sweeps right and back left, keeping the best order
    // seen. What has no wire at all goes last in its column, in the order given.
    std::vector<std::vector<int>> columns(static_cast<std::size_t>(deepest) + 1);
    for (int i = 0; i < n; ++i)
        if (!at(near, i).empty()) at(columns, at(column, i)).push_back(i);
    for (int i = 0; i < n; ++i)
        if (at(near, i).empty()) at(columns, at(column, i)).push_back(i);
    std::vector<double> rank(static_cast<std::size_t>(n));   ///< where an item sits in its column, 0..1
    const auto rerank = [&](std::size_t c) {
        const double size = static_cast<double>(columns[c].size());
        for (std::size_t k = 0; k < columns[c].size(); ++k) at(rank, columns[c][k]) = (static_cast<double>(k) + 0.5) / size;
    };
    for (std::size_t c = 0; c < columns.size(); ++c) rerank(c);
    const auto sweep = [&](std::size_t c, bool fromLeft) {
        std::vector<std::pair<double, int>> keyed;
        keyed.reserve(columns[c].size());
        for (const int i : columns[c]) {
            double sum   = 0;
            int    count = 0;
            for (const int v : at(near, i))
                if (fromLeft ? at(column, v) < at(column, i) : at(column, v) > at(column, i)) {
                    sum += at(rank, v);
                    ++count;
                }
            keyed.emplace_back(count ? sum / count : at(rank, i), i);
        }
        std::ranges::stable_sort(keyed, {}, &std::pair<double, int>::first);
        for (std::size_t k = 0; k < keyed.size(); ++k) columns[c][k] = keyed[k].second;
        rerank(c);
    };
    const auto crossings = [&] {
        std::vector<int> slot(static_cast<std::size_t>(n));
        for (const auto& col : columns)
            for (std::size_t k = 0; k < col.size(); ++k) at(slot, col[k]) = static_cast<int>(k);
        std::vector<std::vector<std::pair<int, int>>> between(columns.size());
        for (const auto& [from, to] : wires) {
            const int a = at(column, from), b = at(column, to);
            if (b == a + 1) between[static_cast<std::size_t>(a)].emplace_back(at(slot, from), at(slot, to));
            else if (a == b + 1) between[static_cast<std::size_t>(b)].emplace_back(at(slot, to), at(slot, from));
        }
        std::size_t total = 0;
        for (std::size_t c = 0; c + 1 < columns.size(); ++c) total += Crossings(between[c], static_cast<int>(columns[c + 1].size()));
        return total;
    };
    std::size_t best      = crossings();
    auto        bestOrder = columns;
    for (int round = 0; round < 4 && best > 0; ++round) {
        for (std::size_t c = 1; c < columns.size(); ++c) sweep(c, true);
        for (std::size_t c = columns.size(); c-- > 0;) sweep(c, false);
        if (const std::size_t now = crossings(); now < best) {
            best      = now;
            bestOrder = columns;
        }
    }
    columns = std::move(bestOrder);

    // Heights: packed in order, then each column drawn toward the middle of what it wires to, rightward then leftward,
    // so a parent sits level with its children as well as a child with its parents. Fit keeps the order and the gaps.
    std::vector<double> top(static_cast<std::size_t>(n), 0);
    for (const auto& col : columns) {
        double y = 0;
        for (const int i : col) {
            at(top, i) = y;
            y += at(sizes, i).y + gaps.row;
        }
    }
    const auto settle = [&](std::size_t c, int side) {   // side: -1 what's left of it, +1 right, 0 both
        if (columns[c].empty()) return;
        std::vector<double> wanted;
        wanted.reserve(columns[c].size());
        for (const int i : columns[c]) {
            double sum   = 0;
            int    count = 0;
            for (const int v : at(near, i))
                if (side == 0 || (side < 0 ? at(column, v) < at(column, i) : at(column, v) > at(column, i))) {
                    sum += at(top, v) + at(sizes, v).y / 2;
                    ++count;
                }
            wanted.push_back(count ? sum / count - at(sizes, i).y / 2 : at(top, i));
        }
        Fit(columns[c], wanted, sizes, gaps.row, top);
    };
    for (int round = 0; round < 3; ++round) {
        for (std::size_t c = 1; c < columns.size(); ++c) settle(c, -1);
        for (std::size_t c = columns.size(); c-- > 0;) settle(c, +1);
    }
    for (std::size_t c = 0; c < columns.size(); ++c) settle(c, 0);

    std::vector<Vec2> placed(static_cast<std::size_t>(n));
    double            x = 0;
    for (const auto& col : columns) {
        double width = 0;
        for (const int i : col) {
            width        = std::max(width, static_cast<double>(at(sizes, i).x));
            at(placed, i) = {static_cast<float>(x), static_cast<float>(at(top, i))};
        }
        if (!col.empty()) x += width + gaps.column;
    }
    // The content starts at zero, whichever way its wires pulled it.
    float lowest = std::numeric_limits<float>::max();
    for (const Vec2& p : placed) lowest = std::min(lowest, p.y);
    if (n > 0)
        for (Vec2& p : placed) p.y -= lowest;
    return placed;
}

namespace {

/// @brief LayoutGraph's working state: the member-group tree as indices, and each item's place relative to its parent.
class GraphLayout {
public:
    GraphLayout(const Graph& graph, const Schema& schema, const std::set<std::string, std::less<>>& keep,
                const std::set<std::string, std::less<>>& collapsed, const LayoutOptions& o)
        : g_(graph), o_(o), n_(graph.nodes.size()), parent_(n_, kTop), children_(n_ + 1), kept_(n_, 0), open_(n_, 0),
          group_(n_, 0), size_(n_), rel_(n_)
    {
        std::unordered_map<std::string_view, std::size_t> index;
        for (std::size_t i = 0; i < n_; ++i) index.emplace(graph.nodes[i].id, i);
        for (std::size_t i = 0; i < n_; ++i) {
            const Node&     node = graph.nodes[i];
            const NodeType* t    = schema.FindNode(node.type);
            group_[i] = t && t->shape == NodeShape::Group && t->groupMode == GroupMode::Members;
            kept_[i]  = keep.contains(node.id);
            open_[i]  = group_[i] && !collapsed.contains(node.id);
        }
        for (std::size_t i = 0; i < n_; ++i) {
            const auto p = graph.nodes[i].parent.empty() ? index.end() : index.find(graph.nodes[i].parent);
            if (p != index.end() && group_[p->second] && p->second != i && !Above(i, p->second, index)) parent_[i] = p->second;
        }
        for (std::size_t i = 0; i < n_; ++i) children_[Slot(parent_[i])].push_back(i);

        // Each edge counts between the two children of the lowest group holding both ends.
        for (const Edge& e : graph.edges) {
            const auto a = index.find(e.from), b = index.find(e.to);
            if (a == index.end() || b == index.end() || a->second == b->second) continue;
            std::vector<std::size_t> chainA = Chain(a->second), chainB = Chain(b->second);
            // Walk down from the top while the chains agree; the first difference names the two children.
            std::size_t ka = chainA.size(), kb = chainB.size();
            while (ka > 0 && kb > 0 && chainA[ka - 1] == chainB[kb - 1]) --ka, --kb;
            if (ka == 0 || kb == 0) continue;   // one end holds the other
            const std::size_t ca = chainA[ka - 1], cb = chainB[kb - 1];
            wires_[Slot(parent_[ca])].emplace_back(ca, cb);
        }
    }

    std::vector<std::pair<std::string, Rect>> Run(Vec2 origin)
    {
        origin_ = origin;
        Measure(kTop);
        std::vector<std::pair<std::string, Rect>> out;
        out.reserve(n_);
        Place(kTop, origin, out);
        return out;
    }

private:
    static constexpr std::size_t kTop = static_cast<std::size_t>(-1);

    [[nodiscard]] std::size_t Slot(std::size_t parent) const { return parent == kTop ? n_ : parent; }

    /// @brief Whether `node` is (an ancestor of) `group` by the parents set so far: a parent cycle is cut there.
    bool Above(std::size_t node, std::size_t group, const std::unordered_map<std::string_view, std::size_t>& index) const
    {
        std::size_t at = group;
        for (std::size_t steps = 0; steps <= n_; ++steps) {
            if (at == node) return true;
            const auto p = g_.nodes[at].parent.empty() ? index.end() : index.find(g_.nodes[at].parent);
            if (p == index.end() || !group_[p->second]) return false;
            at = p->second;
        }
        return true;
    }

    /// @return the node and its ancestors, nearest first
    [[nodiscard]] std::vector<std::size_t> Chain(std::size_t i) const
    {
        std::vector<std::size_t> c;
        for (std::size_t at = i; at != kTop; at = parent_[at]) c.push_back(at);
        return c;
    }

    /// @brief Each item's size and its place relative to its parent's content corner, deepest first.
    Vec2 Measure(std::size_t g)
    {
        const std::vector<std::size_t>& kids = children_[Slot(g)];
        for (const std::size_t c : kids) {
            if (!group_[c]) {
                size_[c] = {g_.nodes[c].frame.w, g_.nodes[c].frame.h};
                continue;
            }
            const Vec2 content = Measure(c);
            size_[c] = {std::max(content.x + 2 * o_.padding, 120.f), open_[c] ? content.y + o_.header + o_.padding : o_.header};
        }
        if (kids.empty()) return {};

        // Columns for what the layout places; a kept item stays where it is, relative to its parent's content corner.
        std::unordered_map<std::size_t, int> local;
        std::vector<Vec2>                   sizes;
        for (const std::size_t c : kids) {
            local.emplace(c, static_cast<int>(sizes.size()));
            sizes.push_back(size_[c]);
        }
        std::vector<std::pair<int, int>> edges;
        for (const auto& [a, b] : wires_[Slot(g)]) edges.emplace_back(local.at(a), local.at(b));
        const std::vector<Vec2> placed = LayoutColumns(sizes, edges, o_.gaps);
        const Vec2 corner = g == kTop ? origin_ : Vec2{g_.nodes[g].frame.x + o_.padding, g_.nodes[g].frame.y + o_.header};
        std::vector<Rect> keptRects;
        for (const std::size_t c : kids)
            if (kept_[c]) {
                rel_[c] = Vec2{g_.nodes[c].frame.x, g_.nodes[c].frame.y} - corner;
                keptRects.push_back({rel_[c].x, rel_[c].y, size_[c].x, size_[c].y});
            }
        for (const std::size_t c : kids) {
            if (kept_[c]) continue;
            rel_[c] = placed[static_cast<std::size_t>(local.at(c))];
            // Slide down past every kept item it would overlap.
            for (bool moved = true; moved;) {
                moved = false;
                for (const Rect& k : keptRects)
                    if (Rect{rel_[c].x, rel_[c].y, size_[c].x, size_[c].y}.Inflated(o_.gaps.row * 0.5f).Intersects(k)) {
                        rel_[c].y = k.Bottom() + o_.gaps.row;
                        moved     = true;
                    }
            }
        }
        Push(kids);

        // The content starts at its own corner (the top level keeps its kept items where they are).
        Vec2 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()}, hi{-lo.x, -lo.y};
        for (const std::size_t c : kids) {
            lo = {std::min(lo.x, rel_[c].x), std::min(lo.y, rel_[c].y)};
            hi = {std::max(hi.x, rel_[c].x + size_[c].x), std::max(hi.y, rel_[c].y + size_[c].y)};
        }
        if (g != kTop)
            for (const std::size_t c : kids) rel_[c] = rel_[c] - lo;
        return hi - lo;
    }

    /// @brief An open group pushes the siblings it overlaps: right when they start right of its reach, else down, until
    ///        none overlap (a bounded number of passes, so a crowd can't loop).
    void Push(const std::vector<std::size_t>& kids)
    {
        for (std::size_t pass = 0; pass < kids.size() + 1; ++pass) {
            bool moved = false;
            for (const std::size_t g : kids) {
                if (!open_[g]) continue;
                const Rect gr{rel_[g].x, rel_[g].y, size_[g].x, size_[g].y};
                for (const std::size_t c : kids) {
                    if (c == g) continue;
                    const Rect cr{rel_[c].x, rel_[c].y, size_[c].x, size_[c].y};
                    if (!cr.Intersects(gr) || cr.x >= gr.Right() || cr.y >= gr.Bottom()) continue;
                    if (cr.x >= gr.x + o_.pushReach) rel_[c].x = gr.Right() + o_.gaps.column;
                    else rel_[c].y = gr.Bottom() + o_.gaps.row;
                    moved = true;
                }
            }
            if (!moved) return;
        }
    }

    void Place(std::size_t g, Vec2 corner, std::vector<std::pair<std::string, Rect>>& out) const
    {
        for (const std::size_t c : children_[Slot(g)]) {
            const Vec2 at = corner + rel_[c];
            out.emplace_back(g_.nodes[c].id, Rect{at.x, at.y, size_[c].x, size_[c].y});
            if (group_[c]) Place(c, at + Vec2{o_.padding, o_.header}, out);
        }
    }

    const Graph&                                     g_;
    const LayoutOptions&                             o_;
    std::size_t                                      n_;
    std::vector<std::size_t>                         parent_;     ///< kTop at the top level
    std::vector<std::vector<std::size_t>>            children_;   ///< by Slot(parent)
    std::vector<char>                                kept_, open_, group_;
    std::vector<Vec2>                                size_, rel_;
    Vec2                                             origin_;
    std::unordered_map<std::size_t, std::vector<std::pair<std::size_t, std::size_t>>> wires_;   ///< by Slot(group)
};

} // namespace

std::vector<std::pair<std::string, Rect>> LayoutGraph(const Graph& graph, const Schema& schema, const std::set<std::string, std::less<>>& keep,
                                                      const std::set<std::string, std::less<>>& collapsed, const LayoutOptions& options,
                                                      Vec2 origin)
{
    return GraphLayout(graph, schema, keep, collapsed, options).Run(origin);
}

} // namespace grapher
