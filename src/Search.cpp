#include <grapher/Search.h>

#include <algorithm>

namespace grapher {

namespace {

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string Folded(std::string_view s)
{
    std::string out(s);
    for (char& c : out) c = Lower(c);
    return out;
}

/// @return how well `text` holds `query` (already folded), weighted by what it is; 0 when it doesn't
int Score(std::string_view text, const std::string& query, int weight)
{
    if (text.empty()) return 0;
    const std::string folded = Folded(text);
    const auto        at     = folded.find(query);
    if (at == std::string::npos) return 0;
    int score = weight;
    if (at == 0) score += 50;              // a match at the start
    if (folded == query) score += 100;     // the whole of it
    return score;
}

} // namespace

SearchResult Search(const Graph& graph, const Schema& schema, std::string_view query, std::size_t limit)
{
    SearchResult result;
    const std::string q = Folded(query);
    if (q.empty()) return result;
    for (const Node& n : graph.nodes) {
        const NodeType* t = schema.FindNode(n.type);
        SearchHit       best;
        auto consider = [&](std::string_view text, std::string_view where, int weight) {
            if (const int s = Score(text, q, weight); s > best.score) best = {n.id, std::string(where), std::string(text), s};
        };
        if (t && !t->titleField.empty()) consider(n.Field(t->titleField), "title", 100);
        consider(n.id, "id", 80);
        if (t) consider(t->label, "type", 60);
        for (const Row& r : n.rows) {
            consider(r.label, r.label, 40);
            consider(r.value, r.label, 40);
        }
        if (t)
            for (const FieldDef& f : t->fields)
                if (f.id != t->titleField) consider(n.Field(f.id), f.label, 30);
        consider(n.body, "body", 10);
        if (best.score > 0) result.hits.push_back(std::move(best));
    }
    result.total = result.hits.size();
    std::ranges::stable_sort(result.hits, std::greater<>{}, &SearchHit::score);
    if (result.hits.size() > limit) result.hits.resize(limit);
    return result;
}

} // namespace grapher
