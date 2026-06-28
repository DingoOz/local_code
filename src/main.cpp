#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
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

// Interactive model picker when --model is not supplied. May set cfg.fit_gpu
// when the user picks the GPU-fit shortcut. Uses cfg.model as the preset.
std::string pick_model(OllamaClient& client, Config& cfg) {
    const std::string preset = cfg.model;
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

    // Detect an installed Ornith model to back the shortcut entries; fall back
    // to the canonical name if none is installed.
    std::string ornith = kGpuFitModel;
    for (const auto& m : models) {
        std::string low = m;
        for (char& c : low) c = static_cast<char>(std::tolower((unsigned char)c));
        if (low.find("ornith") != std::string::npos) { ornith = m; break; }
    }

    std::cout << "Available models:\n";
    for (size_t i = 0; i < models.size(); ++i)
        std::cout << "  " << (i + 1) << ") " << models[i] << "\n";
    std::cout << "Shortcuts:\n"
                 "  d) default        — Ornith (" << ornith
              << "), native 256K context\n"
                 "  g) ornith-gpu-fit — Ornith (" << ornith
              << "), 40K context to fit an 8 GB GPU\n"
                 "  q) ornith-gpu-fit-large — Ornith (" << ornith
              << "), 64K context via a q8_0 KV cache (reconfigures Ollama, sudo)\n"
                 "(tip: Ornith auto-enables reasoning + recommended sampling.)\n";

    while (true) {
        std::cout << "Select [1-" << models.size() << ", d, g, q]: "
                  << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) std::exit(0);
        size_t b = line.find_first_not_of(" \t");
        size_t e = line.find_last_not_of(" \t\r\n");
        std::string sel =
            b == std::string::npos ? "" : line.substr(b, e - b + 1);
        if (sel == "d" || sel == "D") { cfg.fit_gpu = false; return ornith; }
        if (sel == "g" || sel == "G") { cfg.fit_gpu = true;  return ornith; }
        if (sel == "q" || sel == "Q") {
            cfg.fit_gpu = true;
            cfg.kv_cache = "q8_0";
            return ornith;
        }
        try {
            size_t idx = std::stoul(sel);
            if (idx >= 1 && idx <= models.size()) return models[idx - 1];
        } catch (...) {
        }
        std::cout << "Invalid choice.\n";
    }
}

// Run a shell command, capturing combined stdout+stderr. Returns the exit
// status (0 == success), or -1 if the command could not be launched.
int run_capture(const std::string& cmd, std::string& out) {
    out.clear();
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    if (!p) return -1;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    int rc = pclose(p);
    return rc == -1 ? -1 : WEXITSTATUS(rc);
}

// Apply a server-side Ollama KV cache type via a systemd drop-in + service
// restart (needs sudo). `type` is q8_0/q4_0 to enable a quantized cache, or f16
// to revert (removes the drop-in). Idempotent: skips the restart when the
// drop-in already requests `type`. Runs before the TUI so sudo can prompt on the
// terminal. Returns true on success (or when already applied / nothing to do).
bool apply_kv_cache_setting(const Config& cfg, const std::string& type) {
    const std::string conf =
        "/etc/systemd/system/ollama.service.d/local_code-kv.conf";
    const bool revert = (type == "f16");

    // Read the current drop-in (world-readable under /etc) to decide if work is
    // needed.
    std::string cur;
    {
        std::ifstream f(conf);
        std::ostringstream ss;
        ss << f.rdbuf();
        cur = ss.str();
    }
    if (revert) {
        if (cur.empty()) return true;  // nothing to revert
    } else if (cur.find("OLLAMA_KV_CACHE_TYPE=" + type) != std::string::npos) {
        std::cout << "Ollama KV cache already set to " << type << ".\n";
        return true;
    }

    std::cout << (revert ? "Reverting Ollama to the default fp16 KV cache"
                         : "Configuring Ollama for a " + type + " KV cache")
              << " (needs sudo; restarts the Ollama service)...\n";
    std::string cmd;
    if (revert) {
        cmd = "sudo rm -f " + conf +
              " && sudo systemctl daemon-reload && sudo systemctl restart ollama";
    } else {
        cmd =
            "sudo mkdir -p /etc/systemd/system/ollama.service.d && "
            "printf '[Service]\\nEnvironment=\"OLLAMA_FLASH_ATTENTION=1\"\\n"
            "Environment=\"OLLAMA_KV_CACHE_TYPE=" + type + "\"\\n' | "
            "sudo tee " + conf + " >/dev/null && "
            "sudo systemctl daemon-reload && sudo systemctl restart ollama";
    }
    std::string out;
    if (run_capture(cmd, out) != 0) {
        std::cout << "Failed to reconfigure Ollama:\n  " << out
                  << "(Is it a systemd service? Try: systemctl status ollama)\n";
        return false;
    }
    // Wait for the API to come back after the restart.
    const std::string probe =
        "curl -fsS " + cfg.host + "/api/tags -o /dev/null";
    for (int i = 0; i < 30; ++i) {
        std::string p;
        if (run_capture(probe, p) == 0) {
            std::cout << "Ollama restarted ("
                      << (revert ? "fp16" : type) << " KV cache).\n";
            return true;
        }
        ::sleep(1);
    }
    std::cout << "Ollama did not answer after the restart (check its logs).\n";
    return false;
}

