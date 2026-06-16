#pragma once

#include <string>

namespace lc {

// Represents the single software project the agent is working on: the directory
// it was launched in (or --project DIR), plus a persistent knowledge file at
// <root>/.local_code/PROJECT.md that carries understanding across sessions.
class Project {
public:
    // root empty => use the current working directory.
    explicit Project(const std::string& root = "");

    const std::string& root() const { return root_; }
    std::string notes_path() const;        // <root>/.local_code/PROJECT.md

    std::string load_notes() const;        // contents (capped), or "" if absent
    bool save_notes(const std::string& text) const;  // creates .local_code/

    // A compact, filtered, depth-limited relative-path listing of the project.
    std::string tree() const;

    // The dynamic preamble appended to the system prompt: root, layout, notes.
    std::string context_block() const;

private:
    std::string root_;
};

}  // namespace lc
