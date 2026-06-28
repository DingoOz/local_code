// Tests for the Claude Code-inspired feature batch: edit_file/search-tool
// mapping + schema gating, the diff preview, the permission allowlist, and the
// undo stack. Filesystem tests use unique paths under the temp dir.
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "../src/config.hpp"
#include "../src/diff.hpp"
#include "../src/permissions.hpp"
#include "../src/tools.hpp"
#include "../src/undo.hpp"

using namespace lc;
using json = nlohmann::json;
namespace fs = std::filesystem;

static bool advertises(const std::string& schema_json, const std::string& tool) {
    json arr = json::parse(schema_json, nullptr, false);
    assert(!arr.is_discarded() && arr.is_array());
    for (const auto& e : arr)
        if (e["function"]["name"] == tool) return true;
    return false;
}

static std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main() {
    // --- edit_file / search-tool argument mapping ---
    auto e = tool_call_from_args(
        "edit_file",
        "{\"path\":\"a.cpp\",\"old_string\":\"foo\",\"new_string\":\"bar\"}");
    assert(e && e->name == "edit_file" && e->path == "a.cpp" &&
           e->old_str == "foo" && e->new_str == "bar");

    auto ff = tool_call_from_args("find_files", "{\"pattern\":\"*.hpp\"}");
    assert(ff && ff->pattern == "*.hpp");
    auto sc = tool_call_from_args(
        "search_code", "{\"pattern\":\"TODO\",\"path\":\"src\"}");
    assert(sc && sc->pattern == "TODO" && sc->path == "src");

    // --- schema gating ---
    Config base;  // build mode
    std::string s = tool_schemas_json(base);
    assert(advertises(s, "edit_file") && advertises(s, "find_files") &&
           advertises(s, "search_code") && advertises(s, "write_file"));
    Config plan = base;
    plan.plan_mode = true;
    std::string ps = tool_schemas_json(plan);
    assert(advertises(ps, "find_files") && advertises(ps, "search_code"));
    assert(!advertises(ps, "edit_file") && !advertises(ps, "write_file"));

    // --- diff preview ---
    std::string d = preview_diff("line1\nline2\nline3\n", "line1\nCHANGED\nline3\n");
    assert(d.find("- line2") != std::string::npos);
    assert(d.find("+ CHANGED") != std::string::npos);
    assert(preview_diff("same", "same").find("no textual change") !=
           std::string::npos);

    // --- permission allowlist persistence ---
    fs::path perms_path = fs::temp_directory_path() / "lc_test_perms.txt";
    fs::remove(perms_path);
    {
        PermissionStore p(perms_path.string());
        assert(!p.allowed("cmd:git"));
        p.add("cmd:git");
        assert(p.allowed("cmd:git"));
    }
    {
        PermissionStore p2(perms_path.string());  // reload from disk
        assert(p2.allowed("cmd:git"));
        assert(!p2.allowed("cmd:rm"));
    }
    fs::remove(perms_path);

    // --- undo: restore an overwritten file ---
    fs::path bdir = fs::temp_directory_path() / "lc_test_backups";
    fs::remove_all(bdir);
    fs::path f1 = fs::temp_directory_path() / "lc_test_edit.txt";
    { std::ofstream o(f1); o << "ORIGINAL"; }
    {
        UndoStack u(bdir.string());
        u.snapshot(f1.string());
        { std::ofstream o(f1, std::ios::trunc); o << "MODIFIED"; }
        assert(read_file(f1.string()) == "MODIFIED");
        auto msg = u.undo();
        assert(msg.has_value());
        assert(read_file(f1.string()) == "ORIGINAL");
        assert(u.empty());
    }

    // --- undo: a newly created file is removed ---
    fs::path f2 = fs::temp_directory_path() / "lc_test_new.txt";
    fs::remove(f2);
    {
        UndoStack u(bdir.string());
        u.snapshot(f2.string());                 // did not exist
        { std::ofstream o(f2); o << "NEW"; }      // create it
        assert(fs::exists(f2));
        u.undo();
        assert(!fs::exists(f2));                  // undo removes it
    }
    fs::remove(f1);
    fs::remove_all(bdir);

    std::cout << "FEATURE TESTS PASS\n";
    return 0;
}
