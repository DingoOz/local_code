#include "tools.hpp"

#include <fnmatch.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "diff.hpp"
#include "permissions.hpp"
#include "undo.hpp"
#include "web_search.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace lc {

namespace {

// Repair the JSON-escaping mistakes weak models routinely make inside string
// literals: raw newlines/tabs/CRs that should be escaped, and backslash line-
// continuations (a '\' followed by a real newline). Valid JSON passes through
// unchanged, so this is safe to run before every parse attempt.
std::string sanitize_json(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    bool in_str = false;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (!in_str) {
            out += c;
            if (c == '"') in_str = true;
            continue;
        }
        if (c == '"') { out += c; in_str = false; continue; }
        if (c == '\\') {
            char n = (i + 1 < in.size()) ? in[i + 1] : '\0';
            switch (n) {
                case '"': case '\\': case '/': case 'b': case 'f':
                case 'n': case 'r': case 't': case 'u':
                    out += '\\'; out += n; ++i; break;   // valid escape, keep
                case '\n': out += "\\n"; ++i; break;     // line continuation
                case '\r': out += "\\n"; ++i; break;
                default:   out += "\\\\"; break;         // stray '\' -> escape it
            }
            continue;
        }
        switch (c) {                                     // raw control chars
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c;
        }
    }
    return out;
}

