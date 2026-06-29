#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "io.hpp"

namespace lc {

class GpuMonitor;

// ncurses console: a bordered screen with a scrolling output region, inline
// line input (with history), and a bottom status bar showing live GPU stats.
// The output region is an off-screen pad that retains the recent conversation,
// so the user can scroll back through it with PageUp/PageDown. Only safe to
// construct when stdin/stdout are a real terminal.
class TuiConsole : public Console {
public:
    // kv_cache is the active Ollama KV-cache quantisation ("fp16"/"q8_0"/"q4_0"),
    // overlaid onto the context-usage bar in the status line.
    TuiConsole(GpuMonitor& gpu, std::string model, std::string kv_cache = "fp16");
    ~TuiConsole() override;

    void print(const std::string& text) override;
    std::optional<std::string> input(const std::string& prompt) override;
    Confirm confirm(const std::string& prompt,
                    bool allow_always = false) override;
    void set_tps(double tokens_per_sec) override;
    void set_ctx(double percent_used) override;

private:
    void layout();
    void draw_border();
    void draw_status();
    void refresh_status_throttled();
    void render_ansi(const std::string& text);  // translate ANSI SGR -> ncurses
    void apply_attr();                           // push cur_pair_/cur_bold_
    int view_height() const;  // visible rows of the output pad
    void follow();            // snap the viewport to the latest output (bottom)
    void show_out();          // push the visible pad region to the screen

    GpuMonitor& gpu_;
    std::string model_;
    std::string kv_label_;    // KV-cache quant overlaid on the ctx bar
    void* out_ = nullptr;     // WINDOW* — output pad (scrollback) + input echo
    void* status_ = nullptr;  // WINDOW* — bottom GPU bar
    int pad_rows_ = 0;        // height of the output pad (scrollback depth)
    int view_ = 0;            // top pad row currently shown (for scrollback)
    std::vector<std::string> history_;
    std::string history_path_;
    std::chrono::steady_clock::time_point last_status_{};
    int cur_pair_ = 0;        // current ncurses color-pair (0 = default)
    bool cur_bold_ = false;   // current bold state
    double tps_ = 0.0;        // latest generation speed (tokens/sec), 0 = hide
    double ctx_pct_ = -1.0;   // context-window usage %, <0 = hide
};

}  // namespace lc
