#include "undo.hpp"

#include <filesystem>

namespace fs = std::filesystem;

namespace lc {

UndoStack::UndoStack(std::string backups_dir) : dir_(std::move(backups_dir)) {}

void UndoStack::snapshot(const std::string& path) {
    std::error_code ec;
    Entry e;
    e.path = path;
    e.existed = fs::exists(path, ec) && fs::is_regular_file(path, ec);
    if (e.existed) {
        fs::create_directories(dir_, ec);
        e.backup = dir_ + "/" + std::to_string(counter_++) + ".bak";
        fs::copy_file(path, e.backup,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) e.backup.clear();  // copy failed; undo will report it
    }
    stack_.push_back(std::move(e));
}

std::optional<std::string> UndoStack::undo() {
    if (stack_.empty()) return std::nullopt;
    Entry e = std::move(stack_.back());
    stack_.pop_back();

    std::error_code ec;
    if (e.existed) {
        if (e.backup.empty())
            return "Cannot undo '" + e.path + "': no backup was saved.";
        fs::copy_file(e.backup, e.path,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) return "Failed to restore '" + e.path + "': " + ec.message();
        fs::remove(e.backup, ec);
        return "Restored '" + e.path + "' to its previous contents.";
    }
    // The file was newly created by the change; undo removes it.
    fs::remove(e.path, ec);
    if (ec) return "Failed to remove '" + e.path + "': " + ec.message();
    return "Removed '" + e.path + "' (it was newly created).";
}

}  // namespace lc
