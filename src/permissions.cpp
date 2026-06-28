#include "permissions.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace lc {

PermissionStore::PermissionStore(std::string path) : path_(std::move(path)) {
    std::ifstream f(path_);
    std::string line;
    while (std::getline(f, line)) {
        // Trim trailing CR/whitespace; skip blanks and comments.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty() && line[0] != '#') rules_.insert(line);
    }
}

bool PermissionStore::allowed(const std::string& rule) const {
    return !rule.empty() && rules_.count(rule) > 0;
}

void PermissionStore::add(const std::string& rule) {
    if (rule.empty() || !rules_.insert(rule).second) return;  // already present
    save();
}

void PermissionStore::save() const {
    std::error_code ec;
    fs::path p(path_);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(path_, std::ios::trunc);
    if (!f) return;  // best-effort; allowlist stays in-memory if unwritable
    f << "# local_code approved actions (\"always allow\"). One rule per line.\n";
    for (const auto& r : rules_) f << r << "\n";
}

}  // namespace lc
