#pragma once

#include "io.hpp"

namespace lc {

// Console backed by std::cout / std::getline. Used for non-interactive use
// (piped stdin/stdout) and when --no-tui is set. ANSI colors pass through.
class PlainConsole : public Console {
public:
    void print(const std::string& text) override;
    std::optional<std::string> input(const std::string& prompt) override;
    Confirm confirm(const std::string& prompt,
                    bool allow_always = false) override;
};

}  // namespace lc
