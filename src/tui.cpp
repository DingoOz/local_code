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
// Foreground color pairs use number 2 + COLOR_x (COLOR_BLACK..COLOR_WHITE = 0..7).
constexpr int kFgPairBase = 2;

}  // namespace

TuiConsole::TuiConsole(GpuMonitor& gpu, std::string model, std::string kv_cache)
    : gpu_(gpu),
      model_(std::move(model)),
      kv_label_(std::move(kv_cache)),
      history_path_(history_file()) {
    // Load persisted history.
    std::ifstream f(history_path_);
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) history_.push_back(line);

    initscr();
    cbreak();
    noecho();
    curs_set(1);  // keep a visible (blinking) cursor at the input position
    keypad(stdscr, TRUE);
    // Report mouse-wheel events so the conversation can be scrolled with the
    // wheel (the terminal's own scrollback is unavailable on the alt-screen).
    mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED, nullptr);
    mouseinterval(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(kStatusPair, COLOR_WHITE, COLOR_BLUE);
        // One pair per foreground color over the terminal's default background.
        for (short c = 0; c < 8; ++c) init_pair(kFgPairBase + c, c, -1);
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
    //
    // The output region is an off-screen pad far taller than the screen, so old
    // lines are retained for scrollback rather than discarded. When it fills,
    // scrollok lets it roll (dropping the oldest), bounding memory.
    pad_rows_ = LINES > 2000 ? LINES : 2000;
    WINDOW* o = newpad(pad_rows_, COLS - 2);
    scrollok(o, TRUE);
    keypad(o, TRUE);
    view_ = 0;
    WINDOW* st = newwin(1, COLS - 2, LINES - 2, 1);
    if (has_colors()) wbkgd(st, COLOR_PAIR(kStatusPair));
    else wbkgd(st, A_REVERSE);
    out_ = o;
    status_ = st;

    draw_border();
    draw_status();
    follow();
    show_out();
    doupdate();
}

// Visible height of the output pad on screen (rows 1 .. LINES-3).
int TuiConsole::view_height() const {
    int h = LINES - 3;
    return h < 1 ? 1 : h;
}

// Snap the viewport so the pad's current write position sits at the bottom —
// i.e. follow live output. Called on every print and on input edits.
void TuiConsole::follow() {
    WINDOW* o = static_cast<WINDOW*>(out_);
    int cy, cx;
    getyx(o, cy, cx);
    (void)cx;
    int top = cy - (view_height() - 1);
    view_ = top < 0 ? 0 : top;
}

// Copy the visible slice of the pad (starting at row view_) onto the screen.
void TuiConsole::show_out() {
    WINDOW* o = static_cast<WINDOW*>(out_);
    int smaxrow = LINES - 3;
    if (smaxrow < 1) smaxrow = 1;
    int smaxcol = COLS - 2;
    if (smaxcol < 1) smaxcol = 1;
    pnoutrefresh(o, view_, 0, 1, 1, smaxrow, smaxcol);
}

