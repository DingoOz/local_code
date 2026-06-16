# local_code

A small terminal program that drives a **local LLM via Ollama** as an agentic
software developer. The model can read/write files and run shell commands
through a custom text protocol, with per-action confirmation. Designed for weak
local models: the system prompt is deliberately compact and the conversation
context is kept bounded by a sliding window plus automatic summarization.

## Requirements

- [Ollama](https://ollama.com) running locally (default `http://localhost:11434`)
- C++17 compiler, CMake ≥ 3.16
- `libcurl` and GNU `readline` development headers
  - Debian/Ubuntu: `sudo apt install libcurl4-openssl-dev libreadline-dev`
- `nlohmann/json` is vendored at `third_party/json.hpp` (no download needed)

## Build

```sh
cmake -S . -B build
cmake --build build -j
```

Produces `build/local_code`.

## Usage

```sh
./build/local_code                 # pick a model from a menu at startup
./build/local_code --model gemma4:latest
./build/local_code --budget 2048   # tighter context budget for weak models
./build/local_code --yolo          # skip y/N confirmation (dangerous)
./build/local_code --plan          # start in planning mode (no changes)
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
| `--think` | Enable model "thinking" (off by default) |

### REPL commands

`/help` `/plan` `/build` `/reset` `/model` `/quit`

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
- **tools** — parses a ` ```tool {json} ``` ` block from the model and executes
  `read_file` / `list_dir` / `write_file` / `run_command` / `ask_user`,
  returning the result (truncated to caps). The parser is forgiving of weak-model
  output: brace-matched extraction, JSON-escape repair (raw newlines, backslash
  line-continuations), and the common name placements — `{"name":..,"args":..}`,
  flat `{"name":..,"path":..}`, and name-on-its-own-line `write_file\n{..}`.
  **`write_file` content comes from a separate ` ```file ` fence**, not a JSON
  string — packing multi-line source into JSON is the single biggest source of
  weak-model failures (missing quotes, bad escapes, truncation), and a raw fence
  sidesteps all of it. Writes and commands require y/N confirmation, and are
  disabled in planning mode.

- **thinking** — many local models (gemma included) are "thinking" models that
  emit reasoning into a separate field and frequently leave the visible answer
  empty. Thinking is therefore **off by default** (a faster, reliable path for
  weak models); `--think` re-enables it and streams the reasoning too.
- **agent** — loop: query model → if it emitted a tool call, run it and feed the
  result back; otherwise the reply is the final answer. The result of each tool
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
