#pragma once

#include <functional>
#include <string>
#include <vector>

#include "message.hpp"

namespace lc {

// A native function call parsed out of an Ollama /api/chat response
// (message.tool_calls). arguments_json is the function.arguments object dumped
// back to a string for uniform handling with the text-protocol path.
struct NativeToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;
};

// The outcome of one chat() call. content is the visible answer only; reasoning
// holds any thinking / reasoning_content (streamed live but never persisted to
// history); tool_calls is non-empty when the model made native function calls.
struct ChatResult {
    std::string content;
    std::string reasoning;
    std::vector<NativeToolCall> tool_calls;
    std::string raw_tool_calls_json;  // verbatim array, for history round-trip
};

// Per-call request options. Sentinel values (<0 for floats/ints, 0 for num_ctx,
// empty tools_json) mean "omit from the payload and use the server default".
struct ChatOptions {
    bool think = false;
    std::string tools_json;       // tools[] schema to advertise; "" => none
    double temperature = -1.0;
    double top_p = -1.0;
    int top_k = -1;
    int num_ctx = 0;
    // Called with the accumulated visible content after each chunk; returning
    // true ends the stream early (text-protocol fallback only — see Agent).
    std::function<bool(const std::string&)> stop_when;
};

// Streaming token callback: piece is the delta; reasoning=true marks thinking /
// reasoning_content deltas so the caller can style them differently.
using TokenCallback = std::function<void(const std::string& piece, bool reasoning)>;

// Thin libcurl wrapper around the Ollama HTTP API. One client owns one reusable
// CURL handle; not thread-safe (single REPL thread).
class OllamaClient {
public:
    explicit OllamaClient(std::string host);
    ~OllamaClient();

    OllamaClient(const OllamaClient&) = delete;
    OllamaClient& operator=(const OllamaClient&) = delete;

    // GET /api/tags -> model names. Throws std::runtime_error on failure.
    std::vector<std::string> list_models();

    // POST /api/pull (stream) to download a model. Reports progress via
    // on_progress(status, completed, total); completed/total are -1 when the
    // server hasn't reported byte counts yet. Throws on error.
    void pull_model(
        const std::string& name,
        const std::function<void(const std::string& status, long long completed,
                                 long long total)>& on_progress);

    // POST /api/chat with stream:true. Calls on_token for each delta as it
    // arrives and returns a ChatResult (visible content, reasoning, and any
    // native tool_calls). When opts.think is false the model's "thinking" is
    // disabled (so content is reliably populated); when true, thinking deltas
    // are streamed (reasoning=true) too. opts.tools_json, when set, advertises
    // the tools[] schema so the model can emit native function calls. Throws on
    // transport/HTTP errors.
    ChatResult chat(const std::string& model,
                    const std::vector<Message>& messages,
                    const TokenCallback& on_token, const ChatOptions& opts = {});

private:
    std::string host_;
    void* curl_ = nullptr;  // CURL*
};

}  // namespace lc
