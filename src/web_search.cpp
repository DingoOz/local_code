#include "web_search.hpp"

#include <curl/curl.h>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace lc {

namespace {
size_t write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}
}  // namespace

std::vector<WebResult> parse_searxng_json(const std::string& body,
                                          int max_results) {
    std::vector<WebResult> out;
    json o = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (o.is_discarded() || !o.contains("results") || !o["results"].is_array())
        return out;
    for (const auto& r : o["results"]) {
        if ((int)out.size() >= max_results) break;
        WebResult w;
        if (r.contains("title") && r["title"].is_string())
            w.title = r["title"].get<std::string>();
        if (r.contains("url") && r["url"].is_string())
            w.url = r["url"].get<std::string>();
        if (r.contains("content") && r["content"].is_string())
            w.content = r["content"].get<std::string>();
        if (!w.url.empty()) out.push_back(std::move(w));
    }
    return out;
}

std::optional<std::vector<WebResult>> web_search(const std::string& base_url,
                                                 const std::string& query,
                                                 int max_results) {
    CURL* curl = curl_easy_init();
    if (!curl) return std::nullopt;

    std::string url = base_url;
    if (!url.empty() && url.back() == '/') url.pop_back();
    char* esc = curl_easy_escape(curl, query.c_str(), (int)query.size());
    url += "/search?q=";
    url += esc ? esc : "";
    url += "&format=json";
    if (esc) curl_free(esc);

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "local_code/1.0");

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || code >= 400) return std::nullopt;

    // Require an actual SearXNG JSON shape (a "results" array). This keeps the
    // availability probe from being fooled by an unrelated HTTP-200 service on
    // the same port (e.g. a Jupyter server).
    json o = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (o.is_discarded() || !o.contains("results") || !o["results"].is_array())
        return std::nullopt;
    return parse_searxng_json(body, max_results);
}

bool web_search_available(const std::string& base_url) {
    return web_search(base_url, "ping", 1).has_value();
}

}  // namespace lc
