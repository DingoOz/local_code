#include "agent.hpp"

#include <chrono>

#include "stream_filter.hpp"
#include "system_prompt.hpp"
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
             Console& console, PermissionStore& perms, UndoStack& undo)
    : client_(client),
      convo_(convo),
      cfg_(std::move(cfg)),
      build_prompt_(std::move(build_prompt)),
      plan_prompt_(std::move(plan_prompt)),
      console_(console),
      perms_(perms),
      undo_(undo) {}

void Agent::set_plan_mode(bool plan) {
    cfg_.plan_mode = plan;
    convo_.set_system_prompt(plan ? plan_prompt_ : build_prompt_);
}

void Agent::set_web_enabled(bool on) {
    if (on == cfg_.web_enabled) return;  // no change
    cfg_.web_enabled = on;
    if (on) {
        // Web was off at startup, so the web tool line isn't in the prompts yet;
        // append it (to both modes) and refresh the live system prompt so the
        // model is told the web_search tool now exists.
        const std::string line = std::string("\n") + kWebToolLine;
        build_prompt_ += line;
        plan_prompt_ += line;
        convo_.set_system_prompt(cfg_.plan_mode ? plan_prompt_ : build_prompt_);
    }
}

void Agent::handle(const std::string& user_input) {
    convo_.add(Role::User, user_input);

    ToolCtx tctx{cfg_, console_, perms_, undo_};
    std::string last_sig;   // detect a model stuck repeating tool call(s)
    int empty_retries = 0;  // some local models emit a blank turn mid-task
    for (int turn = 0; turn < cfg_.max_tool_turns; ++turn) {
        std::vector<Message> window = convo_.window();

        // Context-window usage for the status bar (estimate vs. history budget).
        int used = 0;
        for (const auto& m : window)
            used += Conversation::estimate_tokens(m.content) + 4;
        double pct = cfg_.budget_tokens > 0
                         ? 100.0 * used / cfg_.budget_tokens
                         : 0.0;
        console_.set_ctx(pct > 100.0 ? 100.0 : pct);

        console_.print("\n\033[36m" + cfg_.model + ":\033[0m ");

        // Strip leaked special-token markers from the visible answer as it
        // streams. Reasoning (thinking) is shown dimmed and never persisted to
        // history — only `reply` (cleaned content) is.
        MarkerFilter filter;
        std::string reply;
        bool in_reasoning = false;
        std::string reason_buf;  // holds an incomplete trailing UTF-8 char
        auto flush_reason = [&]() {
            if (!reason_buf.empty()) { console_.print(reason_buf); reason_buf.clear(); }
        };
        auto end_reasoning = [&]() {
            if (in_reasoning) { flush_reason(); console_.print("\033[0m"); in_reasoning = false; }
        };
        // Live tokens/sec for the status bar: time from the first streamed
        // token (excludes prompt-eval latency) over the count of deltas, which
        // for Ollama's /api/chat stream is ~one token each.
        bool gen_started = false;
        std::chrono::steady_clock::time_point gen_t0;
        long gen_tokens = 0;
        auto on_token = [&](const std::string& piece, bool reasoning) {
            if (!gen_started) {
                gen_started = true;
                gen_t0 = std::chrono::steady_clock::now();
            }
            ++gen_tokens;
            double el = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - gen_t0)
                            .count();
            if (el > 0.05) console_.set_tps(gen_tokens / el);
            if (reasoning) {
                if (!in_reasoning) { console_.print("\033[90m"); in_reasoning = true; }
                reason_buf += piece;
                size_t hold = utf8_incomplete_suffix_len(reason_buf, reason_buf.size());
                console_.print(reason_buf.substr(0, reason_buf.size() - hold));
                reason_buf.erase(0, reason_buf.size() - hold);
                return;
            }
            end_reasoning();
            std::string vis = filter.feed(piece);
            console_.print(vis);
            reply += vis;
        };

        ChatOptions opts;
        opts.think = cfg_.think;
        opts.tools_json = tool_schemas_json(cfg_);  // mode-aware schema
        opts.temperature = cfg_.temperature;
        opts.top_p = cfg_.top_p;
        opts.top_k = cfg_.top_k;
        opts.num_ctx = cfg_.num_ctx;
        // Early-stop for the TEXT-PROTOCOL fallback: stop once a complete tool
        // block has arrived (native tool_calls terminate the stream on their
        // own). write_file/remember wait for their ```file fence to close.
        opts.stop_when = [](const std::string& full) -> bool {
            if (full.find('{') == std::string::npos) return false;
            auto c = parse_tool_call(full);
            if (!c) return false;
            if (c->name == "write_file" && c->content.empty()) return false;
            if (c->name == "remember" && c->notes.empty()) return false;
            return true;
        };

        ChatResult result;
        try {
            result = client_.chat(cfg_.model, window, on_token, opts);
        } catch (const std::exception& e) {
            end_reasoning();
            console_.print(std::string("\n\033[31m[error] ") + e.what() +
                           "\033[0m\n");
            return;
        }
        end_reasoning();
        std::string tail = filter.flush();
        console_.print(tail + "\n");
        reply += tail;

        // ---- Native function-calling path (Ornith and other tool-trained
        // models). Prefer it whenever structured tool_calls are present. ----
        if (!result.tool_calls.empty()) {
            empty_retries = 0;
            Message am{Role::Assistant, reply, result.raw_tool_calls_json, {}};
            convo_.add(std::move(am));

            // Stuck-loop guard over the whole batch of calls this turn.
            std::string sig;
            for (const auto& n : result.tool_calls)
                sig += n.name + "\n" + n.arguments_json + "\n";
            if (sig == last_sig) {
                console_.print(
                    "\033[33m[the model repeated the same tool call(s) without "
                    "making progress — stopping. Tell it how to proceed.]\033[0m\n");
                return;
            }
            last_sig = sig;

            for (const auto& n : result.tool_calls) {
                console_.print("\033[33m[tool] " + n.name + "\033[0m\n");
                auto call = tool_call_from_args(n.name, n.arguments_json);
                ToolResult res;
                if (!call) {
                    res = {false, "Error: unknown tool '" + n.name + "'."};
                    console_.print("\033[90mfailed\033[0m\n");
                } else {
                    res = execute_tool(*call, tctx);
                    show_result(*call, res, console_);
                }
                Message tm{Role::Tool, "[" + n.name + " result]\n" + res.output,
                           {}, n.name};
                convo_.add(std::move(tm));
            }
            continue;
        }

        // ---- Text-protocol fallback path (weak models that emit a ```tool
        // block instead of a native call). ----
        // Some local models emit a blank turn (whitespace, or a stray "```"
        // fence) mid-task. Nudge them a couple of times rather than silently
        // handing control back.
        if (reply.find_first_not_of(" \t\r\n`") == std::string::npos) {
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
        ToolResult res = execute_tool(*call, tctx);
        show_result(*call, res, console_);

        // Feed the observation back as a tool message and continue the loop.
        convo_.add(Role::Tool, "[" + call->name + " result]\n" + res.output);
    }

    console_.print("\033[33m[stopped after " +
                   std::to_string(cfg_.max_tool_turns) +
                   " tool calls — type to continue]\033[0m\n");
}

}  // namespace lc
