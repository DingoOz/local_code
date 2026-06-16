// Deterministic check of the context-window guard: with a tight budget and many
// turns, window() must stay bounded and fold old turns into a rolling summary.
#include <cassert>
#include <iostream>
#include <string>

#include "../src/conversation.hpp"

using namespace lc;

int main() {
    const int budget = 300;  // tight
    Conversation convo("SYSTEM PROMPT (fixed).", budget);

    int summarize_calls = 0;
    convo.set_summarizer([&](const std::vector<Message>& msgs) -> std::string {
        ++summarize_calls;
        return "ROLLING SUMMARY of " + std::to_string(msgs.size()) + " msgs";
    });

    // Simulate a long conversation: 20 user/assistant exchanges, ~60 chars each.
    int max_window_tokens = 0;
    for (int i = 0; i < 20; ++i) {
        convo.add(Role::User,
                  "User message number " + std::to_string(i) +
                      " with some padding text here to add weight.");
        convo.add(Role::Assistant,
                  "Assistant reply number " + std::to_string(i) +
                      " also padded out to consume tokens in the window.");
        auto w = convo.window();
        int t = 0;
        for (const auto& m : w) t += Conversation::estimate_tokens(m.content);
        if (t > max_window_tokens) max_window_tokens = t;
    }

    // 1) The window never blows past the budget by more than a small margin.
    std::cout << "max window tokens = " << max_window_tokens
              << " (budget " << budget << ")\n";
    assert(max_window_tokens <= budget + 64);

    // 2) Summarization actually ran and a rolling summary exists.
    std::cout << "summarize calls = " << summarize_calls << "\n";
    std::cout << "summary present = " << (!convo.summary().empty()) << "\n";
    assert(summarize_calls > 0);
    assert(!convo.summary().empty());

    // 3) The system prompt is always first in the window.
    auto w = convo.window();
    assert(!w.empty() && w[0].role == Role::System &&
           w[0].content == "SYSTEM PROMPT (fixed).");

    std::cout << "PASS\n";
    return 0;
}
