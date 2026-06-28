#include "plain_console.hpp"

#include <cctype>
#include <iostream>

namespace lc {

namespace {
// Interpret a y/N/a answer leniently: the first non-space character decides, so
// stray leading whitespace or type-ahead (" y", "y\n") doesn't read as "No".
Confirm decide(const std::string& line) {
    for (unsigned char c : line) {
        if (std::isspace(c)) continue;
        if (c == 'a' || c == 'A') return Confirm::Always;
        if (c == 'y' || c == 'Y') return Confirm::Once;
        return Confirm::No;
    }
    return Confirm::No;
}
}  // namespace

void PlainConsole::print(const std::string& text) {
    std::cout << text << std::flush;
}

std::optional<std::string> PlainConsole::input(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    return line;
}

Confirm PlainConsole::confirm(const std::string& prompt, bool allow_always) {
    auto line = input(prompt + (allow_always ? " [y/N/a=always] " : " [y/N] "));
    if (!line) return Confirm::No;
    Confirm d = decide(*line);
    if (!allow_always && d == Confirm::Always) d = Confirm::Once;
    return d;
}

}  // namespace lc
