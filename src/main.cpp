#include <readline/history.h>
#include <readline/readline.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "agent.hpp"
#include "config.hpp"
#include "conversation.hpp"
#include "ollama_client.hpp"
#include "system_prompt.hpp"

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

// Interactive model picker when --model is not supplied.
std::string pick_model(OllamaClient& client, const std::string& preset) {
    std::vector<std::string> models = client.list_models();
    if (models.empty())
        throw std::runtime_error("No Ollama models found. Pull one first.");

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

void print_help() {
    std::cout <<
        "Commands:\n"
        "  /help    show this help\n"
        "  /plan    enter planning mode (design & ask questions, no changes)\n"
        "  /build   enter build mode (can write files / run commands)\n"
        "  /reset   clear the conversation (keeps system prompt)\n"
        "  /model   show the active model\n"
        "  /quit    exit\n"
        "Anything else is sent to the agent.\n";
}

std::string history_path() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.local_code_history";
}

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

    const std::string build_prompt = load_system_prompt(cfg);
    const std::string plan_prompt = kPlanSystemPrompt;
    const std::string initial_prompt =
        cfg.plan_mode ? plan_prompt : build_prompt;

    Conversation convo(initial_prompt, cfg.budget_tokens);
    convo.set_summarizer(make_summarizer(client, cfg.model));
    Agent agent(client, convo, cfg, build_prompt, plan_prompt);

    std::cout << "\nlocal_code — agent on '" << cfg.model
              << "' (budget " << cfg.budget_tokens << " tok"
              << (cfg.yolo ? ", yolo" : "")
              << (agent.plan_mode() ? ", PLAN" : "")
              << "). /help for commands.\n";

    const std::string hist = history_path();
    read_history(hist.c_str());

    while (true) {
        const char* prompt = agent.plan_mode()
                                 ? "\n\033[35myou (plan)>\033[0m "
                                 : "\n\033[32myou>\033[0m ";
        char* raw = readline(prompt);
        if (!raw) break;  // EOF / Ctrl-D
        std::string input(raw);
        free(raw);

        // Trim trailing whitespace.
        while (!input.empty() && (input.back() == ' ' || input.back() == '\n'))
            input.pop_back();
        if (input.empty()) continue;

        add_history(input.c_str());

        if (input == "/quit" || input == "/exit") break;
        if (input == "/help") { print_help(); continue; }
        if (input == "/plan") {
            agent.set_plan_mode(true);
            std::cout << "Planning mode: I'll design the approach and may ask "
                         "questions. No files written, no commands run.\n";
            continue;
        }
        if (input == "/build") {
            agent.set_plan_mode(false);
            std::cout << "Build mode: I can write files and run commands "
                         "(with confirmation).\n";
            continue;
        }
        if (input == "/model") {
            std::cout << "Active model: " << cfg.model << "\n";
            continue;
        }
        if (input == "/reset") {
            convo.reset();
            std::cout << "Conversation cleared.\n";
            continue;
        }

        agent.handle(input);
    }

    write_history(hist.c_str());
    std::cout << "Bye.\n";
    return 0;
}
