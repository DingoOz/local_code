// Parser robustness: tool blocks must survive the messy output real local
// models emit (channel markers, extra braces, missing language tag, prose).
#include <cassert>
#include <iostream>

#include "../src/tools.hpp"

using namespace lc;

int main() {
    // Extra trailing brace inside a ```tool fence (observed model output).
    auto a = parse_tool_call(
        "<|channel>thought\n```tool\n"
        "{\"name\":\"ask_user\",\"args\":{\"question\":\"CLI or loop?\"}}}\n```");
    assert(a && a->name == "ask_user" && a->question == "CLI or loop?");

    // Plain prose with no tool block => final answer.
    assert(!parse_tool_call("Here is my plan: do X then Y."));

    // Bare object, no fence, surrounded by prose.
    auto b = parse_tool_call(
        "sure -> {\"name\":\"read_file\",\"args\":{\"path\":\"a.txt\"}} done");
    assert(b && b->name == "read_file" && b->path == "a.txt");

    // write_file args extracted.
    auto c = parse_tool_call(
        "```tool\n{\"name\":\"write_file\",\"args\":"
        "{\"path\":\"x.py\",\"content\":\"print(1)\"}}\n```");
    assert(c && c->name == "write_file" && c->path == "x.py" &&
           c->content == "print(1)");

    // Invalid JSON escape: backslash + literal newline (line continuation) and
    // a raw literal newline inside the content string. Must be repaired.
    std::string bad =
        "```tool\n{\"name\":\"write_file\",\"args\":{\"path\":\"/tmp/f.py\","
        "\"content\":\"def f():\\\n    return 1\nprint(f())\"}}\n```";
    auto d = parse_tool_call(bad);
    assert(d && d->name == "write_file" && d->path == "/tmp/f.py");
    assert(d->content.find("def f():") != std::string::npos);
    assert(d->content.find("return 1") != std::string::npos);

    // Format C: tool name on its own line, JSON is just the args (observed from
    // gemma4:e4b with thinking off).
    auto e = parse_tool_call(
        "```tool\nwrite_file\n{\"path\":\"hello.c\","
        "\"content\":\"#include <stdio.h>\"}\n```");
    assert(e && e->name == "write_file" && e->path == "hello.c" &&
           e->content == "#include <stdio.h>");

    // Format B: flat object, name alongside fields (no "args" wrapper).
    auto g = parse_tool_call(
        "{\"name\":\"run_command\",\"cmd\":\"gcc hello.c -o hello\"}");
    assert(g && g->name == "run_command" && g->cmd == "gcc hello.c -o hello");

    // A tool name merely mentioned in prose (no JSON) must NOT trigger a call.
    assert(!parse_tool_call("I will use write_file to create the program."));

    // write_file with content in a separate ```file fence (the robust path):
    // path-only JSON, body supplied unescaped in the fence.
    auto h = parse_tool_call(
        "```tool\n{\"name\":\"write_file\",\"args\":{\"path\":\"hello.c\"}}\n```\n"
        "```file\n#include <stdio.h>\nint main(){ return 0; }\n```");
    assert(h && h->name == "write_file" && h->path == "hello.c");
    assert(h->content == "#include <stdio.h>\nint main(){ return 0; }");

    // Inline JSON content still wins when present (back-compat).
    auto k = parse_tool_call(
        "```tool\n{\"name\":\"write_file\",\"args\":"
        "{\"path\":\"a.txt\",\"content\":\"hi\"}}\n```\n```file\nIGNORED\n```");
    assert(k && k->content == "hi");

    // Malformed call the gemma coder model emits: unquoted key, no name/args
    // wrapper, a "call:" prefix and a trailing marker. Must still parse.
    auto m = parse_tool_call(
        "call:web_search{query: \"latest version of Qt\"}<tool_call|>");
    assert(m && m->name == "web_search" &&
           m->query == "latest version of Qt");

    // Unquoted keys inside a normal wrapped call.
    auto p = parse_tool_call(
        "```tool\n{name: \"run_command\", args: {cmd: \"ls -a\"}}\n```");
    assert(p && p->name == "run_command" && p->cmd == "ls -a");

    // remember with empty args + notes in a ```file fence.
    auto rm = parse_tool_call(
        "```tool\n{\"name\":\"remember\",\"args\":{}}\n```\n"
        "```file\n# Notes\nbuilds with make\n```");
    assert(rm && rm->name == "remember" &&
           rm->notes == "# Notes\nbuilds with make");

    // Malformed JSON ({"notes":}) salvaged via the fence because we're inside a
    // ```tool fence and the tool name is present.
    auto rm2 = parse_tool_call(
        "```tool\n{\"name\":\"remember\",\"args\":{\"notes\":}}\n```\n"
        "```file\nhello notes\n```");
    assert(rm2 && rm2->name == "remember" && rm2->notes == "hello notes");

    std::cout << "PARSE TESTS PASS\n";
    return 0;
}
