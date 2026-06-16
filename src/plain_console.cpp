#include "plain_console.hpp"

#include <iostream>

namespace lc {

void PlainConsole::print(const std::string& text) {
    std::cout << text << std::flush;
}

std::optional<std::string> PlainConsole::input(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    return line;
}

bool PlainConsole::confirm(const std::string& prompt) {
    auto line = input(prompt + " [y/N] ");
    if (!line) return false;
    return *line == "y" || *line == "Y" || *line == "yes";
}

}  // namespace lc
