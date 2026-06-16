#pragma once

#include <functional>
#include <string>
#include <vector>

#include "message.hpp"

namespace lc {

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
    // arrives and returns the full concatenated assistant text. When think is
    // false, the model's "thinking" is disabled (so content is reliably
    // populated); when true, thinking deltas are streamed too. Throws on
    // transport/HTTP errors.
    std::string chat(const std::string& model,
                     const std::vector<Message>& messages,
                     const std::function<void(const std::string&)>& on_token,
                     bool think = false);

private:
    std::string host_;
    void* curl_ = nullptr;  // CURL*
};

}  // namespace lc