void TuiConsole::draw_border() {
    box(stdscr, 0, 0);
    std::string title = " local_code — " + model_ + " ";
    if ((int)title.size() > COLS - 4) title = title.substr(0, COLS - 4);
    mvwaddstr(stdscr, 0, 2, title.c_str());
    std::string hint = " PgUp/PgDn or wheel: scroll  F2: menu  /help  /quit ";
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
    if (ctx_pct_ >= 0.0) {
        // Context usage as a small bar chart with the KV-cache quantisation
        // (fp16/q8_0/q4_0) overlaid in the middle of the bar:  ctx [██q8_0░░] 45%
        constexpr int kCells = 14;
        double pct = ctx_pct_ < 0 ? 0 : (ctx_pct_ > 100 ? 100 : ctx_pct_);
        int filled = (int)(pct / 100.0 * kCells + 0.5);
        const std::string& label = kv_label_;
        int lstart = (kCells - (int)label.size()) / 2;
        if (lstart < 0) lstart = 0;
        std::string bar;
        for (int i = 0; i < kCells; ++i) {
            if (i >= lstart && i < lstart + (int)label.size())
                bar += label[i - lstart];        // overlaid quant label
            else
                bar += (i < filled ? "█"     // █ filled
                                   : "░");    // ░ empty
        }
        char cb[32];
        std::snprintf(cb, sizeof cb, " %d%% ", (int)(pct + 0.5));
        s += " |  ctx [" + bar + "]" + cb;
    }
    if (tps_ > 0.0) {
        char tb[48];
        std::snprintf(tb, sizeof tb, " |  %.1f tok/s ", tps_);
        s += tb;
    }
    // When scrolled up, lead with a clear indicator so it's obvious the view is
    // not at the live bottom (and how to get back).
    if (out_) {
        WINDOW* o = static_cast<WINDOW*>(out_);
        int cy, cx;
        getyx(o, cy, cx);
        (void)cx;
        int bottom = cy - (view_height() - 1);
        if (bottom < 0) bottom = 0;
        if (view_ < bottom) {
            char sb[64];
            std::snprintf(sb, sizeof sb, " ▲ SCROLLBACK -%d  (End=bottom) | ",
                          bottom - view_);
            s = std::string(sb) + s;
        }
    }
    int w = COLS - 2;
    if ((int)s.size() > w) {
        // Truncate to the bar width without splitting a multi-byte UTF-8 glyph
        // (the bar/scrollback markers are multibyte), which would render as a
        // stray replacement character at the edge.
        int i = 0;
        while (i < (int)s.size()) {
            unsigned char c = (unsigned char)s[i];
            int len = (c & 0x80) == 0      ? 1
                      : (c & 0xE0) == 0xC0 ? 2
                      : (c & 0xF0) == 0xE0 ? 3
                                           : 4;
            if (i + len > w) break;
            i += len;
        }
        s.resize(i);
    }
    wattron(st, A_BOLD);
    mvwaddstr(st, 0, 0, s.c_str());
    wattroff(st, A_BOLD);
    wnoutrefresh(st);
    // Refresh the output pad LAST so the hardware cursor lands at the input
    // position inside the conversation, not on the status bar.
    if (out_) show_out();
    doupdate();
}

void TuiConsole::refresh_status_throttled() {
    auto now = std::chrono::steady_clock::now();
    if (now - last_status_ >= std::chrono::seconds(1)) {
        draw_status();
        last_status_ = now;
    }
}

void TuiConsole::apply_attr() {
    WINDOW* o = static_cast<WINDOW*>(out_);
    wattr_set(o, cur_bold_ ? A_BOLD : A_NORMAL,
              static_cast<short>(cur_pair_), nullptr);
}

// Walk the text, translating ANSI SGR sequences (\033[..m) into ncurses color
// attributes and writing the literal runs in between. Categories of output keep
// the same colors they have in plain mode (cyan model label, yellow tool lines,
// gray results, magenta questions, green/magenta prompts, red errors).
void TuiConsole::render_ansi(const std::string& text) {
    WINDOW* o = static_cast<WINDOW*>(out_);
    size_t i = 0, n = text.size();
    while (i < n) {
        if (text[i] == '\033' && i + 1 < n && text[i + 1] == '[') {
            size_t j = i + 2;
            while (j < n && text[j] != 'm' &&
                   (std::isdigit((unsigned char)text[j]) || text[j] == ';'))
                ++j;
            if (j < n && text[j] == 'm') {
                std::string params = text.substr(i + 2, j - (i + 2));
                // Apply each ';'-separated SGR code.
                size_t p = 0;
                bool any = false;
                while (p <= params.size()) {
                    size_t sc = params.find(';', p);
                    std::string tok = params.substr(
                        p, sc == std::string::npos ? std::string::npos : sc - p);
                    any = true;
                    int code = tok.empty() ? 0 : std::atoi(tok.c_str());
                    if (code == 0) { cur_pair_ = 0; cur_bold_ = false; }
                    else if (code == 1) cur_bold_ = true;
                    else if (code == 22) cur_bold_ = false;
                    else if (code >= 30 && code <= 37)
                        cur_pair_ = kFgPairBase + (code - 30);
                    else if (code == 39) cur_pair_ = 0;
                    else if (code >= 90 && code <= 97) {
                        cur_pair_ = kFgPairBase + (code - 90);
                        cur_bold_ = true;  // bright = bold variant
                    }
                    if (sc == std::string::npos) break;
                    p = sc + 1;
                }
                if (!any) { cur_pair_ = 0; cur_bold_ = false; }
                apply_attr();
                i = j + 1;
                continue;
            }
            i += 2;  // malformed; drop "\033["
            continue;
        }
        size_t k = i;
        while (k < n && text[k] != '\033') ++k;
        waddnstr(o, text.c_str() + i, static_cast<int>(k - i));
        i = k;
    }
}

