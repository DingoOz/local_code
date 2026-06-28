# local_code

A small terminal program that drives a **local LLM via Ollama** as an agentic
software developer. The model can read/write files and run shell commands
through a custom text protocol, with per-action confirmation. Designed for weak
local models: the system prompt is deliberately compact and the conversation
context is kept bounded by a sliding window plus automatic summarization.

## Requirements

- [Ollama](https://ollama.com) running locally (default `http://localhost:11434`)
- **A local GPU capable of running Ollama**, and enough VRAM for the model you
  pick. The recommended default is **`qwen2.5-coder:7b`** (~4.7 GB on disk, fits
  comfortably in **8 GB VRAM**) — it has clean, consistent tool-calling, which
  matters a lot for an agent. Other coder/instruct models work too; models with
  weak or inconsistent tool-calling (e.g. some community GGUF merges) produce
  malformed tool calls that the parser has to repair. CPU-only inference
  technically works but is too slow for interactive agentic use.
- C++17 compiler, CMake ≥ 3.16
- `libcurl` and `ncurses` development headers
  - Debian/Ubuntu: `sudo apt install libcurl4-openssl-dev libncurses-dev`
- `nlohmann/json` is vendored at `third_party/nlohmann/json.hpp` (no download
  needed)
- `nvidia-smi` (optional) — used for the GPU status bar; without it the bar
  shows "unavailable"

If no models are installed, the program offers on startup to download the
recommended coder model for you (via Ollama's pull API) so you can get going
without a separate `ollama pull`.

## Build

```sh
cmake -S . -B build
cmake --build build -j
```

Produces `build/local_code`.

### Quick install (Ubuntu/Debian)

```sh
./install.sh
```

Installs build dependencies, builds the binary, and **optionally** sets up web
search by running a local [SearXNG](https://github.com/searxng/searxng)
container (Docker) configured for JSON API access on `127.0.0.1:8888`.

## Usage

```sh
./build/local_code                 # pick a model from a menu at startup
./build/local_code --model qwen2.5-coder:7b
./build/local_code --budget 2048   # tighter context budget for weak models
./build/local_code --yolo          # skip y/N confirmation (dangerous)
./build/local_code --plan          # start in planning mode (no changes)
./build/local_code --gpu           # Ornith with a context that fits an 8 GB GPU
./build/local_code --system my_prompt.txt
```

### Options

| Flag | Meaning |
|------|---------|
| `--model NAME` | Model to use (default: choose at startup) |
| `--host URL` | Ollama host (default `http://localhost:11434`) |
| `--budget N` | History token budget (default 4096) |
| `--system FILE` | Override the built-in system prompt |
| `--yolo` | Auto-execute tools without confirmation |
| `--plan` | Start in planning mode (no writes/commands) |
| `--think` | Enable model "thinking" (off by default; auto-on for Ornith) |
| `--no-think` | Disable thinking (overrides the Ornith auto-enable) |
| `--temperature F` | Sampling temperature (Ornith default `0.6`) |
| `--top-p F` | Nucleus sampling `top_p` (Ornith default `0.95`) |
| `--top-k N` | Top-k sampling (Ornith default `20`) |
| `--num-ctx N` | Context window in tokens (Ornith defaults to its native `262144`) |
| `--gpu` | Start on Ornith with a context (`40960`) sized to stay fully on an 8 GB GPU; implies the Ornith model unless `--model` is given |
| `--kv-cache TYPE` | Ollama KV cache type: `q8_0`/`q4_0` packs a larger GPU context (Ornith ~`81920`), `f16` reverts. Reconfigures Ollama via systemd + sudo |
| `--no-tui` | Disable the ncurses TUI (plain output) |
| `--searxng URL` | SearXNG base URL (default `http://localhost:8888`) |
| `--web` | Force-enable web search (skip the probe) |
| `--no-web` | Disable web search |
| `--project DIR` | Project root (default: current directory) |
| `--no-project` | Disable project awareness / notes |

### REPL commands

`/help` `/plan` `/build` `/learn` `/project` `/undo` `/compact` `/yolo`
`/reset` `/model` `/quit` — plus any custom `/name` from
`.local_code/commands/` (see below).

### Interface

On a real terminal the program runs as a full-screen **ncurses TUI**: a border
around the screen, a scrolling conversation area, inline input with history
(↑/↓), and a **bottom status bar showing live GPU utilization, VRAM usage, and
generation speed (tokens/sec)** (GPU stats polled from `nvidia-smi` on a
background thread; tok/s measured live while the model streams). When stdin/stdout are piped
or redirected — or with `--no-tui` — it falls back to a plain line-based stream,
so scripting and non-interactive use still work.

### Web search (local SearXNG)

If a local [SearXNG](https://github.com/searxng/searxng) instance is reachable,
the agent gains a read-only `web_search` tool backed by SearXNG's JSON API — no
third-party API key, all local. On startup the app probes the configured URL
(default `http://localhost:8888`); when found, the banner shows `web` and the
tool is advertised to the model (in build *and* planning mode). Enable it the
easy way with `./install.sh`, or point at an existing instance with
`--searxng URL`. A refused connection is instant, so there's no startup penalty
when it isn't running.

### Ornith-1 (native tool-calling)

[Ornith-1](https://github.com/deepreinforce-ai/Ornith-1) is a family of agentic
coding models trained to emit **well-formed native function calls** and a
separate reasoning stream. Its 9B is published as **GGUF for Ollama**, so it runs
through the same local path as any other model:

```sh
ollama pull <ornith-1-gguf>        # whatever tag you imported it under
./build/local_code --model <ornith-1-gguf>
```

When the model name contains `ornith`, the app auto-enables thinking, applies
Ornith's recommended sampling (`temperature 0.6 / top_p 0.95 / top_k 20`), and
sets the context window to Ornith's native **256K** (`num_ctx 262144`) — for any
of those you didn't set explicitly. The startup banner shows `ctx 262144` and
`ornith-tuned`. Override any of them with `--temperature/--top-p/--top-k/--no-think`;
a 256K KV cache is large, so pass a smaller `--num-ctx` (e.g. `16384`) if it
doesn't fit your VRAM — or use `--gpu`, which selects Ornith and sets a context
(`40960`) measured to keep the 9B Q4_K_M weights fully on an 8 GB GPU.

> **Bigger window on the same GPU.** `--gpu` caps the *raw* context because the
> fp16 KV cache is what fills VRAM (~50K tokens is the spill cliff on 8 GB; ~1.3
> GB is held back by llama.cpp for compute buffers and can't be used for KV). To
> pack a **much larger** window into the same VRAM, quantize the KV cache with
> `--kv-cache q8_0` (≈ half the bytes/token → ~80K context) or `q4_0` (more
> still, at some quality cost). Because the KV cache type is a **server-side**
> Ollama setting, this reconfigures Ollama via a systemd drop-in
> (`OLLAMA_FLASH_ATTENTION=1` + `OLLAMA_KV_CACHE_TYPE=…`) and restarts the
> service — it needs **sudo** and affects all Ollama clients. Revert with
> `--kv-cache f16`. The model picker's `q) ornith-gpu-fit-large` shortcut does
> the same as `--gpu --kv-cache q8_0`.

**Native vs. text tool-calling.** On every turn the app advertises a real
`tools[]` schema (the same `read_file` / `list_dir` / `write_file` /
`run_command` / `ask_user` / `web_search` / `remember` set, filtered by mode).
Models trained for function-calling — Ornith, `qwen2.5-coder`, etc. — reply with
structured `tool_calls`, which the agent executes directly (no JSON-repair
needed) and rounds back so the model sees its own call. Models that instead emit
the legacy ` ```tool ` text block still work unchanged via the forgiving
text-protocol parser, so nothing regresses for weaker models. Reasoning is
streamed dimmed but kept out of the saved history.

### Editing, search, safety & workflow

A batch of Claude Code-inspired features, all local and built on the same tool
schema:

- **`edit_file`** — targeted `{path, old_string, new_string}` replacement.
  `old_string` must occur **exactly once** (otherwise it errors and asks for
  more context). Far cheaper and safer than rewriting a whole file with
  `write_file`; prefer it for changes to existing files.
- **`find_files` / `search_code`** — locate files by glob and grep file
  contents (`path:line: text`), so the model doesn't have to guess shell
  incantations. Read-only, available in planning mode too.
- **Diff preview** — `write_file` and `edit_file` show a colored `-`/`+` diff
  (not a full-file dump) before you confirm.
- **Permission allowlist** — confirmations offer `y` / `N` / `a`(lways).
  Choosing **a** remembers the action (commands by first word, e.g. `cmd:git`;
  file writes as `write`) in `.local_code/permissions`, so it won't ask again —
  this session or future ones.
- **Undo** — every write/edit is checkpointed to `.local_code/backups/`; `/undo`
  reverts the last one (restoring the old contents, or deleting a file that was
  freshly created). No sandbox, so this is the safety net.
- **`/yolo`** — toggle auto-accept (skip confirmations) mid-session, without
  restarting with `--yolo`.
- **`/compact`** — summarize the conversation now and shrink the context, and a
  **`ctx NN%`** indicator in the status bar shows how full the history budget is.
- **Custom commands** — drop a Markdown file in `.local_code/commands/`;
  `foo.md` becomes `/foo`, and its text is sent to the agent (with `$ARGS`
  replaced by any text after the command). Reusable prompts, no rebuild.

### Project awareness

Run `local_code` from your project's directory (or pass `--project DIR`). It
treats that folder and all its subfolders as **one program**: at startup it
`chdir`s into the root, injects the directory layout into the model's context,
and tells the model to use relative paths. It also keeps a persistent knowledge
file at **`.local_code/PROJECT.md`** — the model records durable facts
(architecture, build/run commands, conventions, gotchas) with a `remember` tool,
and that file is **auto-loaded into context every session**, so understanding
carries over. `remember` is disabled in planning mode (no writes). Use `/learn`
to have the agent scan the project and write the initial notes, and `/project`
to see the root and notes status. Disable the whole feature with `--no-project`.

### Planning mode

`--plan` (or `/plan` at runtime) puts the agent in a **design-only** mode: it
reasons about the problem and how to structure the software, inspects the
codebase read-only, and **asks you clarifying questions** via an `ask_user`
tool — but it never writes files or runs commands. `write_file` / `run_command`
are blocked by the runtime even with `--yolo`. Switch to `/build` when you're
ready to implement. The prompt shows `you (plan)>` while planning.

## How it works

- **ollama_client** — libcurl wrapper; `GET /api/tags` for the model list,
  streaming `POST /api/chat` (NDJSON) for generation.
- **conversation** — owns the transcript, estimates tokens (~bytes/4), keeps the
  most recent turns verbatim, and folds older turns into a rolling summary
  (produced by a one-shot model call) once the budget is exceeded. The system
  prompt is always retained on top.
- **tools** — advertises a real `tools[]` schema to the model and executes
  `read_file` / `list_dir` / `find_files` / `search_code` / `write_file` /
  `edit_file` / `run_command` / `ask_user` / `web_search` / `remember`,
  returning the result (truncated to caps). Two request formats are accepted,
  sharing one execution path:
  - **Native `tool_calls`** (Ollama function-calling) — what Ornith and other
    tool-trained models emit; parsed straight from `message.tool_calls`, no
    repair needed, and rounded back to the model on the next turn.
  - **Legacy ` ```tool {json} ``` ` text block** — the fallback for weak models.
    The parser is forgiving: brace-matched extraction, JSON-escape repair (raw
    newlines, backslash line-continuations), and the common name placements —
    `{"name":..,"args":..}`, flat `{"name":..,"path":..}`, and
    name-on-its-own-line `write_file\n{..}`. Here **`write_file` content comes
    from a separate ` ```file ` fence**, not a JSON string — packing multi-line
    source into JSON is the single biggest source of weak-model failures (missing
    quotes, bad escapes, truncation), and a raw fence sidesteps all of it.

  Writes and commands require y/N confirmation, and are disabled (and hidden from
  the schema) in planning mode.

- **thinking** — many local models (gemma included) and reasoning models like
  Ornith emit their reasoning into a separate field. The reasoning is streamed
  **dimmed** and then dropped from history (it never pollutes tool parsing or the
  context budget). Thinking is **off by default** (a faster, reliable path for
  weak models that otherwise leave the visible answer empty) but auto-enables for
  Ornith; toggle with `--think` / `--no-think`.
- **agent** — loop: query model → if it emitted a tool call (native or text),
  run it and feed the result back; otherwise the reply is the final answer. The result of each tool
  is also previewed to you (compiler errors, command output, exit codes — file
  reads are summarized as a byte count). If the model repeats the exact same
  tool call without progress, the loop stops and hands control back to you. If
  it emits a blank turn mid-task (a known quirk of some local models), it is
  nudged to continue a couple of times. Capped at `max_tool_turns` consecutive
  tool calls.

## Safety

`write_file` and `run_command` prompt before acting unless `--yolo` is set.
There is **no sandboxing** — commands run with your privileges. Use in a
directory you trust.

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).

The vendored `third_party/nlohmann/json.hpp` is distributed under its own MIT
license.
