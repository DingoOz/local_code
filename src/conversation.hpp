#pragma once

#include <functional>
#include <string>
#include <vector>

#include "message.hpp"

namespace lc {

// Owns the full transcript plus a rolling summary of evicted older turns, and
// produces a token-bounded message window for each model call. This is the
// component that keeps a weak model's context small and stable.
class Conversation {
public:
    // summarizer(messages) -> short summary text. Invoked when old turns must be
    // compacted. May be empty, in which case eviction falls back to dropping.
    using Summarizer = std::function<std::string(const std::vector<Message>&)>;

    Conversation(std::string system_prompt, int budget_tokens);

    void set_summarizer(Summarizer fn) { summarize_ = std::move(fn); }

    void add(Role role, std::string content);
    void reset();  // clears transcript + summary, keeps system prompt

    // Swap the system prompt (e.g. when toggling planning/build mode). History
    // is preserved; the new prompt applies to the next window().
    void set_system_prompt(std::string prompt) { system_.content = std::move(prompt); }

    // Build the messages to send: system prompt, then rolling summary (if any),
    // then the most recent turns that fit the budget. Older turns that don't fit
    // are folded into the rolling summary (mutates internal summary state).
    std::vector<Message> window();

    // Heuristic token estimate (~bytes/4); no tokenizer endpoint in Ollama.
    static int estimate_tokens(const std::string& s);

    const std::string& summary() const { return summary_; }
    size_t turn_count() const { return turns_.size(); }

private:
    int budget_;
    Message system_;
    std::string summary_;            // rolling summary of evicted turns
    std::vector<Message> turns_;     // user/assistant/tool, in order
    Summarizer summarize_;

    int budget_for_turns() const;    // budget minus system + summary cost
};

}  // namespace lc
