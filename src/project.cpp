#include "project.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace lc {

namespace {
constexpr size_t kNotesLoadCap = 3 * 1024;   // notes injected into context
constexpr size_t kNotesSaveCap = 8 * 1024;   // notes written to disk
constexpr int kTreeMaxEntries = 40;
constexpr int kTreeMaxDepth = 3;

// Directories never worth showing the model.
bool skip_dir(const std::string& name) {
    static const std::array<const char*, 11> kSkip{
        ".git",   ".local_code", "build",  "node_modules", "dist",  "target",
        ".venv",  "__pycache__", ".idea",  ".vscode",      ".cache"};
    for (const char* s : kSkip)
        if (name == s) return true;
    return false;
}
}  // namespace

Project::Project(const std::string& root) {
    std::error_code ec;
    fs::path p = root.empty() ? fs::current_path(ec) : fs::path(root);
    fs::path abs = fs::absolute(p, ec);
    root_ = ec ? p.string() : abs.lexically_normal().string();
}

std::string Project::notes_path() const {
    return (fs::path(root_) / ".local_code" / "PROJECT.md").string();
}

std::string Project::load_notes() const {
    std::ifstream f(notes_path(), std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() > kNotesLoadCap) {
        s.resize(kNotesLoadCap);
        s += "\n... [notes truncated]";
    }
    return s;
}

bool Project::save_notes(const std::string& text) const {
    std::error_code ec;
    fs::create_directories(fs::path(root_) / ".local_code", ec);
    std::ofstream f(notes_path(), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text.substr(0, kNotesSaveCap);
    return true;
}

std::string Project::tree() const {
    std::ostringstream ss;
    int shown = 0;
    bool truncated = false;
    const fs::path base(root_);

    std::error_code ec;
    fs::recursive_directory_iterator it(
        base, fs::directory_options::skip_permission_denied, ec);
    if (ec) return "(unable to read project directory)";
    fs::recursive_directory_iterator end;

    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path& path = it->path();
        const std::string name = path.filename().string();

        bool is_dir = it->is_directory(ec);
        if (is_dir && skip_dir(name)) {
            it.disable_recursion_pending();
            continue;
        }
        if (it.depth() >= kTreeMaxDepth && is_dir)
            it.disable_recursion_pending();
        if (name.empty()) continue;

        if (shown >= kTreeMaxEntries) { truncated = true; break; }
        std::string rel = fs::relative(path, base, ec).string();
        if (rel.empty()) continue;
        ss << "  " << rel << (is_dir ? "/" : "") << "\n";
        ++shown;
    }
    if (truncated) ss << "  ... (more files; truncated)\n";
    std::string out = ss.str();
    return out.empty() ? "  (empty)\n" : out;
}

std::string Project::context_block() const {
    std::ostringstream ss;
    ss << "PROJECT\n"
       << "Root: " << root_ << "\n"
       << "All files here and in subfolders belong to this one program.\n"
       << "Layout:\n"
       << tree();
    std::string notes = load_notes();
    ss << "Project notes (.local_code/PROJECT.md):\n";
    if (notes.empty())
        ss << "  (none yet — run /learn or use the remember tool to build "
              "them)\n";
    else
        ss << notes << "\n";
    return ss.str();
}

}  // namespace lc
