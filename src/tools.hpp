#pragma once

#include <optional>
#include <string>

#include "config.hpp"
#include "io.hpp"

namespace lc {

// A tool invocation parsed out of an assistant message.
struct ToolCall {
    std::string name;
    std::string raw_args_json;  // the {...} object, for echoing on errors
    // Extracted fields (only the relevant ones are populated per tool).
    std::string path;
    std::string content;
    std::string cmd;
    std::string question;
    std::string query;
    std::string notes;
    std::string old_str;  // edit_file: text to replace
    std::string new_str;  // edit_file: replacement text
    std::string pattern;  // find_files / search_code: glob or regex
};

struct ToolResult {
    bool ok = false;
    std::string output;  // fed back to the model as a tool message
};

class PermissionStore;
class UndoStack;

// Everything a tool needs to run: config, the I/O surface, the persistent
// allowlist (for "always allow"), and the undo stack (checkpoints before
// writes). Bundled so execute_tool's signature stays stable as tools grow.
struct ToolCtx {
    const Config& cfg;
    Console& console;
    PermissionStore& perms;
    UndoStack& undo;
};

// Scan an assistant message for the first ```tool ... ``` fenced JSON block and
// parse it. Returns nullopt if no well-formed tool block is present (=> the
// message is a final answer to the user). This is the fallback path for models
// that emit the text protocol instead of native function calls.
std::optional<ToolCall> parse_tool_call(const std::string& assistant_text);

// Build a ToolCall from a native function call: the tool name plus its arguments
// as a JSON object string (the function.arguments from a tool_calls entry).
// Returns nullopt if `name` isn't a known tool or `arguments_json` isn't an
// object. Shares the same field extraction as parse_tool_call.
std::optional<ToolCall> tool_call_from_args(const std::string& name,
                                            const std::string& arguments_json);

// The tools[] schema advertised to the model on /api/chat, as a JSON array
// string. Mode-aware, mirroring the prompt/runtime gating: planning mode exposes
// only read_file/list_dir/ask_user; web_search appears only when cfg.web_enabled;
// remember only when project awareness is on.
std::string tool_schemas_json(const Config& cfg);

// Execute a tool. Mutating tools (write_file / edit_file / run_command /
// remember) prompt for y/N/a confirmation unless cfg.yolo or a stored allowlist
// rule applies, and snapshot files to the undo stack first. Output is truncated
// per cfg caps. Previews and prompts are rendered through ctx.console.
ToolResult execute_tool(const ToolCall& call, ToolCtx& ctx);

}  // namespace lc