// Quote bare/unquoted object keys, e.g. {query: "x"} -> {"query": "x"}. Some
// models emit JS-ish objects without quoting keys; valid JSON is unchanged.
std::string quote_bare_keys(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    bool in_str = false, esc = false;
    for (size_t i = 0; i < in.size();) {
        char c = in[i];
        if (in_str) {
            out += c;
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            ++i;
            continue;
        }
        if (c == '"') { in_str = true; out += c; ++i; continue; }
        if (c == '{' || c == ',') {
            out += c;
            ++i;
            size_t j = i;
            while (j < in.size() && std::isspace((unsigned char)in[j])) ++j;
            size_t s = j;
            while (j < in.size() &&
                   (std::isalnum((unsigned char)in[j]) || in[j] == '_'))
                ++j;
            size_t e = j;
            size_t m = j;
            while (m < in.size() && std::isspace((unsigned char)in[m])) ++m;
            if (e > s && m < in.size() && in[m] == ':') {
                out.append(in, i, s - i);  // whitespace before the key
                out += '"';
                out.append(in, s, e - s);  // the bare key
                out += '"';
                i = e;
            }
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

const char* const kToolNames[] = {
    "read_file",   "write_file",  "edit_file",   "list_dir", "run_command",
    "ask_user",    "web_search",  "remember",    "find_files", "search_code"};

bool is_tool_name(const std::string& s) {
    for (const char* n : kToolNames)
        if (s == n) return true;
    return false;
}

// Scan [from, to) for the first brace-balanced {...} that parses as an object.
// Tolerant of trailing junk (e.g. an extra "}") because it stops at the matching
// close brace, and of bad escapes via sanitize_json. Does NOT require a "name"
// key — the object may be just the tool's args.
std::optional<json> scan_object(const std::string& text, size_t from,
                                size_t to) {
    for (size_t i = from; i < to; ++i) {
        if (text[i] != '{') continue;
        int depth = 0;
        bool in_str = false, esc = false;
        for (size_t j = i; j < to; ++j) {
            char c = text[j];
            if (in_str) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') in_str = false;
            } else if (c == '"') {
                in_str = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}') {
                if (--depth == 0) {
                    std::string cand = text.substr(i, j - i + 1);
                    std::string san = sanitize_json(cand);
                    for (const std::string& v :
                         {cand, san, quote_bare_keys(san)}) {
                        json o = json::parse(v, nullptr, false);
                        if (!o.is_discarded() && o.is_object()) return o;
                    }
                    break;  // unbalanced/invalid; try the next '{'
                }
            }
        }
    }
    return std::nullopt;
}

// Extract the body of the first ``` fence at or after `start` (skipping its
// optional language-tag line). Used for write_file content, which models supply
// in a separate fence rather than as a fragile JSON string.
std::optional<std::string> extract_fenced_content(const std::string& text,
                                                  size_t start) {
    size_t open = text.find("```", start);
    if (open == std::string::npos) return std::nullopt;
    size_t line_end = text.find('\n', open + 3);
    if (line_end == std::string::npos) return std::nullopt;
    size_t body = line_end + 1;
    size_t close = text.find("```", body);
    if (close == std::string::npos) return std::nullopt;
    std::string content = text.substr(body, close - body);
    if (!content.empty() && content.back() == '\n') content.pop_back();
    return content;
}

// First known tool name appearing as a token in [from, to) — used when a model
// puts the name on its own line ("write_file\n{...}") instead of inside the JSON.
std::string scan_tool_name(const std::string& text, size_t from, size_t to) {
    size_t best = std::string::npos;
    std::string found;
    for (const char* n : kToolNames) {
        size_t p = text.find(n, from);
        if (p != std::string::npos && p < to && p < best) {
            best = p;
            found = n;
        }
    }
    return found;
}

// Populate a ToolCall's typed fields from an args object. Shared by the text
// parser and the native function-calling path so both behave identically.
ToolCall build_tool_call(const std::string& name, const json& args) {
    ToolCall c;
    c.name = name;
    c.raw_args_json = args.dump();
    auto str = [&](const char* k) -> std::string {
        return args.contains(k) && args[k].is_string()
                   ? args[k].get<std::string>()
                   : std::string();
    };
    c.path = str("path");
    c.content = str("content");
    c.cmd = str("cmd");
    c.question = str("question");
    c.query = str("query");
    c.notes = str("notes");
    c.old_str = str("old_string");
    c.new_str = str("new_string");
    c.pattern = str("pattern");
    return c;
}

std::string truncate(std::string s, size_t cap) {
    if (s.size() > cap) {
        s.resize(cap);
        s += "\n... [truncated]";
    }
    return s;
}

ToolResult do_read(const ToolCall& c, const Config& cfg) {
    std::ifstream f(c.path, std::ios::binary);
    if (!f) return {false, "Error: cannot open '" + c.path + "'"};
    std::ostringstream ss;
    ss << f.rdbuf();
    return {true, truncate(ss.str(), cfg.max_read_bytes)};
}

ToolResult do_list(const ToolCall& c) {
    std::string p = c.path.empty() ? "." : c.path;
    std::error_code ec;
    if (!fs::exists(p, ec)) return {false, "Error: no such path '" + p + "'"};
    std::ostringstream ss;
    for (const auto& e : fs::directory_iterator(p, ec)) {
        ss << (e.is_directory(ec) ? "[dir]  " : "       ")
           << e.path().filename().string() << "\n";
    }
    if (ec) return {false, "Error listing '" + p + "': " + ec.message()};
    std::string out = ss.str();
    return {true, out.empty() ? "(empty directory)" : out};
}

// Read a whole file into a string (empty if it doesn't exist / unreadable).
std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// First whitespace-delimited token of a command, for the "cmd:<tool>" rule.
std::string first_token(const std::string& cmd) {
    size_t i = cmd.find_first_not_of(" \t");
    if (i == std::string::npos) return {};
    size_t j = cmd.find_first_of(" \t", i);
    return cmd.substr(i, j == std::string::npos ? std::string::npos : j - i);
}

// Gate a mutating action: approved if --yolo, a stored allowlist rule matches,
// or the user confirms. Answering "always" records the rule for next time.
bool approve(ToolCtx& ctx, const std::string& prompt, const std::string& rule) {
    if (ctx.cfg.yolo || ctx.perms.allowed(rule)) return true;
    Confirm d = ctx.console.confirm(prompt, /*allow_always=*/!rule.empty());
    if (d == Confirm::Always) ctx.perms.add(rule);
    return d != Confirm::No;
}

ToolResult do_write(const ToolCall& c, ToolCtx& ctx) {
    const std::string current = slurp(c.path);
    ctx.console.print("\n\033[36m--- write_file: " + c.path + " (" +
                      std::to_string(c.content.size()) + " bytes) ---\033[0m\n");
    ctx.console.print(preview_diff(current, c.content));
    ctx.console.print("\033[36m----------------------------\033[0m\n");
    if (!approve(ctx, "Write this file?", "write"))
        return {false, "User declined the write."};

    ctx.undo.snapshot(c.path);  // checkpoint before overwriting
    std::error_code ec;
    fs::path pp(c.path);
    if (pp.has_parent_path()) fs::create_directories(pp.parent_path(), ec);
    std::ofstream f(c.path, std::ios::binary | std::ios::trunc);
    if (!f) return {false, "Error: cannot write '" + c.path + "'"};
    f << c.content;
    return {true, "Wrote " + std::to_string(c.content.size()) +
                      " bytes to " + c.path + " (/undo to revert)"};
}

ToolResult do_edit(const ToolCall& c, ToolCtx& ctx) {
    if (c.old_str.empty())
        return {false,
                "edit_file needs a non-empty 'old_string' to locate the text "
                "to replace."};
    std::ifstream in(c.path, std::ios::binary);
    if (!in)
        return {false, "Error: cannot open '" + c.path +
                           "'. Use write_file to create a new file."};
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string content = ss.str();

    const size_t first = content.find(c.old_str);
    if (first == std::string::npos)
        return {false, "edit_file: old_string not found in '" + c.path +
                           "'. Read the file and copy the exact text "
                           "(including whitespace)."};
    if (content.find(c.old_str, first + c.old_str.size()) != std::string::npos)
        return {false, "edit_file: old_string is not unique in '" + c.path +
                           "' (appears multiple times). Add more surrounding "
                           "context so it matches exactly once."};

    const std::string updated = content.substr(0, first) + c.new_str +
                                content.substr(first + c.old_str.size());
    ctx.console.print("\n\033[36m--- edit_file: " + c.path + " ---\033[0m\n");
    ctx.console.print(preview_diff(content, updated));
    ctx.console.print("\033[36m-------------------------\033[0m\n");
    if (!approve(ctx, "Apply this edit?", "write"))
        return {false, "User declined the edit."};

    ctx.undo.snapshot(c.path);
    std::ofstream out(c.path, std::ios::binary | std::ios::trunc);
    if (!out) return {false, "Error: cannot write '" + c.path + "'"};
    out << updated;
    return {true, "Edited '" + c.path + "' (1 replacement). /undo to revert."};
}

ToolResult do_ask(const ToolCall& c, Console& console) {
    std::string q = c.question.empty() ? "(the agent asked a question)"
                                       : c.question;
    console.print("\n\033[35m[agent asks]\033[0m " + q + "\n");
    auto line = console.input("\033[35myour answer>\033[0m ");
    if (!line) return {false, "User gave no answer (input closed)."};
    return {true, "User answered: " + *line};
}

ToolResult do_run(const ToolCall& c, ToolCtx& ctx) {
    ctx.console.print("\n\033[36m--- run_command ---\033[0m\n\033[1m$ " + c.cmd +
                      "\033[0m\n\033[36m-------------------\033[0m\n");
    const std::string tok = first_token(c.cmd);
    if (!approve(ctx, "Run this command?",
                 tok.empty() ? std::string("cmd") : "cmd:" + tok))
        return {false, "User declined the command."};

    // Merge stderr into stdout so the model sees errors too.
    std::string full = "( " + c.cmd + " ) 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) return {false, "Error: failed to start command"};
    std::string out;
    std::array<char, 4096> buf;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0)
        out.append(buf.data(), n);
    int rc = pclose(pipe);
    int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    std::string result = "exit code: " + std::to_string(code) + "\n" +
                         truncate(out, ctx.cfg.max_cmd_output);
    return {code == 0, result};
}

