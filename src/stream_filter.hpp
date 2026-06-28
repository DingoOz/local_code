#pragma once

#include <array>
#include <string>
#include <string_view>

namespace lc {

// Number of trailing bytes of s[0,len) that form an INCOMPLETE UTF-8 sequence
// (0 if it ends on a character boundary). Streamed model output can split a
// multi-byte character across chunks; emitting the leading bytes alone corrupts
// terminals — ncurses especially — that decode each write independently
// (e.g. "—" -> "\xEF\xBF\xBD~@~T"). Hold the partial tail until it completes.
inline size_t utf8_incomplete_suffix_len(const std::string& s, size_t len) {
    size_t cont = 0;
    for (size_t i = len; i > 0 && cont < 4; --i) {
        unsigned char c = static_cast<unsigned char>(s[i - 1]);
        if ((c & 0xC0) == 0x80) { ++cont; continue; }  // continuation byte
        size_t need;
        if      ((c & 0x80) == 0x00) need = 1;         // ASCII
        else if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else return 0;                  // invalid lead byte: don't hold
        size_t have = cont + 1;         // lead + continuations seen so far
        return have < need ? have : 0;  // hold only an unfinished sequence
    }
    return 0;  // ran past the start / malformed run: don't hold
}

// Cleans the harmony-style control tokens some local models leak into their
// output. Two behaviours:
//   * "channel headers" — text from an open marker (e.g. "<|channel>") up to a
//     close marker (e.g. "<channel|>") is the channel *name* (thought/analysis)
//     and is dropped entirely.
//   * stray standalone markers are simply removed.
// Markers may be split across streamed chunks, so a possible partial-marker
// suffix is held back until the next feed()/flush().
class MarkerFilter {
public:
    std::string feed(std::string_view chunk) {
        pending_.append(chunk);
        std::string out;
        for (;;) {
            if (state_ == State::Normal) {
                // Earliest of: an open marker, or a strip-only marker.
                size_t best = std::string::npos, mlen = 0;
                bool is_open = false;
                find_first(kOpen, best, mlen, is_open, true);
                find_first(kStrip, best, mlen, is_open, false);
                if (best != std::string::npos) {
                    out.append(pending_, 0, best);
                    pending_.erase(0, best + mlen);
                    if (is_open) { state_ = State::Suppress; suppressed_ = 0; }
                    continue;
                }
                size_t hold = holdback(kOpen, holdback(kStrip, holdback(kClose, 0)));
                // Also hold back an incomplete trailing UTF-8 character so a
                // multi-byte glyph is never split across writes.
                hold += utf8_incomplete_suffix_len(pending_, pending_.size() - hold);
                out.append(pending_, 0, pending_.size() - hold);
                pending_.erase(0, pending_.size() - hold);
                break;
            } else {  // Suppress: drop until a close marker appears.
                size_t best = std::string::npos, mlen = 0;
                bool dummy = false;
                find_first(kClose, best, mlen, dummy, false);
                if (best != std::string::npos) {
                    suppressed_ += best;
                    pending_.erase(0, best + mlen);
                    state_ = State::Normal;
                    continue;
                }
                size_t hold = holdback(kClose, 0);
                suppressed_ += pending_.size() - hold;
                pending_.erase(0, pending_.size() - hold);
                // Safety: a channel name is short; if no close arrives, give up
                // suppressing so we never swallow a real (long) answer.
                if (suppressed_ > kMaxHeader) state_ = State::Normal;
                break;
            }
        }
        return out;
    }

    std::string flush() {
        std::string out = (state_ == State::Normal) ? std::move(pending_)
                                                    : std::string();
        pending_.clear();
        return out;
    }

private:
    enum class State { Normal, Suppress };

    template <size_t N>
    void find_first(const std::array<std::string_view, N>& set, size_t& best,
                    size_t& mlen, bool& flag, bool flag_value) {
        for (std::string_view m : set) {
            size_t p = pending_.find(m);
            if (p != std::string::npos && p < best) {
                best = p;
                mlen = m.size();
                flag = flag_value;
            }
        }
    }

    template <size_t N>
    size_t holdback(const std::array<std::string_view, N>& set, size_t cur) {
        for (std::string_view m : set) {
            size_t maxk = std::min(m.size() - 1, pending_.size());
            for (size_t k = maxk; k > cur; --k) {
                if (std::string_view(pending_).substr(pending_.size() - k) ==
                    m.substr(0, k)) {
                    cur = k;
                    break;
                }
            }
        }
        return cur;
    }

    static constexpr size_t kMaxHeader = 64;

    static constexpr std::array<std::string_view, 2> kOpen{"<|channel>",
                                                           "<|channel|>"};
    static constexpr std::array<std::string_view, 2> kClose{"<channel|>",
                                                            "<|message|>"};
    static constexpr std::array<std::string_view, 9> kStrip{
        "<channel|>",   "<|message|>",  "<|end|>",      "<|start|>",
        "<|im_end|>",   "<|im_start|>", "<|return|>",   "<|tool_call>",
        "<tool_call|>"};

    State state_ = State::Normal;
    size_t suppressed_ = 0;
    std::string pending_;
};

}  // namespace lc
