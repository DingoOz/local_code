#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lc {

// Session-scoped checkpoint stack: before a tool overwrites or creates a file,
// snapshot() copies the current version aside (or records that it did not
// exist). undo() reverses the most recent change — restoring the old contents,
// or deleting a file that was freshly created. No sandbox, so this is the safety
// net for write_file / edit_file.
class UndoStack {
public:
    // backups_dir holds the saved copies (created on first use).
    explicit UndoStack(std::string backups_dir);

    // Record the current state of `path` before it is modified.
    void snapshot(const std::string& path);

    // Reverse the most recent snapshot. Returns a human-readable message, or
    // nullopt if there is nothing to undo (or the restore failed).
    std::optional<std::string> undo();

    bool empty() const { return stack_.empty(); }

private:
    struct Entry {
        std::string path;     // the file that was modified
        std::string backup;   // saved copy (empty if the file did not exist)
        bool existed;         // whether `path` existed before the change
    };

    std::string dir_;
    int counter_ = 0;
    std::vector<Entry> stack_;
};

}  // namespace lc