// Translate a relative path for display: strip a leading "./".
std::string rel_display(const fs::path& p) {
    std::string s = p.lexically_normal().string();
    if (s.rfind("./", 0) == 0) s = s.substr(2);
    return s;
}

// Directories we never descend into for find_files / search_code.
bool skip_dir(const std::string& name) {
    return name == ".git" || name == ".local_code" || name == "build" ||
           name == "node_modules" || name == ".cache" || name == ".venv";
}

ToolResult do_find(const ToolCall& c) {
    if (c.pattern.empty())
        return {false, "find_files needs a glob 'pattern', e.g. \"*.cpp\"."};
    std::error_code ec;
    std::vector<std::string> hits;
    constexpr size_t kCap = 200;
    fs::recursive_directory_iterator it(
        ".", fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end && hits.size() < kCap; it.increment(ec)) {
        if (ec) break;
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (skip_dir(name)) it.disable_recursion_pending();
            continue;
        }
        const std::string rel = rel_display(it->path());
        if (fnmatch(c.pattern.c_str(), rel.c_str(), 0) == 0 ||
            fnmatch(c.pattern.c_str(), name.c_str(), 0) == 0)
            hits.push_back(rel);
    }
    if (hits.empty()) return {true, "No files match: " + c.pattern};
    std::ostringstream ss;
    ss << hits.size() << (hits.size() >= kCap ? "+" : "") << " file(s) matching "
       << c.pattern << ":\n";
    for (const auto& h : hits) ss << "  " << h << "\n";
    return {true, ss.str()};
}