void TuiConsole::print(const std::string& text) {
    WINDOW* o = static_cast<WINDOW*>(out_);
    if (has_colors()) {
        render_ansi(text);
    } else {
        std::string clean = strip_ansi(text);
        waddstr(o, clean.c_str());
    }
    follow();  // new output snaps the viewport back to the bottom
    show_out();
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
        follow();
        show_out();
        doupdate();
    };

    // Scroll the conversation viewport by `delta` rows (>0 = up/back), clamped
    // to the top and the live bottom.
    auto scroll_view = [&](int delta) {
        int cy, cx;
        getyx(o, cy, cx);
        (void)cx;
        int bottom = cy - (view_height() - 1);
        if (bottom < 0) bottom = 0;
        view_ -= delta;
        if (view_ < 0) view_ = 0;
        if (view_ > bottom) view_ = bottom;
        show_out();
        draw_status();  // refresh the SCROLLBACK indicator
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
            follow();
            show_out();
            doupdate();
            break;
        }
        if (ch == KEY_F(2) || ch == 7) {  // F2 / Ctrl-G — open the menu
            line = "/menu";
            waddch(o, '\n');
            follow();
            show_out();
            doupdate();
            break;
        }
        // Scrollback: Page keys, Shift+Page, mouse wheel, Home/End.
        if (ch == KEY_PPAGE || ch == KEY_SPREVIOUS) { scroll_view(view_height() - 1); continue; }
        if (ch == KEY_NPAGE || ch == KEY_SNEXT)     { scroll_view(-(view_height() - 1)); continue; }
        if (ch == KEY_HOME) { scroll_view(view_); continue; }        // jump to top
        if (ch == KEY_END)  { scroll_view(-(1 << 20)); continue; }     // jump to bottom
        if (ch == KEY_MOUSE) {
            MEVENT me;
            if (getmouse(&me) == OK) {
                if (me.bstate & BUTTON4_PRESSED) scroll_view(3);       // wheel up
                else if (me.bstate & BUTTON5_PRESSED) scroll_view(-3); // wheel down
            }
            continue;
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
                follow();
                show_out();
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
            follow();  // typing snaps back to the bottom if scrolled up
            show_out();
            doupdate();
        }
    }
    wtimeout(o, -1);
    if (!line.empty() && (history_.empty() || history_.back() != line))
        history_.push_back(line);
    return line;
}

Confirm TuiConsole::confirm(const std::string& prompt, bool allow_always) {
    // Discard any keys typed while the model was generating so a safety prompt
    // is never auto-answered by stale type-ahead.
    flushinp();
    auto line = input(prompt + (allow_always ? " [y/N/a=always] " : " [y/N] "));
    if (!line) return Confirm::No;
    // Lenient parse: first non-space character decides (handles a leaked
    // leading space like " y", which previously read as "No").
    for (unsigned char c : *line) {
        if (std::isspace(c)) continue;
        if (allow_always && (c == 'a' || c == 'A')) return Confirm::Always;
        return (c == 'y' || c == 'Y') ? Confirm::Once : Confirm::No;
    }
    return Confirm::No;
}

void TuiConsole::set_tps(double tokens_per_sec) {
    // Touched only on the main loop thread (same as draw_status); no lock needed.
    tps_ = tokens_per_sec;
}

void TuiConsole::set_ctx(double percent_used) { ctx_pct_ = percent_used; }

}  // namespace lc
