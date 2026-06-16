#include "agent.hpp"

#include "stream_filter.hpp"
#include "tools.hpp"

namespace lc {

namespace {

// Show a concise, indented preview of a tool result so the user can see what
// happened — especially failures (compiler errors, non-zero exits) — without
// dumping the full contents of a large file read back to the screen.
void show_result(const ToolCall& call, const ToolResult& res, Console& con) {
    con.print(std::string("\033[90m") + (res.ok ? "ok" : "failed") +
              "\033[0m\n");

    if (call.name == "read_file" && res.ok) {
        con.print("\033[90m  (" + std::to_string(res.output.size()) +
                  " bytes read)\033[0m\n");
        return;
    }
    if (res.output.empty()) return;

    constexpr size_t kMaxLines = 15, kMaxChars = 1200;
    const std::string body = res.output.substr(0, kMaxChars);
    size_t shown = 0, pos = 0;
    while (pos < body.size() && shown < kMaxLines) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();
        con.print("\033[90m  " + body.substr(pos, nl - pos) + "\033[0m\n");
        pos = nl + 1;
        ++shown;
    }
    if (pos < res.output.size() || res.output.size() > kMaxChars)
        con.print("\033[90m  ... [truncated]\033[0m\n");
}

}  // namespace

Agent::Agent(OllamaClient& client, Conversation& convo, Config cfg,
             std::string build_prompt, std::string plan_prompt,
             Console& console)
    : client_(client),
      convo_(convo),
      cfg_(std::move(cfg)),
      build_prompt_(std::move(build_prompt)),
      plan_prompt_(std::move(plan_prompt)),
      console_(console) {}

void Agent::set_plan_mode(bool plan) {
    cfg_.plan_mode = plan;
    convo_.set_system_prompt(plan ? plan_prompt_ : build_prompt_);
}

void Agent::handle(const std::string& user_input) {
    convo_.add(Role::User, user_input);

    std::string last_sig;   // detect a model stuck repeating one tool call
    int empty_retries = 0;  // some local models emit a blank turn mid-task
    for (int turn = 0; turn < cfg_.max_tool_turns; ++turn) {
        std::vector<Message> window = convo_.window();

        console_.print("\n\033[36m" + cfg_.model + ":\033[0m ");

        // Strip leaked special-token markers as the reply streams; keep the
        // cleaned text for history + tool parsing.
        MarkerFilter filter;
        std::string reply;
        try {
            client_.chat(cfg_.model, window,
                         [&](const std::string& piece) {
                             std::string vis = filter.feed(piece);
                             console_.print(vis);
                             reply += vis;
                         },
                         cfg_.think);
        } catch (const std::exception& e) {
            console_.print(std::string("\n\033[31m[error] ") + e.what() +
                           "\033[0m\n");
            return;
        }
        std::string tail = filter.flush();
        console_.print(tail + "\n");
        reply += tail;

        // Some local models emit a blank turn (e.g. a lone stray marker) mid-
        // task instead of continuing. Nudge them a couple of times rather than
        // silently handing control back.
        if (reply.find_first_not_of(" \t\r\n") == std::string::npos) {
            if (++empty_retries > 2) {
                console_.print(
                    "\033[33m[the model went quiet — type to continue]\033[0m\n");
                return;
            }
            convo_.add(Role::User, "Continue.");
            continue;
        }
        empty_retries = 0;
        convo_.add(Role::Assistant, reply);

        auto call = parse_tool_call(reply);
        if (!call) return;  // plain answer => user's turn

        // Break out if the model just repeated the exact same tool call — a
        // weak model retrying a failing command would otherwise loop forever.
        // Checked before executing so a side-effecting command isn't re-run.
        const std::string sig = call->name + "\n" + call->raw_args_json;
        if (sig == last_sig) {
            console_.print("\033[33m[the model repeated the same " + call->name +
                           " call without making progress — stopping. Tell it "
                           "how to proceed.]\033[0m\n");
            return;
        }
        last_sig = sig;

        console_.print("\033[33m[tool] " + call->name + "\033[0m\n");
        ToolResult res = execute_tool(*call, cfg_, console_);
        show_result(*call, res, console_);

        // Feed the observation back as a tool message and continue the loop.
        convo_.add(Role::Tool, "[" + call->name + " result]\n" + res.output);
    }

    console_.print("\033[33m[stopped after " +
                   std::to_string(cfg_.max_tool_turns) +
                   " tool calls — type to continue]\033[0m\n");
}

}  // namespace lc