ToolResult do_grep(const ToolCall& c) {
    if (c.pattern.empty())
        return {false, "search_code needs a 'pattern' to search for."};
    const std::string root = c.path.empty() ? "." : c.path;
    std::error_code ec;
    if (!fs::exists(root, ec))
        return {false, "Error: no such path '" + root + "'"};

    // Prefer regex; fall back to a literal substring if the pattern won't compile.
    std::regex re;
    bool use_re = true;
    try {
        re = std::regex(c.pattern, std::regex::ECMAScript);
    } catch (...) {
        use_re = false;
    }

    std::ostringstream ss;
    int matches = 0;
    constexpr int kMaxMatches = 100;
    constexpr std::uintmax_t kMaxFileBytes = 512 * 1024;
    auto scan = [&](const fs::path& p) {
        if (matches >= kMaxMatches) return;
        std::error_code e2;
        if (fs::file_size(p, e2) > kMaxFileBytes || e2) return;
        std::ifstream f(p, std::ios::binary);
        if (!f) return;
        const std::string rel = rel_display(p);
        std::string line;
        int ln = 0;
        while (std::getline(f, line) && matches < kMaxMatches) {
            ++ln;
            const bool hit = use_re ? std::regex_search(line, re)
                                    : line.find(c.pattern) != std::string::npos;
            if (hit) {
                ss << rel << ":" << ln << ": " << truncate(line, 200) << "\n";
                ++matches;
            }
        }
    };

    if (fs::is_regular_file(root, ec)) {
        scan(root);
    } else {
        fs::recursive_directory_iterator it(
            root, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end && matches < kMaxMatches; it.increment(ec)) {
            if (ec) break;
            const std::string name = it->path().filename().string();
            if (it->is_directory(ec)) {
                if (skip_dir(name)) it.disable_recursion_pending();
                continue;
            }
            scan(it->path());
        }
    }
    if (matches == 0) return {true, "No matches for: " + c.pattern};
    std::string out = ss.str();
    if (matches >= kMaxMatches) out += "... [more matches truncated]\n";
    return {true, out};
}

ToolResult do_web_search(const ToolCall& c, const Config& cfg) {
    if (!cfg.web_enabled)
        return {false,
                "web_search is unavailable (no local SearXNG configured)."};
    if (c.query.empty()) return {false, "web_search needs a 'query'."};

    auto results = web_search(cfg.searxng_url, c.query, 5);
    if (!results)
        return {false, "web_search failed: SearXNG unreachable at " +
                           cfg.searxng_url};
    if (results->empty()) return {true, "No web results for: " + c.query};

    std::ostringstream ss;
    ss << "Top web results for \"" << c.query << "\":\n";
    int i = 1;
    for (const auto& r : *results) {
        ss << i++ << ". " << r.title << "\n   " << r.url << "\n";
        if (!r.content.empty()) ss << "   " << truncate(r.content, 240) << "\n";
    }
    return {true, ss.str()};
}

ToolResult do_remember(const ToolCall& c, ToolCtx& ctx) {
    const Config& cfg = ctx.cfg;
    if (cfg.no_project || cfg.notes_path.empty())
        return {false, "remember is unavailable (project awareness disabled)."};
    if (c.notes.empty()) return {false, "remember needs 'notes' content."};

    ctx.undo.snapshot(cfg.notes_path);  // checkpoint so /undo can revert notes
    std::error_code ec;
    fs::path p(cfg.notes_path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(cfg.notes_path, std::ios::binary | std::ios::trunc);
    if (!f) return {false, "Error: cannot write project notes '" +
                               cfg.notes_path + "'"};
    f << c.notes;
    return {true, "Saved " + std::to_string(c.notes.size()) +
                      " bytes to project notes (.local_code/PROJECT.md)."};
}

}  // namespace

