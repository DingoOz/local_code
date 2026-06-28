#pragma once

#include <string>

namespace lc {

// Context window that keeps the 9B Q4_K_M Ornith model (5.6 GB weights) fully
// resident on an 8 GB GPU. Measured: 32K ctx -> ~6.3 GB (100% GPU); 48K spills
// to CPU. 32K leaves headroom for the desktop/compositor and compute buffers.
constexpr int kGpuFitNumCtx = 32768;

// Model selected by --gpu when the user does not pass --model.
constexpr const char* kGpuFitModel = "ornith:latest";

struct Config {
    std::string host = "http://localhost:11434";
    std::string model;                  // empty => prompt at startup
    std::string system_file;            // optional override for system prompt

    // History token budget (heuristic ~bytes/4). Kept small so weak models stay
    // sharp; the system prompt is always retained on top of this.
    int budget_tokens = 4096;

    // Skip y/N confirmation before write_file / run_command.
    bool yolo = false;

    // Start in planning mode: reason about the approach and ask questions, but
    // never write files or run commands.
    bool plan_mode = false;

    // Enable model "thinking". Off by default: thinking models otherwise emit
    // their answer into a separate field and often leave content empty, which
    // looks like a blank reply. When on, reasoning is streamed too. Reasoning
    // models like Ornith-1 enable this automatically (see apply_model_defaults).
    bool think = false;
    bool think_set = false;  // true once --think/--no-think pinned it explicitly

    // Sampling + context options passed through to Ollama's `options`. Sentinel
    // values mean "unset": leave the option out of the payload and use the
    // server default. Ornith-1 recommends temperature 0.6 / top_p 0.95 /
    // top_k 20, applied automatically for ornith-named models when left unset.
    double temperature = -1.0;  // <0 => unset
    double top_p = -1.0;        // <0 => unset
    int top_k = -1;             // <0 => unset
    int num_ctx = 0;            // 0  => unset (Ollama default context window)

    // --gpu: start on the Ornith model with a context window sized to keep it
    // fully on the GPU (see kGpuFitNumCtx). Implies the Ornith model unless the
    // user pins --model, and a fitting --num-ctx unless one was given.
    bool fit_gpu = false;

    // True once any of the sampling fields was set explicitly on the CLI, so the
    // Ornith auto-tune never clobbers a user's choice.
    bool sampling_set = false;

    // Disable the ncurses TUI even on a terminal (use the plain stream).
    bool no_tui = false;

    // Web search via a local SearXNG instance.
    std::string searxng_url = "http://localhost:8888";
    bool web_forced = false;   // --web: enable without probing
    bool no_web = false;       // --no-web: never use web search
    bool web_enabled = false;  // resolved at startup (probe or forced)

    // Project awareness: a single project rooted at project_root, with durable
    // notes at notes_path. Resolved at startup from --project / the cwd.
    std::string project_dir;    // --project DIR override (empty => cwd)
    std::string project_root;   // resolved absolute root
    std::string notes_path;     // <root>/.local_code/PROJECT.md
    bool no_project = false;    // --no-project: disable project awareness

    // Safety / sizing caps.
    int max_tool_turns = 12;            // consecutive tool calls before user check-in
    size_t max_read_bytes = 16 * 1024;  // truncate large file reads
    size_t max_cmd_output = 16 * 1024;  // truncate command output

    // Returns false and prints usage if args are invalid; sets *exit_now on --help.
    static bool parse(int argc, char** argv, Config& out, bool& exit_now);
    static void print_usage(const char* argv0);

    // Apply model-specific defaults once `model` is known (after the startup
    // picker). For Ornith-1 (model name contains "ornith", case-insensitive)
    // this enables thinking and the recommended sampling — but only for fields
    // the user did not pin on the CLI. Returns true if any default was applied
    // (so the caller can surface it in the banner).
    bool apply_model_defaults();
};

}  // namespace lc
