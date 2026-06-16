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
