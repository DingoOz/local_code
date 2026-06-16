#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "io.hpp"

namespace lc {

class GpuMonitor;

// ncurses console: a bordered screen with a scrolling output region, inline
// line input (with history), and a bottom status bar showing live GPU stats.
// Only safe to construct when stdin/stdout are a real terminal.
class TuiConsole : public Console {
public:
    TuiConsole(GpuMonitor& gpu, std::string model);
    ~TuiConsole() override;

    void print(const std::string& text) override;
    std::optional<std::string> input(const std::string& prompt) override;
    bool confirm(const std::string& prompt) override;

private:
    void layout();
    void draw_border();
    void draw_status();
    void refresh_status_throttled();

    GpuMonitor& gpu_;
    std::string model_;
    void* out_ = nullptr;     // WINDOW* — scrolling output + input
    void* status_ = nullptr;  // WINDOW* — bottom GPU bar
    std::vector<std::string> history_;
    std::string history_path_;
    std::chrono::steady_clock::time_point last_status_{};
};

}  // namespace lc
