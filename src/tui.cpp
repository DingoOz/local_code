#include "tui.hpp"

#include <ncurses.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "gpu_monitor.hpp"

namespace lc {

namespace {

// Remove ANSI CSI sequences (e.g. "\033[36m") — ncurses would print them raw.
std::string strip_ansi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\033') {
            size_t j = i + 1;
            if (j < s.size() && s[j] == '[') {
                ++j;
                while (j < s.size() && !std::isalpha((unsigned char)s[j])) ++j;
                if (j < s.size()) ++j;  // consume the final letter
            } else {
                ++j;
            }
            i = j;
        } else {
            out += s[i++];
        }
    }
    return out;
}

std::string history_file() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.local_code_history";
}

constexpr int kStatusPair = 1;

}  // namespace

TuiConsole::TuiConsole(GpuMonitor& gpu, std::string model)
    : gpu_(gpu), model_(std::move(model)), history_path_(history_file()) {
    // Load persisted history.
    std::ifstream f(history_path_);
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) history_.push_back(line);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(kStatusPair, COLOR_WHITE, COLOR_BLUE);
    }
    layout();
}

TuiConsole::~TuiConsole() {
    if (out_) delwin(static_cast<WINDOW*>(out_));
    if (status_) delwin(static_cast<WINDOW*>(status_));
    endwin();

    // Persist the most recent history entries.
    std::ofstream f(history_path_, std::ios::trunc);
    size_t start = history_.size() > 500 ? history_.size() - 500 : 0;
    for (size_t i = start; i < history_.size(); ++i) f << history_[i] << "\n";
}

void TuiConsole::layout() {
    if (out_) delwin(static_cast<WINDOW*>(out_));
    if (status_) delwin(static_cast<WINDOW*>(status_));

    // Rows: 0 = top border, 1..LINES-3 = output, LINES-2 = status, LINES-1 =
    // bottom border. Columns 0 and COLS-1 are the side borders.
    int out_h = LINES - 3;
    if (out_h < 1) out_h = 1;
    WINDOW* o = newwin(out_h, COLS - 2, 1, 1);
    scrollok(o, TRUE);
    keypad(o, TRUE);
    WINDOW* st = newwin(1, COLS - 2, LINES - 2, 1);
    if (has_colors()) wbkgd(st, COLOR_PAIR(kStatusPair));
    else wbkgd(st, A_REVERSE);
    out_ = o;
    status_ = st;

    draw_border();
    draw_status();
    wrefresh(o);
}

void TuiConsole::draw_border() {
    box(stdscr, 0, 0);
    std::string title = " local_code — " + model_ + " ";
    if ((int)title.size() > COLS - 4) title = title.substr(0, COLS - 4);
    mvwaddstr(stdscr, 0, 2, title.c_str());
    std::string hint = " /help  /plan  /build  /quit ";
    if ((int)hint.size() < COLS - 4)
        mvwaddstr(stdscr, LINES - 1, 2, hint.c_str());
    wnoutrefresh(stdscr);
    doupdate();
}

void TuiConsole::draw_status() {
    WINDOW* st = static_cast<WINDOW*>(status_);
    werase(st);
    GpuStats g = gpu_.get();
    char buf[320];
    if (g.available) {
        int pct = g.mem_total > 0 ? (int)(g.mem_used * 100 / g.mem_total) : 0;
        std::snprintf(buf, sizeof buf,
                      " %s  |  GPU %3d%%  |  VRAM %ld/%ld MB (%d%%) ",
                      g.name.c_str(), g.util, g.mem_used, g.mem_total, pct);
    } else {
        std::snprintf(buf, sizeof buf, " GPU: nvidia-smi unavailable ");
    }
    std::string s(buf);
    int w = COLS - 2;
    if ((int)s.size() > w) s = s.substr(0, w);
    wattron(st, A_BOLD);
    mvwaddstr(st, 0, 0, s.c_str());
    wattroff(st, A_BOLD);
    wnoutrefresh(st);
    doupdate();
}

void TuiConsole::refresh_status_throttled() {
    auto now = std::chrono::steady_clock::now();
    if (now - last_status_ >= std::chrono::seconds(1)) {
        draw_status();
        last_status_ = now;
    }
}

void TuiConsole::print(const std::string& text) {
    WINDOW* o = static_cast<WINDOW*>(out_);
    std::string clean = strip_ansi(text);
    waddstr(o, clean.c_str());
    wnoutrefresh(o);
    refresh_status_throttled();
    doupdate();
}

std::optional<std::string> TuiConsole::input(const std::string& prompt) {
    WINDOW* o = static_cast<WINDOW*>(out_);
    print(prompt);

    std::string line;
    int hist_idx = static_cast<int>(history_.size());  // one past the end

    auto erase_typed = [&](size_t n) {
        for (size_t i = 0; i < n; ++i) {
            int y, x;
            getyx(o, y, x);
            if (x > 0) {
                wmove(o, y, x - 1);
                wdelch(o);
            }
        }
    };
    auto replace_line = [&](const std::string& repl) {
        erase_typed(line.size());
        waddstr(o, repl.c_str());
        line = repl;
        wnoutrefresh(o);
        doupdate();
    };

    wtimeout(o, 500);  // wake periodically to refresh the status bar
    for (;;) {
        int ch = wgetch(o);
        if (ch == ERR) {  // timeout
            draw_status();
            last_status_ = std::chrono::steady_clock::now();
            continue;
        }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            waddch(o, '\n');
            wnoutrefresh(o);
            doupdate();
            break;
        }
        if (ch == KEY_RESIZE) {
            layout();
            print(prompt);
            print(line);
            continue;
        }
        if (ch == 4) {  // Ctrl-D
            if (line.empty()) {
                wtimeout(o, -1);
                return std::nullopt;
            }
            continue;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (!line.empty()) {
                line.pop_back();
                erase_typed(1);
                wnoutrefresh(o);
                doupdate();
            }
            continue;
        }
        if (ch == KEY_UP) {
            if (hist_idx > 0) replace_line(history_[--hist_idx]);
            continue;
        }
        if (ch == KEY_DOWN) {
            if (hist_idx < (int)history_.size()) {
                ++hist_idx;
                replace_line(hist_idx < (int)history_.size()
                                 ? history_[hist_idx]
                                 : std::string());
            }
            continue;
        }
        if (ch >= 32 && ch < 127) {
            line.push_back(static_cast<char>(ch));
            waddch(o, ch);
            wnoutrefresh(o);
            doupdate();
        }
    }
    wtimeout(o, -1);
    if (!line.empty() && (history_.empty() || history_.back() != line))
        history_.push_back(line);
    return line;
}

bool TuiConsole::confirm(const std::string& prompt) {
    auto line = input(prompt + " [y/N] ");
    if (!line) return false;
    return *line == "y" || *line == "Y" || *line == "yes";
}

}  // namespace lc
