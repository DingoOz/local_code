#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
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
#include "plain_console.hpp"
#include "system_prompt.hpp"
#include "tui.hpp"
#include "web_search.hpp"

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
            return client.chat(model, req, nullptr);
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
    "  /reset   clear the conversation (keeps system prompt)\n"
    "  /model   show the active model\n"
    "  /quit    exit\n"
    "Anything else is sent to the agent.\n";

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    bool exit_now = false;
    if (!Config::parse(argc, argv, cfg, exit_now)) return 2;
    if (exit_now) return 0;

    OllamaClient client(cfg.host);

    try {
        cfg.model = pick_model(client, cfg.model);
    } catch (const std::exception& e) {
        std::cerr << "Startup error: " << e.what() << "\n";
        return 1;
    }

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

    Agent agent(client, convo, cfg, build_prompt, plan_prompt, *console);

    console->print(std::string("\nlocal_code — agent on '") + cfg.model +
                   "' (budget " + std::to_string(cfg.budget_tokens) + " tok" +
                   (cfg.yolo ? ", yolo" : "") +
                   (agent.plan_mode() ? ", PLAN" : "") +
                   (cfg.web_enabled ? ", web" : "") +
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
        if (input == "/reset") {
            convo.reset();
            console->print("Conversation cleared.\n");
            continue;
        }

        agent.handle(input);
    }

    console->print("Bye.\n");
    return 0;
}
