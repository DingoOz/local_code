#include "conversation.hpp"

#include <algorithm>

#include "system_prompt.hpp"

namespace lc {

namespace {
// Per-message framing overhead in the heuristic (role tag, JSON braces).
constexpr int kPerMsgOverhead = 4;
// Tokens reserved for the rolling summary so window() has a stable target.
constexpr int kSummaryReserve = 256;
// Never evict below this many recent turns, even if oversized.
constexpr size_t kKeepMin = 2;
}  // namespace

Conversation::Conversation(std::string system_prompt, int budget_tokens)
    : budget_(budget_tokens),
      system_{Role::System, std::move(system_prompt)} {}

int Conversation::estimate_tokens(const std::string& s) {
    return static_cast<int>(s.size() / 4) + 1;
}

void Conversation::add(Role role, std::string content) {
    turns_.push_back({role, std::move(content)});
}

void Conversation::add(Message msg) {
    turns_.push_back(std::move(msg));
}

void Conversation::reset() {
    turns_.clear();
    summary_.clear();
}

size_t Conversation::compact() {
    if (turns_.empty()) return 0;
    const size_t n = turns_.size();
    if (summarize_) {
        std::vector<Message> to_sum;
        if (!summary_.empty())
            to_sum.push_back({Role::System, "Summary so far:\n" + summary_});
        to_sum.insert(to_sum.end(), turns_.begin(), turns_.end());
        std::string s = summarize_(to_sum);
        if (!s.empty()) summary_ = std::move(s);
    }
    // Drop the turns regardless: even without a summarizer this bounds context.
    turns_.clear();
    return n;
}

int Conversation::budget_for_turns() const {
    int b = budget_ - estimate_tokens(system_.content);
    if (!summary_.empty()) b -= kSummaryReserve;
    return std::max(b, 64);
}

std::vector<Message> Conversation::window() {
    // Decide how many oldest turns must be evicted: walk newest -> oldest,
    // keeping turns until the budget would be exceeded. Everything older is the
    // eviction set (but always keep at least kKeepMin recent turns).
    const int turn_budget = budget_for_turns();
    int running = 0;
    size_t keep_from = turns_.size();  // index of first KEPT turn
    for (size_t i = turns_.size(); i-- > 0;) {
        const int cost = estimate_tokens(turns_[i].content) + kPerMsgOverhead;
        if (running + cost > turn_budget && (turns_.size() - i) > kKeepMin) {
            keep_from = i + 1;
            break;
        }
        running += cost;
        keep_from = i;
    }

    // Compact evicted turns into the rolling summary (permanent), if any.
    if (keep_from > 0) {
        std::vector<Message> evicted(turns_.begin(),
                                     turns_.begin() + keep_from);
        if (summarize_) {
            // Seed the summarizer with the prior summary so context accumulates.
            std::vector<Message> to_sum;
            if (!summary_.empty())
                to_sum.push_back({Role::System,
                                  "Summary so far:\n" + summary_});
            to_sum.insert(to_sum.end(), evicted.begin(), evicted.end());
            std::string s = summarize_(to_sum);
            if (!s.empty()) summary_ = std::move(s);
        }
        // If no summarizer (or it returned empty), evicted turns are simply
        // dropped — still bounds the context.
        turns_.erase(turns_.begin(), turns_.begin() + keep_from);
    }

    // Assemble the outgoing window.
    std::vector<Message> out;
    out.push_back(system_);
    if (!summary_.empty()) {
        out.push_back({Role::System,
                       "Earlier conversation summary:\n" + summary_});
    }
    out.insert(out.end(), turns_.begin(), turns_.end());
    return out;
}

}  // namespace lc
