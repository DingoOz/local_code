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
};

struct ToolResult {
    bool ok = false;
    std::string output;  // fed back to the model as a tool message
};

// Scan an assistant message for the first ```tool ... ``` fenced JSON block and
// parse it. Returns nullopt if no well-formed tool block is present (=> the
// message is a final answer to the user).
std::optional<ToolCall> parse_tool_call(const std::string& assistant_text);

// Execute a tool. write_file / run_command prompt for y/N confirmation unless
// cfg.yolo is set. Output is truncated per cfg caps. Previews and prompts are
// rendered through `console`.
ToolResult execute_tool(const ToolCall& call, const Config& cfg,
                        Console& console);

}  // namespace lc
