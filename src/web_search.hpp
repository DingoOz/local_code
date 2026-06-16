#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lc {

struct WebResult {
    std::string title;
    std::string url;
    std::string content;  // snippet
};

// Parse a SearXNG JSON response body into up to max_results entries.
std::vector<WebResult> parse_searxng_json(const std::string& body,
                                          int max_results);

// Query a SearXNG instance's JSON API (base_url + /search?q=...&format=json).
// Returns nullopt on transport/HTTP/parse failure.
std::optional<std::vector<WebResult>> web_search(const std::string& base_url,
                                                 const std::string& query,
                                                 int max_results = 5);

// True if the SearXNG JSON API at base_url is reachable and usable.
bool web_search_available(const std::string& base_url);

}  // namespace lc
