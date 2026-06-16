// SearXNG JSON parsing: must extract title/url/content, honor the max, skip
// entries without a URL, and tolerate malformed input.
#include <cassert>
#include <iostream>

#include "../src/web_search.hpp"

using namespace lc;

int main() {
    const std::string sample =
        R"({"query":"q","results":[
        {"title":"First","url":"https://a.example","content":"snippet one"},
        {"title":"Second","url":"https://b.example","content":"snippet two"},
        {"title":"NoUrl","content":"dropped"}
        ]})";

    auto r = parse_searxng_json(sample, 5);
    assert(r.size() == 2);  // the URL-less entry is skipped
    assert(r[0].title == "First" && r[0].url == "https://a.example" &&
           r[0].content == "snippet one");
    assert(r[1].url == "https://b.example");

    // max_results is honored.
    assert(parse_searxng_json(sample, 1).size() == 1);

    // Malformed / missing results -> empty, no throw.
    assert(parse_searxng_json("not json", 5).empty());
    assert(parse_searxng_json(R"({"foo":1})", 5).empty());

    std::cout << "WEB TESTS PASS\n";
    return 0;
}
