#pragma once

#include "config.hpp"
#include "conversation.hpp"
#include "io.hpp"
#include "ollama_client.hpp"
#include "permissions.hpp"
#include "undo.hpp"

namespace lc {

// Drives one user turn to completion: repeatedly query the model, execute any
// tool it requests, feed the result back, and stop when the model produces a
// plain-prose answer (or the tool-turn cap is hit).
class Agent {
public:
    Agent(OllamaClient& client, Conversation& convo, Config cfg,
          std::string build_prompt, std::string plan_prompt, Console& console,
          PermissionStore& perms, UndoStack& undo);

    // Handles a single user message end-to-end. Streams output to stdout.
    void handle(const std::string& user_input);

    // Toggle planning mode: swaps the system prompt and gates mutating tools.
    void set_plan_mode(bool plan);
    bool plan_mode() const { return cfg_.plan_mode; }

    // Runtime auto-accept toggle (/yolo): skip confirmations until turned off.
    void set_yolo(bool on) { cfg_.yolo = on; }
    bool yolo() const { return cfg_.yolo; }

private:
    OllamaClient& client_;
    Conversation& convo_;
    Config cfg_;
    std::string build_prompt_;
    std::string plan_prompt_;
    Console& console_;
    PermissionStore& perms_;
    UndoStack& undo_;
};

}  // namespace lc
