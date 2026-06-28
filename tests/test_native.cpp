// Native function-calling surface: building a ToolCall from a model's
// function.arguments object, and the mode-aware tools[] schema advertised to the
// model. These cover the Ornith path that complements the text-protocol parser.
#include <cassert>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "../src/config.hpp"
#include "../src/tools.hpp"

using namespace lc;
using json = nlohmann::json;

namespace {

// True if the schema array names a function `tool`.
bool advertises(const std::string& schema_json, const std::string& tool) {
    json arr = json::parse(schema_json, nullptr, false);
    assert(!arr.is_discarded() && arr.is_array());  // must be valid JSON array
    for (const auto& e : arr) {
        if (e.contains("function") && e["function"].contains("name") &&
            e["function"]["name"] == tool)
            return true;
    }
    return false;
}

}  // namespace

int main() {
    // --- tool_call_from_args: maps native arguments to the typed ToolCall. ---
    auto r = tool_call_from_args("read_file", "{\"path\":\"a.txt\"}");
    assert(r && r->name == "read_file" && r->path == "a.txt");

    auto w = tool_call_from_args(
        "write_file", "{\"path\":\"x.py\",\"content\":\"print(1)\"}");
    assert(w && w->name == "write_file" && w->path == "x.py" &&
           w->content == "print(1)");

    auto cmd = tool_call_from_args("run_command", "{\"cmd\":\"ls -a\"}");
    assert(cmd && cmd->cmd == "ls -a");

    auto q = tool_call_from_args("web_search", "{\"query\":\"qt version\"}");
    assert(q && q->query == "qt version");

    // No-arg call arriving as an empty string is tolerated (=> empty object).
    auto ld = tool_call_from_args("list_dir", "");
    assert(ld && ld->name == "list_dir" && ld->path.empty());

    // Unknown tool name => not a call.
    assert(!tool_call_from_args("frobnicate", "{}"));

    // --- tool_schemas_json: valid JSON, gated by mode/web/project. ---
    Config base;  // defaults: build mode, no web, project off (notes_path empty)
    std::string s = tool_schemas_json(base);
    assert(advertises(s, "read_file") && advertises(s, "list_dir") &&
           advertises(s, "ask_user"));
    assert(advertises(s, "write_file") && advertises(s, "run_command"));
    assert(!advertises(s, "web_search"));  // web not enabled
    assert(!advertises(s, "remember"));    // project off

    // Planning mode hides the mutating tools.
    Config plan = base;
    plan.plan_mode = true;
    std::string ps = tool_schemas_json(plan);
    assert(advertises(ps, "read_file") && advertises(ps, "list_dir") &&
           advertises(ps, "ask_user"));
    assert(!advertises(ps, "write_file") && !advertises(ps, "run_command"));

    // web_search appears only when web is enabled.
    Config web = base;
    web.web_enabled = true;
    assert(advertises(tool_schemas_json(web), "web_search"));

    // remember appears only when project awareness is on (notes_path set).
    Config proj = base;
    proj.no_project = false;
    proj.notes_path = "/tmp/.local_code/PROJECT.md";
    assert(advertises(tool_schemas_json(proj), "remember"));
    // ...and is still hidden in planning mode even with a project.
    proj.plan_mode = true;
    assert(!advertises(tool_schemas_json(proj), "remember"));

    std::cout << "NATIVE TESTS PASS\n";
    return 0;
}
