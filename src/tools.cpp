#include "tools.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

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

const char* const kToolNames[] = {"read_file",   "write_file", "list_dir",
                                  "run_command", "ask_user",   "web_search",
                                  "remember"};

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

ToolResult do_write(const ToolCall& c, const Config& cfg, Console& console) {
    console.print("\n\033[36m--- write_file: " + c.path + " (" +
                  std::to_string(c.content.size()) + " bytes) ---\033[0m\n");
    console.print(truncate(c.content, 1200));
    console.print("\n\033[36m----------------------------\033[0m\n");
    if (!cfg.yolo && !console.confirm("Write this file?"))
        return {false, "User declined the write."};

    std::error_code ec;
    fs::path pp(c.path);
    if (pp.has_parent_path()) fs::create_directories(pp.parent_path(), ec);
    std::ofstream f(c.path, std::ios::binary | std::ios::trunc);
    if (!f) return {false, "Error: cannot write '" + c.path + "'"};
    f << c.content;
    return {true, "Wrote " + std::to_string(c.content.size()) +
                      " bytes to " + c.path};
}

ToolResult do_ask(const ToolCall& c, Console& console) {
    std::string q = c.question.empty() ? "(the agent asked a question)"
                                       : c.question;
    console.print("\n\033[35m[agent asks]\033[0m " + q + "\n");
    auto line = console.input("\033[35myour answer>\033[0m ");
    if (!line) return {false, "User gave no answer (input closed)."};
    return {true, "User answered: " + *line};
}

ToolResult do_run(const ToolCall& c, const Config& cfg, Console& console) {
    console.print("\n\033[36m--- run_command ---\033[0m\n\033[1m$ " + c.cmd +
                  "\033[0m\n\033[36m-------------------\033[0m\n");
    if (!cfg.yolo && !console.confirm("Run this command?"))
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
                         truncate(out, cfg.max_cmd_output);
    return {code == 0, result};
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

ToolResult do_remember(const ToolCall& c, const Config& cfg) {
    if (cfg.no_project || cfg.notes_path.empty())
        return {false, "remember is unavailable (project awareness disabled)."};
    if (c.notes.empty()) return {false, "remember needs 'notes' content."};

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

ToolResult execute_tool(const ToolCall& call, const Config& cfg,
                        Console& console) {
    // In planning mode, file-writing tools are disabled — the model should
    // describe the step in its plan instead of performing it.
    if (cfg.plan_mode &&
        (call.name == "write_file" || call.name == "run_command" ||
         call.name == "remember")) {
        return {false,
                "Planning mode: '" + call.name +
                    "' is disabled. Do not modify anything yet — describe this "
                    "step in your plan, or use ask_user to clarify."};
    }

    if (call.name == "read_file")   return do_read(call, cfg);
    if (call.name == "list_dir")    return do_list(call);
    if (call.name == "write_file")  return do_write(call, cfg, console);
    if (call.name == "run_command") return do_run(call, cfg, console);
    if (call.name == "ask_user")    return do_ask(call, console);
    if (call.name == "web_search")  return do_web_search(call, cfg);
    if (call.name == "remember")    return do_remember(call, cfg);
    return {false, "Error: unknown tool '" + call.name +
                       "'. Valid tools: read_file, list_dir, write_file, "
                       "run_command, ask_user, web_search, remember."};
}

}  // namespace lc