std::optional<ToolCall> parse_tool_call(const std::string& assistant_text) {
    const std::string& text = assistant_text;

    // Search inside a ```tool fence when present (low false-positive), else the
    // whole message.
    size_t from = 0, to = text.size();
    bool fenced = false;
    if (size_t f = text.find("```tool"); f != std::string::npos) {
        from = f + 7;
        size_t e = text.find("```", from);
        to = (e == std::string::npos) ? text.size() : e;
        fenced = true;
    }

    auto obj = scan_object(text, from, to);

    // Resolve the tool name and the args object across the formats models emit:
    //   A) {"name":"write_file","args":{...}}     (spec)
    //   B) {"name":"write_file","path":...}        (flat, name + fields)
    //   C) write_file\n{"path":...}                (name on its own line)
    std::string name;
    json args = json::object();
    if (obj) {
        if (obj->contains("name") && (*obj)["name"].is_string()) {
            name = (*obj)["name"].get<std::string>();
            args = obj->contains("args") && (*obj)["args"].is_object()
                       ? (*obj)["args"]
                       : *obj;  // flat
        } else {
            args = *obj;  // object is the args; name comes from a bare token
        }
    }
    // Bare-name fallback when we have a complete args object (e.g. name on its
    // own line). Requires obj so a half-streamed call can't match early.
    if (name.empty() && obj) name = scan_tool_name(text, from, to);

    // Salvage a malformed/empty tool JSON (e.g. {"notes":}) ONLY for the
    // fence-based tools and ONLY once their ```file fence has actually arrived —
    // which never happens mid-stream, so early-stop stays correct.
    if (name.empty() && fenced) {
        std::string tok = scan_tool_name(text, from, to);
        if (tok == "write_file" || tok == "remember") {
            size_t after = (to < text.size()) ? to + 3 : text.size();
            if (extract_fenced_content(text, after)) name = tok;
        }
    }

    if (!is_tool_name(name)) return std::nullopt;

    ToolCall c = build_tool_call(name, args);

    // For write_file, the body normally arrives in a separate ```file fence
    // after the tool block (far more reliable than a JSON-escaped string). Use
    // it unless the model already inlined "content" in the JSON.
    if (c.name == "write_file" && c.content.empty()) {
        size_t after = (to < text.size()) ? to + 3 : text.size();
        if (auto fc = extract_fenced_content(text, after)) c.content = *fc;
    }
    // remember likewise accepts its (often multi-line) notes from a fence,
    // which avoids the JSON-escaping fragility of long content in a string.
    if (c.name == "remember" && c.notes.empty()) {
        size_t after = (to < text.size()) ? to + 3 : text.size();
        if (auto fc = extract_fenced_content(text, after)) c.notes = *fc;
    }
    return c;
}

std::optional<ToolCall> tool_call_from_args(const std::string& name,
                                            const std::string& arguments_json) {
    if (!is_tool_name(name)) return std::nullopt;
    // Ollama sends function.arguments as a JSON object; tolerate a no-arg call
    // arriving as "", "{}", or null by treating it as an empty object.
    json args = json::parse(arguments_json, nullptr, /*allow_exceptions=*/false);
    if (args.is_discarded() || !args.is_object()) args = json::object();
    return build_tool_call(name, args);
}

