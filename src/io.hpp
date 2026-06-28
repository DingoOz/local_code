#pragma once

#include <optional>
#include <string>

namespace lc {

// Result of a confirmation prompt. Always == approve just this once; Always ==
// approve and remember (the caller records an allowlist rule).
enum class Confirm { No, Once, Always };

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

    // Confirmation prompt (defaults to No). When allow_always is true the user
    // may also answer "always" to approve this kind of action permanently.
    virtual Confirm confirm(const std::string& prompt,
                            bool allow_always = false) = 0;

    // Report the latest generation speed (tokens/second) for display, e.g. in a
    // status bar. Default no-op for consoles without one (plain stream).
    virtual void set_tps(double /*tokens_per_sec*/) {}

    // Report context-window usage (0..100%) for display. Default no-op.
    virtual void set_ctx(double /*percent_used*/) {}
};

}  // namespace lc
