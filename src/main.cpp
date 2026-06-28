#include <unistd.h>

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "agent.hpp"
#include "config.hpp"
#include "conversation.hpp"
#include "gpu_monitor.hpp"
#include "io.hpp"
#include "ollama_client.hpp"
#include "permissions.hpp"
#include "plain_console.hpp"
#include "project.hpp"
#include "system_prompt.hpp"
#include "tui.hpp"
#include "undo.hpp"
#include "web_search.hpp"

namespace fs = std::filesystem;

using namespace lc;

namespace {

std::string load_system_prompt(const Config& cfg) {
    if (cfg.system_file.empty()) return kSystemPrompt;
    std::ifstream f(cfg.system_file);
    if (!f) {
        std::cerr << "Warning: cannot read --system file '" << cfg.system_file
                  << "', using built-in prompt.\n";
        return kSystemPrompt;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Suggested when no models are installed: a coder model with clean, consistent
// tool-calling that works well with this agent's protocol.
constexpr const char* kRecommendedModel = "qwen2.5-coder:7b";

bool ask_yes(const std::string& prompt) {
    std::cout << prompt << " [y/N]: " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    return line == "y" || line == "Y" || line == "yes";
}

// Download a model with a single-line progress indicator. Throws on failure.
void download_model(OllamaClient& client, const std::string& name) {
    std::cout << "Downloading " << name << "\n(this can take a while)...\n";
    std::string last_status;
    client.pull_model(
        name, [&](const std::string& status, long long completed,
                  long long total) {
            if (total > 0) {
                std::printf("\r  %-24s %3lld%%  %.2f / %.2f GB        ",
                            status.c_str(), completed * 100 / total,
                            completed / 1e9, total / 1e9);
            } else if (status != last_status) {
                std::printf("\r  %-48s", status.c_str());
                last_status = status;
            }
            std::fflush(stdout);
        });
    std::cout << "\n  Download complete.\n";
}

// Interactive model picker when --model is not supplied.
std::string pick_model(OllamaClient& client, const std::string& preset) {
    std::vector<std::string> models = client.list_models();

    // No models installed: offer to download one (the preset if given, else the
    // recommended coder model).
    if (models.empty()) {
        const std::string target =
            preset.empty() ? std::string(kRecommendedModel) : preset;
        std::cout << "No Ollama models are installed.\n"
                     "Download this model now? (it may be several GB)\n  "
                  << target << "\n";
        if (!ask_yes("Download")) {
            std::cout << "No model to use — exiting. Pull one with "
                         "`ollama pull <model>` and run again.\n";
            std::exit(0);
        }
        download_model(client, target);
        return target;
    }

    if (!preset.empty()) {
        for (const auto& m : models)
            if (m == preset) return preset;
        std::cerr << "Note: model '" << preset
                  << "' not in installed list; using it anyway.\n";
        return preset;
    }

    std::cout << "Available models:\n";
    for (size_t i = 0; i < models.size(); ++i)
        std::cout << "  " << (i + 1) << ") " << models[i] << "\n";
    std::cout << "(tip: for the strongest native tool-calling, try an Ornith-1 "
                 "GGUF — reasoning + recommended sampling auto-enable.)\n";

    while (true) {
        std::cout << "Select model [1-" << models.size() << "]: " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) std::exit(0);
        try {
            size_t idx = std::stoul(line);
            if (idx >= 1 && idx <= models.size()) return models[idx - 1];
        } catch (...) {
        }
        std::cout << "Invalid choice.\n";
    }
}

// Builds a summarizer bound to the client/model — compacts evicted turns.
Conversation::Summarizer make_summarizer(OllamaClient& client,
                                         const std::string& model) {
    return [&client, model](const std::vector<Message>& msgs) -> std::string {
        std::vector<Message> req;
        req.push_back(
            {Role::System,
             "You compress conversations. Output <=120 words capturing key "
             "facts, file paths, decisions, and open tasks. No preamble."});
        std::ostringstream body;
        for (const auto& m : msgs)
            body << role_to_api(m.role) << ": " << m.content << "\n";
        req.push_back({Role::User,
                       "Summarize this conversation:\n" + body.str()});
        try {
            return client.chat(model, req, nullptr).content;
        } catch (...) {
            return "";  // fall back to plain eviction
        }
    };
}

const char* kHelpText =
    "Commands:\n"
    "  /help    show this help\n"
    "  /plan    enter planning mode (design & ask questions, no changes)\n"
    "  /build   enter build mode (can write files / run commands)\n"
    "  /learn   scan the project and write its notes (.local_code/PROJECT.md)\n"
    "  /project show the project root and notes status\n"
    "  /undo    revert the last file write/edit\n"
    "  /compact summarize the conversation now and shrink the context\n"
    "  /yolo    toggle auto-accept (run tools without confirmation)\n"
    "  /reset   clear the conversation (keeps system prompt)\n"
    "  /model   show the active model\n"
    "  /quit    exit\n"
    "Custom commands: drop a Markdown file in .local_code/commands/ (foo.md ->\n"
    "  /foo); its text is sent to the agent ($ARGS becomes any trailing text).\n"
    "Anything else is sent to the agent.\n";

// Load custom slash commands from <dir>/*.md: filename stem -> file contents.
std::map<std::string, std::string> load_commands(const std::string& dir) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec) || e.path().extension() != ".md") continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        out[e.path().stem().string()] = ss.str();
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    // Adopt the user's locale so ncurses (wide build) decodes/renders UTF-8 in
    // the TUI — without this, multibyte glyphs like "—" show as mojibake.
    std::setlocale(LC_ALL, "");

    Config cfg;
    bool exit_now = false;
    if (!Config::parse(argc, argv, cfg, exit_now)) return 2;
    if (exit_now) return 0;

    OllamaClient client(cfg.host);

    // --gpu starts on the Ornith model (sized to fit the GPU) unless the user
    // pinned a different model with --model.
    if (cfg.fit_gpu && cfg.model.empty()) cfg.model = kGpuFitModel;

    try {
        cfg.model = pick_model(client, cfg.model);
    } catch (const std::exception& e) {
        std::cerr << "Startup error: " << e.what() << "\n";
        return 1;
    }

    // Apply model-specific defaults (Ornith-1: thinking + recommended sampling)
    // for any options the user left unset.
    const bool tuned = cfg.apply_model_defaults();

    // Resolve web search: probe the local SearXNG unless disabled. A refused
    // connection returns instantly, so there's no penalty when it isn't running.
    if (!cfg.no_web)
        cfg.web_enabled = cfg.web_forced || web_search_available(cfg.searxng_url);

    std::string build_prompt = load_system_prompt(cfg);
    std::string plan_prompt = kPlanSystemPrompt;
    if (cfg.web_enabled) {
        build_prompt += std::string("\n") + kWebToolLine;
        plan_prompt += std::string("\n") + kWebToolLine;
    }

    // Project awareness: resolve the root + notes path and inject a project
    // context block (root, layout, persisted notes) into both prompts.
    Project project(cfg.project_dir);
    if (!cfg.no_project) {
        // Operate inside the project so tools (read_file, list_dir,
        // run_command) and the notes path are all relative to its root.
        if (chdir(project.root().c_str()) != 0)
            std::cerr << "Warning: cannot enter project dir '" << project.root()
                      << "'\n";
        cfg.project_root = project.root();
        cfg.notes_path = project.notes_path();
        const std::string block =
            std::string("\n") + kProjectToolLine + "\n" + project.context_block();
        build_prompt += block;
        plan_prompt += block;
    }

    const std::string initial_prompt =
        cfg.plan_mode ? plan_prompt : build_prompt;

    Conversation convo(initial_prompt, cfg.budget_tokens);
    convo.set_summarizer(make_summarizer(client, cfg.model));

    // Use the ncurses TUI only on a real terminal; fall back to a plain stream
    // for piped/redirected I/O (and when --no-tui is set).
    const bool use_tui = !cfg.no_tui && isatty(STDIN_FILENO) &&
                         isatty(STDOUT_FILENO);
    std::unique_ptr<GpuMonitor> gpu;
    std::unique_ptr<Console> console;
    if (use_tui) {
        gpu = std::make_unique<GpuMonitor>();
        console = std::make_unique<TuiConsole>(*gpu, cfg.model);
    } else {
        console = std::make_unique<PlainConsole>();
    }

    // Allowlist, undo checkpoints, and custom commands live under .local_code
    // (relative to the project root we chdir'd into, or the cwd otherwise).
    PermissionStore perms(".local_code/permissions");
    UndoStack undo(".local_code/backups");
    auto commands = load_commands(".local_code/commands");

    Agent agent(client, convo, cfg, build_prompt, plan_prompt, *console, perms,
                undo);

    console->print(std::string("\nlocal_code — agent on '") + cfg.model +
                   "' (budget " + std::to_string(cfg.budget_tokens) + " tok" +
                   (cfg.yolo ? ", yolo" : "") +
                   (agent.plan_mode() ? ", PLAN" : "") +
                   (cfg.web_enabled ? ", web" : "") +
                   (cfg.think ? ", think" : "") +
                   (cfg.num_ctx > 0
                        ? ", ctx " + std::to_string(cfg.num_ctx)
                        : "") +
                   (cfg.fit_gpu ? ", gpu-fit" : "") +
                   (tuned ? ", ornith-tuned" : "") +
                   "). /help for commands.\n");

    while (true) {
        const std::string prompt = agent.plan_mode()
                                       ? "\n\033[35myou (plan)>\033[0m "
                                       : "\n\033[32myou>\033[0m ";
        auto in = console->input(prompt);
        if (!in) break;  // EOF / Ctrl-D
        std::string input = *in;

        // Trim trailing whitespace.
        while (!input.empty() && (input.back() == ' ' || input.back() == '\n'))
            input.pop_back();
        if (input.empty()) continue;

        if (input == "/quit" || input == "/exit") break;
        if (input == "/help") { console->print(kHelpText); continue; }
        if (input == "/plan") {
            agent.set_plan_mode(true);
            console->print(
                "Planning mode: I'll design the approach and may ask "
                "questions. No files written, no commands run.\n");
            continue;
        }
        if (input == "/build") {
            agent.set_plan_mode(false);
            console->print(
                "Build mode: I can write files and run commands (with "
                "confirmation).\n");
            continue;
        }
        if (input == "/model") {
            console->print("Active model: " + cfg.model + "\n");
            continue;
        }
        if (input == "/project") {
            if (cfg.no_project) {
                console->print("Project awareness is disabled.\n");
            } else {
                std::ifstream nf(cfg.notes_path);
                console->print("Project root: " + cfg.project_root + "\n" +
                               "Notes file:   " + cfg.notes_path + "\n" +
                               "Notes:        " +
                               (nf.good() ? "present" : "not created yet") +
                               "\n");
            }
            continue;
        }
        if (input == "/learn") {
            if (cfg.no_project) {
                console->print("Project awareness is disabled.\n");
                continue;
            }
            agent.handle(
                "Study this project so you can maintain its notes. Use list_dir "
                "and read_file to inspect the layout and key files (README, "
                "build files, main entry points), then call the remember tool "
                "with a concise PROJECT.md covering: what the project is, how to "
                "build and run it, the key files/modules, and important "
                "conventions or gotchas.");
            continue;
        }
        if (input == "/reset") {
            convo.reset();
            console->print("Conversation cleared.\n");
            continue;
        }
        if (input == "/undo") {
            auto msg = undo.undo();
            console->print(msg ? (*msg + "\n") : "Nothing to undo.\n");
            continue;
        }
        if (input == "/compact") {
            size_t n = convo.compact();
            console->print(n ? ("Compacted " + std::to_string(n) +
                                " turn(s) into the running summary.\n")
                             : "Nothing to compact yet.\n");
            continue;
        }
        if (input == "/yolo") {
            bool on = !agent.yolo();
            agent.set_yolo(on);
            console->print(on ? "Auto-accept ON — tools run without "
                                "confirmation. /yolo again to turn off.\n"
                              : "Auto-accept OFF — tools ask for confirmation "
                                "again.\n");
            continue;
        }

        // Custom slash commands from .local_code/commands/*.md. Reaching here
        // means it's a '/...' that isn't a built-in, so resolve or report it
        // rather than sending the literal command to the model.
        if (input[0] == '/') {
            std::string name = input.substr(1), args;
            if (size_t sp = name.find_first_of(" \t"); sp != std::string::npos) {
                args = name.substr(sp + 1);
                name = name.substr(0, sp);
            }
            auto it = commands.find(name);
            if (it == commands.end()) {
                console->print("Unknown command: " + input +
                               "  (/help for the list)\n");
                continue;
            }
            std::string prompt_text = it->second;
            if (size_t pos = prompt_text.find("$ARGS");
                pos != std::string::npos)
                prompt_text.replace(pos, 5, args);
            else if (!args.empty())
                prompt_text += "\n\n" + args;
            agent.handle(prompt_text);
            continue;
        }

        agent.handle(input);
    }

    console->print("Bye.\n");
    return 0;
}