std::string tool_schemas_json(const Config& cfg) {
    // One function-schema entry: {type:function, function:{name,description,
    // parameters:<JSON Schema>}}. `required` lists the mandatory params.
    auto fn = [](const char* name, const char* desc,
                 std::initializer_list<std::pair<const char*, const char*>> props,
                 std::initializer_list<const char*> required) {
        json params;
        params["type"] = "object";
        params["properties"] = json::object();
        for (const auto& p : props) {
            params["properties"][p.first] = {{"type", "string"},
                                             {"description", p.second}};
        }
        json req = json::array();
        for (const char* r : required) req.push_back(r);
        params["required"] = std::move(req);
        return json{{"type", "function"},
                    {"function",
                     {{"name", name},
                      {"description", desc},
                      {"parameters", std::move(params)}}}};
    };

    json tools = json::array();
    tools.push_back(fn("read_file", "Read a file and return its contents.",
                       {{"path", "Relative path to the file."}}, {"path"}));
    tools.push_back(fn("list_dir",
                       "List the entries of a directory (defaults to '.').",
                       {{"path", "Directory path; omit for the current dir."}},
                       {}));
    tools.push_back(fn("ask_user",
                       "Ask the user a clarifying question and wait for their "
                       "answer.",
                       {{"question", "The question to ask."}}, {"question"}));
    tools.push_back(fn("find_files",
                       "Find files under the project by glob pattern "
                       "(e.g. \"*.cpp\", \"src/*.hpp\").",
                       {{"pattern", "A glob pattern matched against paths."}},
                       {"pattern"}));
    tools.push_back(fn("search_code",
                       "Search file contents for a regex (literal substring if "
                       "it isn't valid regex). Returns path:line: text.",
                       {{"pattern", "Regex or text to search for."},
                        {"path", "Optional file/dir to limit the search."}},
                       {"pattern"}));

    // Mutating tools are hidden in planning mode (the runtime would reject them).
    if (!cfg.plan_mode) {
        tools.push_back(fn("write_file",
                           "Create or overwrite a whole file. Prefer edit_file "
                           "for changes to an existing file. Asks to confirm.",
                           {{"path", "Relative path to write."},
                            {"content", "The exact file contents."}},
                           {"path", "content"}));
        tools.push_back(fn("edit_file",
                           "Replace an exact, unique snippet in an existing "
                           "file. old_string must occur exactly once. Cheaper "
                           "and safer than rewriting the whole file.",
                           {{"path", "Relative path to edit."},
                            {"old_string", "Exact text to replace (unique)."},
                            {"new_string", "Replacement text."}},
                           {"path", "old_string", "new_string"}));
        tools.push_back(fn("run_command",
                           "Run a shell command and return its combined "
                           "stdout/stderr. The user is asked to confirm.",
                           {{"cmd", "The shell command to run."}}, {"cmd"}));
    }

    if (cfg.web_enabled) {
        tools.push_back(fn("web_search",
                           "Search the web via a local SearXNG instance for "
                           "current or external information.",
                           {{"query", "The search query."}}, {"query"}));
    }

    // remember writes notes, so it is also gated out of planning mode.
    if (!cfg.plan_mode && !cfg.no_project && !cfg.notes_path.empty()) {
        tools.push_back(fn("remember",
                           "Save durable project knowledge (architecture, build "
                           "commands, conventions, gotchas) to "
                           ".local_code/PROJECT.md, reloaded next session.",
                           {{"notes", "The notes content (Markdown)."}},
                           {"notes"}));
    }

    return tools.dump();
}

ToolResult execute_tool(const ToolCall& call, ToolCtx& ctx) {
    const Config& cfg = ctx.cfg;
    // In planning mode, mutating tools are disabled — the model should describe
    // the step in its plan instead of performing it.
    if (cfg.plan_mode &&
        (call.name == "write_file" || call.name == "edit_file" ||
         call.name == "run_command" || call.name == "remember")) {
        return {false,
                "Planning mode: '" + call.name +
                    "' is disabled. Do not modify anything yet — describe this "
                    "step in your plan, or use ask_user to clarify."};
    }

    if (call.name == "read_file")   return do_read(call, cfg);
    if (call.name == "list_dir")    return do_list(call);
    if (call.name == "find_files")  return do_find(call);
    if (call.name == "search_code") return do_grep(call);
    if (call.name == "write_file")  return do_write(call, ctx);
    if (call.name == "edit_file")   return do_edit(call, ctx);
    if (call.name == "run_command") return do_run(call, ctx);
    if (call.name == "ask_user")    return do_ask(call, ctx.console);
    if (call.name == "web_search")  return do_web_search(call, cfg);
    if (call.name == "remember")    return do_remember(call, ctx);
    return {false, "Error: unknown tool '" + call.name +
                       "'. Valid tools: read_file, list_dir, find_files, "
                       "search_code, write_file, edit_file, run_command, "
                       "ask_user, web_search, remember."};
}

}  // namespace lc
