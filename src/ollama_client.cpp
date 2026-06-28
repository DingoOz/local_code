#include "ollama_client.hpp"

#include <curl/curl.h>

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace lc {

namespace {

// Accumulates a simple response body (for non-streaming GET).
size_t write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// Streaming state for /api/chat: Ollama emits newline-delimited JSON objects.
// We buffer partial bytes and process one complete line at a time.
struct StreamCtx {
    std::string buffer;                  // unparsed tail
    std::string content;                 // accumulated visible answer
    std::string reasoning;               // accumulated thinking/reasoning
    json tool_calls = json::array();     // accumulated native tool_calls entries
    const TokenCallback* on_token = nullptr;
    const std::function<bool(const std::string&)>* stop_when = nullptr;
    bool stopped = false;                // early-stop requested
};

void process_line(StreamCtx* ctx, const std::string& line) {
    if (line.empty()) return;
    json obj = json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (obj.is_discarded()) return;  // skip malformed/partial lines defensively
    if (obj.contains("error")) {
        throw std::runtime_error("Ollama error: " + obj["error"].get<std::string>());
    }
    if (!obj.contains("message")) return;
    const json& m = obj["message"];

    // Stream reasoning (thinking / reasoning_content) first, so it shows even
    // when a reasoning model leaves "content" empty until the very end.
    for (const char* field : {"thinking", "reasoning_content"}) {
        if (!m.contains(field) || !m[field].is_string()) continue;
        const std::string piece = m[field].get<std::string>();
        if (piece.empty()) continue;
        ctx->reasoning += piece;
        if (ctx->on_token && *ctx->on_token) (*ctx->on_token)(piece, /*reasoning=*/true);
    }
    if (m.contains("content") && m["content"].is_string()) {
        const std::string piece = m["content"].get<std::string>();
        if (!piece.empty()) {
            ctx->content += piece;
            if (ctx->on_token && *ctx->on_token) (*ctx->on_token)(piece, false);
        }
    }
    // Native function calls. Ollama usually sends these complete in one chunk,
    // but accumulate defensively in case they are split across messages.
    if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
        for (const auto& tc : m["tool_calls"]) ctx->tool_calls.push_back(tc);
    }
}

size_t write_stream(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    const size_t n = size * nmemb;
    ctx->buffer.append(ptr, n);

    size_t pos;
    while ((pos = ctx->buffer.find('\n')) != std::string::npos) {
        std::string line = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 1);
        process_line(ctx, line);
    }
    // Stop early once the caller is satisfied (e.g. a complete tool call).
    // Returning a short count makes libcurl abort with CURLE_WRITE_ERROR.
    if (ctx->stop_when && *ctx->stop_when && !ctx->stopped &&
        (*ctx->stop_when)(ctx->content)) {
        ctx->stopped = true;
        return 0;
    }
    return n;
}

// Streaming state for /api/pull: newline-delimited JSON progress objects.
struct PullCtx {
    std::string buffer;
    std::string error;
    const std::function<void(const std::string&, long long, long long)>* cb;
};

void pull_line(PullCtx* ctx, const std::string& line) {
    if (line.empty()) return;
    json obj = json::parse(line, nullptr, false);
    if (obj.is_discarded()) return;
    if (obj.contains("error")) {
        ctx->error = obj["error"].get<std::string>();
        return;
    }
    const std::string status =
        obj.contains("status") && obj["status"].is_string()
            ? obj["status"].get<std::string>()
            : std::string();
    auto num = [&](const char* k) -> long long {
        return obj.contains(k) && obj[k].is_number()
                   ? obj[k].get<long long>()
                   : -1;
    };
    if (ctx->cb && *ctx->cb) (*ctx->cb)(status, num("completed"), num("total"));
}

size_t write_pull(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<PullCtx*>(userdata);
    const size_t n = size * nmemb;
    ctx->buffer.append(ptr, n);
    size_t pos;
    while ((pos = ctx->buffer.find('\n')) != std::string::npos) {
        std::string line = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 1);
        pull_line(ctx, line);
    }
    return n;
}

}  // namespace

OllamaClient::OllamaClient(std::string host) : host_(std::move(host)) {
    if (!host_.empty() && host_.back() == '/') host_.pop_back();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_ = curl_easy_init();
    if (!curl_) throw std::runtime_error("Failed to init libcurl");
}

OllamaClient::~OllamaClient() {
    if (curl_) curl_easy_cleanup(static_cast<CURL*>(curl_));
    curl_global_cleanup();
}

