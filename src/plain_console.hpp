#pragma once

#include "io.hpp"

namespace lc {

// Console backed by std::cout / std::getline. Used for non-interactive use
// (piped stdin/stdout) and when --no-tui is set. ANSI colors pass through.
class PlainConsole : public Console {
public:
    void print(const std::string& text) override;
    std::optional<std::string> input(const std::string& prompt) override;
    bool confirm(const std::string& prompt) override;
};

}  // namespace lc
