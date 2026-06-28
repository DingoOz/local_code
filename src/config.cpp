#include "config.hpp"

#include <cctype>
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
        "  --no-think       Disable model thinking (overrides auto-enable)\n"
        "  --temperature F  Sampling temperature (Ornith default 0.6)\n"
        "  --top-p F        Nucleus sampling top_p (Ornith default 0.95)\n"
        "  --top-k N        Top-k sampling (Ornith default 20)\n"
        "  --num-ctx N      Context window size in tokens (e.g. 8192)\n"
        "  --gpu            Start on Ornith with a context sized to fit an 8 GB\n"
        "                   GPU (32K ctx; implies the Ornith model)\n"
        "  --no-tui         Disable the ncurses TUI (plain output)\n"
        "  --searxng URL    SearXNG base URL (default http://localhost:8888)\n"
        "  --web            Force-enable web search (skip the probe)\n"
        "  --no-web         Disable web search\n"
        "  --project DIR    Project root (default: current directory)\n"
        "  --no-project     Disable project awareness / notes\n"
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
            out.think_set = true;
        } else if (!std::strcmp(a, "--no-think")) {
            out.think = false;
            out.think_set = true;
        } else if (!std::strcmp(a, "--temperature")) {
            const char* v = need_value(i); if (!v) return false;
            out.temperature = std::atof(v);
            out.sampling_set = true;
        } else if (!std::strcmp(a, "--top-p")) {
            const char* v = need_value(i); if (!v) return false;
            out.top_p = std::atof(v);
            out.sampling_set = true;
        } else if (!std::strcmp(a, "--top-k")) {
            const char* v = need_value(i); if (!v) return false;
            out.top_k = std::atoi(v);
            out.sampling_set = true;
        } else if (!std::strcmp(a, "--num-ctx")) {
            const char* v = need_value(i); if (!v) return false;
            out.num_ctx = std::atoi(v);
            if (out.num_ctx < 0) { std::cerr << "--num-ctx must be >= 0\n"; return false; }
        } else if (!std::strcmp(a, "--gpu")) {
            out.fit_gpu = true;
        } else if (!std::strcmp(a, "--no-tui")) {
            out.no_tui = true;
        } else if (!std::strcmp(a, "--searxng")) {
            const char* v = need_value(i); if (!v) return false;
            out.searxng_url = v;
        } else if (!std::strcmp(a, "--web")) {
            out.web_forced = true;
        } else if (!std::strcmp(a, "--no-web")) {
            out.no_web = true;
        } else if (!std::strcmp(a, "--project")) {
            const char* v = need_value(i); if (!v) return false;
            out.project_dir = v;
        } else if (!std::strcmp(a, "--no-project")) {
            out.no_project = true;
        } else {
            std::cerr << "Unknown argument: " << a << "\n";
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

bool Config::apply_model_defaults() {
    // Case-insensitive substring match on the model name.
    std::string lower = model;
    for (char& c : lower) c = static_cast<char>(std::tolower((unsigned char)c));
    const bool is_ornith = lower.find("ornith") != std::string::npos;
    if (!is_ornith) return false;

    bool applied = false;
    // Ornith-1 is a reasoning model: enable thinking unless the user pinned it.
    if (!think_set && !think) { think = true; applied = true; }
    // Recommended sampling, only if the user passed no sampling flags at all.
    if (!sampling_set) {
        temperature = 0.6;
        top_p = 0.95;
        top_k = 20;
        applied = true;
    }
    // Context window, when --num-ctx was not given. With --gpu, size it to keep
    // the model fully on an 8 GB GPU; otherwise use Ornith-1's native 256K
    // window (a large KV cache — pass --num-ctx or --gpu if it won't fit VRAM).
    if (num_ctx == 0) {
        num_ctx = fit_gpu ? kGpuFitNumCtx : 262144;  // 256 * 1024
        applied = true;
    }
    return applied;
}

}  // namespace lc
