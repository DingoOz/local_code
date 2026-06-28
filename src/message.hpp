#pragma once

#include <string>

namespace lc {

// Chat roles understood by Ollama's /api/chat. We model tool results as a
// "tool" role; ollama_client maps it onto the API ("tool" is accepted, and we
// fall back to a labelled user message if a model dislikes it).
enum class Role { System, User, Assistant, Tool };

struct Message {
    Role role;
    std::string content;

    // Native function-calling round-trip (Ollama /api/chat). Populated only when
    // structured tool-calling is in play; empty for the text-protocol path.
    //   - On an Assistant message: the verbatim JSON array of tool_calls the
    //     model emitted, so the model sees its own calls echoed back next turn.
    //   - On a Tool message: the name of the tool this result is for (Ollama
    //     accepts "tool_name" on tool-role messages).
    std::string tool_calls_json{};  // assistant only
    std::string tool_name{};        // tool result only
};

inline const char* role_to_api(Role r) {
    switch (r) {
        case Role::System:    return "system";
        case Role::User:      return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool:      return "tool";
    }
    return "user";
}

}  // namespace lc