// Start the local SearXNG Docker container (the installer names it "searxng")
// and wait for its JSON API to answer. Streams progress to the console and
// returns true once reachable.
bool start_searxng(Console& console, const Config& cfg) {
    console.print("Starting SearXNG container (docker start searxng)...\n");
    std::string out;
    // Try without sudo first, then with — the installer may have used sudo.
    int rc = run_capture("docker start searxng", out);
    if (rc != 0) rc = run_capture("sudo docker start searxng", out);
    if (rc != 0) {
        console.print("Could not start the SearXNG container:\n  " + out +
                      "If it doesn't exist yet, run ./install.sh to create it.\n");
        return false;
    }
    console.print("Waiting for the JSON API at " + cfg.searxng_url + " ...\n");
    for (int i = 0; i < 30; ++i) {
        if (web_search_available(cfg.searxng_url)) return true;
        ::sleep(1);
    }
    console.print("SearXNG started but its JSON API didn't answer in time.\n"
                  "Check: docker logs searxng\n");
    return false;
}

// Interactive menu opened with /menu. Currently a single action: bring up the
// local SearXNG web-search server and enable the web_search tool for the agent.
void run_menu(Console& console, Config& cfg, Agent& agent) {
    while (true) {
        const bool web = agent.web_enabled();
        console.print(std::string("\n== Menu ==\n") +
                      "  1) Start SearXNG web-search server" +
                      (web ? "  [enabled]" : "") + "\n" +
                      "  0) Close menu\n");
        auto choice = console.input("menu> ");
        if (!choice) return;  // EOF
        std::string c = *choice;
        while (!c.empty() && (c.back() == ' ' || c.back() == '\n')) c.pop_back();
        if (c.empty() || c == "0" || c == "q") return;
        if (c == "1") {
            if (agent.web_enabled()) {
                console.print("Web search is already enabled.\n");
                continue;
            }
            if (start_searxng(console, cfg)) {
                cfg.web_enabled = true;
                agent.set_web_enabled(true);
                console.print("SearXNG is up — web_search is now enabled.\n");
            } else {
                console.print("web_search stays off.\n");
            }
            continue;
        }
        console.print("Unknown choice: " + c + "\n");
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
    "  /menu    open the menu (start the SearXNG web-search server) — also F2\n"
    "  (scroll the conversation with PgUp/PgDn, the mouse wheel, or Home/End)\n"
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
        cfg.model = pick_model(client, cfg);
    } catch (const std::exception& e) {
        std::cerr << "Startup error: " << e.what() << "\n";
        return 1;
    }

    // Apply a server-side quantized KV cache if requested (before sizing the
    // context, since success enlarges the GPU-fit window). On failure, fall back
    // to fp16 so the larger context isn't loaded onto a fp16 cache.
    if (!cfg.kv_cache.empty()) {
        const bool ok = apply_kv_cache_setting(cfg, cfg.kv_cache);
        if (cfg.kv_cache == "f16") {
            cfg.kv_cache.clear();  // f16 == server default; nothing to flag
        } else if (!ok) {
            std::cerr << "Falling back to the fp16 KV cache (smaller GPU "
                         "context).\n";
            cfg.kv_cache.clear();
        }
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
                   (!cfg.kv_cache.empty() ? ", kv " + cfg.kv_cache : "") +
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
        if (input == "/menu") { run_menu(*console, cfg, agent); continue; }
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
