#include "config.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace lc {

void Config::print_usage(const char* argv0) {
    std::cerr <<
        "Usage: " << argv0 << " [options]\n"
        "  --model NAME     Ollama model to use (default: pick at startup)\n"
        "  --host URL       Ollama host (default: http://localhost:11434)\n"
        "  --budget N       History token budget (default: 4096)\n"
        "  --system FILE    Override the system prompt from FILE\n"
        "  --yolo           Auto-execute tools without y/N confirmation\n"
        "  --plan           Start in planning mode (no writes/commands)\n"
        "  --think          Enable model thinking (off by default)\n"
        "  --no-tui         Disable the ncurses TUI (plain output)\n"
        "  -h, --help       Show this help\n";
}

bool Config::parse(int argc, char** argv, Config& out, bool& exit_now) {
    exit_now = false;
    auto need_value = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << argv[i] << "\n";
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
            print_usage(argv[0]);
            exit_now = true;
            return true;
        } else if (!std::strcmp(a, "--model")) {
            const char* v = need_value(i); if (!v) return false;
            out.model = v;
        } else if (!std::strcmp(a, "--host")) {
            const char* v = need_value(i); if (!v) return false;
            out.host = v;
        } else if (!std::strcmp(a, "--system")) {
            const char* v = need_value(i); if (!v) return false;
            out.system_file = v;
        } else if (!std::strcmp(a, "--budget")) {
            const char* v = need_value(i); if (!v) return false;
            out.budget_tokens = std::atoi(v);
            if (out.budget_tokens < 256) {
                std::cerr << "--budget must be >= 256\n";
                return false;
            }
        } else if (!std::strcmp(a, "--yolo")) {
            out.yolo = true;
        } else if (!std::strcmp(a, "--plan")) {
            out.plan_mode = true;
        } else if (!std::strcmp(a, "--think")) {
            out.think = true;
        } else if (!std::strcmp(a, "--no-tui")) {
            out.no_tui = true;
        } else {
            std::cerr << "Unknown argument: " << a << "\n";
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

}  // namespace lc