std::vector<std::string> OllamaClient::list_models() {
    CURL* curl = static_cast<CURL*>(curl_);
    curl_easy_reset(curl);
    std::string body;
    const std::string url = host_ + "/api/tags";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("Cannot reach Ollama at ") + url +
                                 ": " + curl_easy_strerror(rc));
    }

    json obj = json::parse(body, nullptr, false);
    if (obj.is_discarded() || !obj.contains("models")) {
        throw std::runtime_error("Unexpected /api/tags response");
    }
    std::vector<std::string> names;
    for (const auto& m : obj["models"]) {
        if (m.contains("name")) names.push_back(m["name"].get<std::string>());
    }
    return names;
}

void OllamaClient::pull_model(
    const std::string& name,
    const std::function<void(const std::string&, long long, long long)>&
        on_progress) {
    CURL* curl = static_cast<CURL*>(curl_);
    curl_easy_reset(curl);

    json payload;
    payload["name"] = name;
    payload["stream"] = true;
    const std::string body = payload.dump();

    PullCtx ctx;
    ctx.cb = &on_progress;

    const std::string url = host_ + "/api/pull";
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_pull);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    // No total timeout: a model download can take a long time.

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("Pull request failed: ") +
                                 curl_easy_strerror(rc));
    }
    if (!ctx.buffer.empty()) pull_line(&ctx, ctx.buffer);
    if (!ctx.error.empty())
        throw std::runtime_error("Pull failed: " + ctx.error);
    if (http_code >= 400) {
        throw std::runtime_error("Pull failed: HTTP " +
                                 std::to_string(http_code) + " from " + url);
    }
}

ChatResult OllamaClient::chat(const std::string& model,
                              const std::vector<Message>& messages,
                              const TokenCallback& on_token,
                              const ChatOptions& opts) {
    CURL* curl = static_cast<CURL*>(curl_);
    curl_easy_reset(curl);

    json payload;
    payload["model"] = model;
    payload["stream"] = true;
    payload["think"] = opts.think;
    json msgs = json::array();
    for (const auto& m : messages) {
        json jm = {{"role", role_to_api(m.role)}, {"content", m.content}};
        // Round-trip native tool calls so the model sees its own calls + the
        // tool result it produced (Ollama accepts tool_calls on assistant
        // messages and tool_name on tool-role results).
        if (!m.tool_calls_json.empty()) {
            json tc = json::parse(m.tool_calls_json, nullptr, false);
            if (!tc.is_discarded()) jm["tool_calls"] = std::move(tc);
        }
        if (!m.tool_name.empty()) jm["tool_name"] = m.tool_name;
        msgs.push_back(std::move(jm));
    }
    payload["messages"] = std::move(msgs);

    // Advertise the tool schema (enables native function calling).
    if (!opts.tools_json.empty()) {
        json t = json::parse(opts.tools_json, nullptr, false);
        if (!t.is_discarded()) payload["tools"] = std::move(t);
    }
    // Sampling + context window; omit unset options so server defaults apply.
    json options = json::object();
    if (opts.temperature >= 0) options["temperature"] = opts.temperature;
    if (opts.top_p >= 0) options["top_p"] = opts.top_p;
    if (opts.top_k >= 0) options["top_k"] = opts.top_k;
    if (opts.num_ctx > 0) options["num_ctx"] = opts.num_ctx;
    if (!options.empty()) payload["options"] = std::move(options);

    const std::string body = payload.dump();

    StreamCtx ctx;
    ctx.on_token = &on_token;
    ctx.stop_when = &opts.stop_when;

    const std::string url = host_ + "/api/chat";
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_stream);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    // No total timeout: local generation can be slow on large models.

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    // A deliberate early stop aborts the transfer with CURLE_WRITE_ERROR; that
    // is success, not failure.
    if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && ctx.stopped)) {
        throw std::runtime_error(std::string("Chat request failed: ") +
                                 curl_easy_strerror(rc));
    }
    // Flush any trailing line without a newline (not after an early stop).
    if (!ctx.stopped && !ctx.buffer.empty()) process_line(&ctx, ctx.buffer);

    ChatResult result;
    result.content = std::move(ctx.content);
    result.reasoning = std::move(ctx.reasoning);
    if (!ctx.tool_calls.empty()) {
        result.raw_tool_calls_json = ctx.tool_calls.dump();
        for (const auto& tc : ctx.tool_calls) {
            if (!tc.contains("function") || !tc["function"].is_object()) continue;
            const json& fn = tc["function"];
            NativeToolCall ntc;
            if (tc.contains("id") && tc["id"].is_string())
                ntc.id = tc["id"].get<std::string>();
            if (fn.contains("name") && fn["name"].is_string())
                ntc.name = fn["name"].get<std::string>();
            // arguments is normally a JSON object; some servers send a string.
            if (fn.contains("arguments")) {
                ntc.arguments_json = fn["arguments"].is_string()
                                         ? fn["arguments"].get<std::string>()
                                         : fn["arguments"].dump();
            }
            if (!ntc.name.empty()) result.tool_calls.push_back(std::move(ntc));
        }
    }
    return result;
}

}  // namespace lc
