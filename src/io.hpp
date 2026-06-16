#pragma once

#include <optional>
#include <string>

namespace lc {

// Abstraction over the terminal I/O surface so the agent and tools don't depend
// on whether output goes to a plain stream or an ncurses TUI.
class Console {
public:
    virtual ~Console() = default;

    // Append text to the output area. Strings may contain newlines and ANSI
    // color codes (honored in plain mode, stripped by the TUI).
    virtual void print(const std::string& text) = 0;

    // Read one line shown after `prompt`. Returns nullopt on EOF (Ctrl-D).
    virtual std::optional<std::string> input(const std::string& prompt) = 0;

    // Yes/No confirmation (defaults to No).
    virtual bool confirm(const std::string& prompt) = 0;
};

}  // namespace lc
