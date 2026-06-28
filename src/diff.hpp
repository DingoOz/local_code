#pragma once

#include <string>
#include <vector>

namespace lc {

// A compact, colored diff preview between two texts, for showing a proposed
// write/edit before the user confirms. Not a minimal edit script: it trims the
// common leading/trailing lines and shows the changed block as red '-' (removed)
// and green '+' (added) lines, with a little context and an overall cap so a
// huge change never floods the screen.
inline std::string preview_diff(const std::string& old_s,
                                const std::string& new_s,
                                size_t max_lines = 60) {
    auto split = [](const std::string& s) {
        std::vector<std::string> lines;
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t nl = s.find('\n', pos);
            if (nl == std::string::npos) {
                lines.push_back(s.substr(pos));
                break;
            }
            lines.push_back(s.substr(pos, nl - pos));
            pos = nl + 1;
        }
        return lines;
    };

    std::vector<std::string> a = split(old_s), b = split(new_s);
    // Trim common prefix.
    size_t pre = 0;
    while (pre < a.size() && pre < b.size() && a[pre] == b[pre]) ++pre;
    // Trim common suffix (not overlapping the prefix).
    size_t suf = 0;
    while (suf < a.size() - pre && suf < b.size() - pre &&
           a[a.size() - 1 - suf] == b[b.size() - 1 - suf])
        ++suf;

    const size_t a_end = a.size() - suf, b_end = b.size() - suf;
    std::string out;
    size_t shown = 0;
    auto emit = [&](const char* color, char sign, const std::string& line) {
        if (shown >= max_lines) return;
        out += "\033[";
        out += color;
        out += "m";
        out += sign;
        out += ' ';
        out += line;
        out += "\033[0m\n";
        ++shown;
    };

    // One line of leading context, if any was trimmed.
    if (pre > 0) emit("90", ' ', a[pre - 1]);
    for (size_t i = pre; i < a_end && shown < max_lines; ++i) emit("31", '-', a[i]);
    for (size_t i = pre; i < b_end && shown < max_lines; ++i) emit("32", '+', b[i]);
    if (suf > 0 && shown < max_lines) emit("90", ' ', a[a.size() - suf]);

    const size_t changed = (a_end - pre) + (b_end - pre);
    if (changed > shown)
        out += "\033[90m  ... (" + std::to_string(changed - shown) +
               " more changed lines)\033[0m\n";
    if (out.empty()) out = "\033[90m(no textual change)\033[0m\n";
    return out;
}

}  // namespace lc
