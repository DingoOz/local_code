#pragma once

#include <string>

namespace lc {

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
    // looks like a blank reply. When on, reasoning is streamed too.
    bool think = false;

    // Disable the ncurses TUI even on a terminal (use the plain stream).
    bool no_tui = false;

    // Safety / sizing caps.
    int max_tool_turns = 12;            // consecutive tool calls before user check-in
    size_t max_read_bytes = 16 * 1024;  // truncate large file reads
    size_t max_cmd_output = 16 * 1024;  // truncate command output

    // Returns false and prints usage if args are invalid; sets *exit_now on --help.
    static bool parse(int argc, char** argv, Config& out, bool& exit_now);
    static void print_usage(const char* argv0);
};

}  // namespace lc
