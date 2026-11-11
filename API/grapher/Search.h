#pragma once
/// @file
/// @brief Finding nodes by their text: titles, ids, type names, rows, on-card fields and bodies, ranked so that the
///        likeliest node comes first (a title before a field, a match at the start before one inside, a whole match
///        first of all). Case doesn't matter for ASCII letters; other text matches byte for byte.

#include <grapher/Graph.h>
#include <grapher/Schema.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace grapher {

struct SearchHit {
    std::string node;    ///< the node's id
    std::string where;   ///< what matched: "title", "id", "type", a row's or a field's label, "body"
    std::string text;    ///< the matching text
    int         score = 0;
};

struct SearchResult {
    std::vector<SearchHit> hits;    ///< the best, best first, at most `limit`
    std::size_t            total = 0;   ///< every node that matched
};

/// @return the nodes matching `query`, one hit per node (its best match); nothing for an empty query
[[nodiscard]] SearchResult Search(const Graph& graph, const Schema& schema, std::string_view query, std::size_t limit = 200);

} // namespace grapher
