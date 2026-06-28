# local_code — Development Plan

A roadmap for `local_code`, the terminal agent that drives a local LLM (via
Ollama) as a software developer. This document captures the current state, the
architecture, and planned work — with **Whisper speech-to-text (voice input)**
as the next major feature.

## Vision

A fast, local-first coding agent for the terminal that works well even with
weak local models. Everything runs on the user's machine: the model (Ollama),
the tools (file read/write, shell), and — soon — speech input (Whisper). No
cloud dependency, no data leaving the box.

## Current status (built)

- **Streaming chat** with Ollama (`/api/chat`, NDJSON) — `src/ollama_client.*`.
- **Agentic tools** via a tolerant text protocol — `src/tools.*`:
  `read_file`, `list_dir`, `write_file` (content in a ```` ```file ```` fence),
  `run_command`, `ask_user`. Per-action y/N confirmation.
- **Context management** — sliding window + rolling summarization to keep weak
  models within budget — `src/conversation.*`.
- **Planning mode** (`--plan` / `/plan`) — design & ask questions, no changes.
- **ncurses TUI** — bordered screen, scrolling output, inline input with
  history, and a **bottom status bar with live GPU utilization / VRAM** from
  `nvidia-smi` — `src/tui.*`, `src/gpu_monitor.*`. Plain fallback for piped I/O
  (`src/plain_console.*`), selected via the `Console` interface (`src/io.hpp`).
- **Robustness for weak models**: JSON-escape repair, multiple tool-name
  placements, channel-marker stripping, thinking off by default, blank-turn
  nudging, repeat-call loop breaking, category colors.
- **Model bootstrap**: offers to `pull` a recommended model when none exist.

## Architecture overview

```
main.cpp ── Console (io.hpp) ──┬── PlainConsole   (cout/getline)
                               └── TuiConsole      (ncurses + GpuMonitor)
   │
   ├── OllamaClient  (libcurl: chat / tags / pull)
   ├── Conversation  (history + summarization)
   └── Agent ── tools.cpp (parse + execute tool calls)
```

The `Console` abstraction is the key extension point: any new input modality
(such as voice) plugs in here without touching the agent or tools.

---

## Whisper STT integration (next major feature)

**Goal:** let the user speak a prompt instead of typing it. Press a key in the
TUI, talk, release/stop, and the transcribed text drops into the input line for
review and submission. Fully local, **CPU-based by default**, consistent with
the project's ethos.

### Approach: local whisper.cpp (CPU-first)

Use [whisper.cpp](https://github.com/ggerganov/whisper.cpp) — a C/C++ GGML
implementation of OpenAI Whisper that runs locally on CPU or GPU. It matches
this project's local-first, C++ design and needs no network or Python.

**Run STT on the CPU, not the GPU.** The 8 GB GPU is already fully committed to
the LLM — especially in `--gpu` / `--kv-cache` modes, where the weights + KV
cache leave no spare VRAM. Whisper is small and fast enough on CPU for
short dictation clips, so keeping it off the GPU avoids contention and VRAM
pressure entirely. GPU/CUDA transcription stays an opt-in for machines with
headroom, never the default.

- **CPU model choice:** prefer the small English models — `base.en` (good
  accuracy, ~1.5× realtime on a few cores) or `tiny.en` (fastest, lower
  accuracy). Use the **quantized** GGML builds (e.g. `ggml-base.en-q5_1.bin`)
  to cut RAM and speed up CPU inference further.
- **Threads:** pass `whisper-cli -t <N>` (e.g. number of physical cores) so a
  short clip transcribes in well under its own duration.
- A multi-second dictation typically transcribes in ~1 s on a modern CPU — fast
  enough that the "transcribing…" status is barely visible.

**Lightweight CPU-only alternative (fallback):** if whisper.cpp is unavailable,
[Vosk](https://alphacephei.com/vosk/) offers a small, fully offline, CPU-only
streaming recognizer with a tiny (~50 MB) English model. Lower accuracy than
Whisper but trivial to run on any CPU; worth keeping as an optional backend
behind the same `dictate()` seam.

Two integration options, in order of preference:

1. **Shell out to the `whisper-cli` binary** (initial implementation). Capture
   audio to a temp WAV, run `whisper-cli -m <model.bin> -f clip.wav -otxt`, read
   the resulting transcript. Mirrors how `run_command` and `ollama pull` already
   shell out — lowest coupling, fastest to ship.
2. **Link `libwhisper` directly** (later optimization). Removes the subprocess
   and temp file, enables streaming/partial transcripts, but adds a build
   dependency and model-loading lifecycle to manage.

### Audio capture

Record from the default microphone to a 16 kHz mono WAV (Whisper's expected
format). Options, preferring the "shell out" pattern first:

- `arecord -f S16_LE -r 16000 -c 1 clip.wav` (ALSA, usually preinstalled), or
- `ffmpeg -f alsa -i default -ar 16000 -ac 1 clip.wav`, or
- PortAudio/libsndfile if/when we link libraries directly.

Recording runs on a background thread (like `GpuMonitor`); start/stop is driven
by a TUI hotkey so the UI stays responsive.

### Integration points

- **`Console` interface** gains an optional capability, e.g.
  `std::optional<std::string> dictate()`, default-unsupported in `PlainConsole`
  and implemented in `TuiConsole`. The REPL loop offers voice when available.
- **TUI hotkey**: a dedicated key (e.g. `Ctrl-R` or `F2`) toggles
  recording. While recording, the **status bar** shows a `● REC` indicator and
  elapsed time (reusing the existing status-bar render path).
- **Flow**: hotkey → start capture → hotkey/Enter → stop → transcribe (show a
  transient "transcribing…" status) → insert text into the input line. The user
  can edit before pressing Enter — voice never auto-submits or auto-runs tools.
- **Slash command**: `/listen` as an alternative to the hotkey; `/voice on|off`
  to enable/disable.

### New components (proposed)

- `src/audio_recorder.{hpp,cpp}` — background mic capture to a temp WAV, with
  start/stop and a "is recording" flag.
- `src/whisper_stt.{hpp,cpp}` — wraps transcription (subprocess first), returns
  the recognized text or an error.
- Wire both into `TuiConsole::dictate()`.

### Configuration / flags

- `--whisper-model PATH` — path to the GGML Whisper model (e.g.
  `ggml-base.en.bin`); default looks in `~/.local_code/` and `$WHISPER_MODEL`.
- `--whisper-bin PATH` — path to `whisper-cli` (default: found on `PATH`).
- `--no-voice` — disable voice even if the tooling is present.
- Auto-detect: if neither the binary nor a model is found, voice is silently
  disabled and a hint is shown in `/help`.

### Phases

1. **Plumbing** — `AudioRecorder` (arecord) + `WhisperStt` (subprocess), unit
   tests against a fixed sample WAV; no UI yet.
2. **TUI wiring** — `Console::dictate()`, `TuiConsole` hotkey, `● REC` status
   indicator, transcript-into-input flow.
3. **Config & detection** — flags, model auto-detection, graceful disable,
   `/listen` and `/voice` commands; README docs.
4. **Optimization (optional)** — link `libwhisper` for streaming partial
   transcripts and to drop the temp-file round trip.

### Dependencies

- `whisper.cpp` (`whisper-cli` binary + a small GGML model such as a quantized
  `base.en`), built CPU-only — no CUDA required.
- An audio capture tool (`arecord`/ALSA, or `ffmpeg`).
- A microphone. CUDA transcription is optional and off by default, since the GPU
  is reserved for the LLM; CPU handles short dictation clips comfortably.
- Optional fallback backend: Vosk (small CPU-only model) for machines without
  whisper.cpp.

### Risks & mitigations

- **No mic / no whisper installed** → feature auto-disables; typing always works.
- **Transcription latency** on CPU → prefer a small model (`base.en`) and/or the
  GPU build; show a "transcribing…" status so the UI never looks frozen.
- **Accuracy on code/jargon** → transcript lands in the editable input line, not
  straight into execution; the user confirms before submitting.
- **Audio format mismatch** → always capture 16 kHz mono S16_LE WAV.

---

## Web search via local SearXNG

**Goal:** give the agent a `web_search` tool backed by a **local
[SearXNG](https://github.com/searxng/searxng) instance**, so it can look up
current/external information without any third-party API key — consistent with
the local-first design.

### Approach

- SearXNG runs locally (Docker is the simplest deployment) and exposes a JSON
  search API: `GET http://<host>/search?q=<query>&format=json`. The JSON format
  must be enabled in SearXNG's `settings.yml` (`search.formats: [html, json]`).
- A small libcurl client (`src/web_search.*`) issues the GET, parses the
  `results[]` array (title / url / content) and returns the top N concisely so
  the model's context stays small.
- A new `web_search {"query"}` tool is exposed to the model. It is **read-only**,
  so it is permitted in planning mode too (useful for research).

### Detection & configuration

- `--searxng URL` sets the instance (default `http://localhost:8888`).
- On startup the app probes the JSON API; if reachable, `web_search` is enabled
  and added to the system prompt. A refused connection is instant, so there's no
  startup penalty when SearXNG isn't running.
- `--web` forces it on (skip probe); `--no-web` disables it entirely.

### Installer

`install.sh` (Ubuntu) builds the app and **offers** to set up web search: if the
user opts in, it installs Docker (if needed), runs the `searxng/searxng`
container bound to `127.0.0.1:8888`, and writes a `settings.yml` that enables the
JSON API. The app then auto-detects it on next launch.

### Status: implemented (tool + client + installer); see `src/web_search.*`.

## Auto-compaction (context nearly full)

**Goal:** when the conversation is about to overflow the context window,
**automatically** summarize and shrink it — so a long session never gets
truncated mid-thought or silently drops important earlier turns. This extends
the manual `/compact` (feature #7) into a proactive safety net for weak models
with small windows.

### Approach

- A single high-water mark drives it: the same context-usage estimate already
  surfaced as `ctx NN%` via `Console::set_ctx` (window token estimate ÷ budget).
- Before each model call in `Agent::handle`, check usage. When it crosses a
  threshold (default **85%**), call `Conversation::compact()` — fold the older
  turns into the running summary — *before* sending the request, so the turn
  goes out within budget instead of triggering a hard eviction.
- Compact the **oldest** turns first and keep the most recent N intact, so the
  immediate working context (current file, last tool result) is preserved.
- Surface it unobtrusively: a one-line notice ("Context 85% full — compacted
  earlier turns.") and the status bar's `ctx%` drops, so the user sees what
  happened. Never auto-compacts the system prompt or the active turn.

### Configuration / flags

- `--compact-at PCT` — high-water mark (default `85`; `0`/`100` disables auto).
- `--no-auto-compact` — keep only manual `/compact`.
- Hysteresis: after compacting, require usage to fall and re-cross the mark
  before compacting again, so it can't thrash on a single oversized turn.

### Integration points

- `Conversation` already owns `compact()` and the token estimate; add a small
  `usage_pct()` / `should_compact()` helper and an "auto" policy field.
- `Agent::handle` calls it at the top of the loop (and again before re-querying
  after a tool result, since tool output can be large).
- Reuses the existing summarizer (`make_summarizer` in `main.cpp`); on a
  summarizer failure it falls back to plain oldest-turn eviction, as today.

### Status: planned. `Conversation::compact()` and the `ctx%` estimate exist;
this adds the automatic trigger, threshold flags, and hysteresis.

## KV-cache quantization (larger GPU context, later)

**Goal:** fit a **much larger** context window into the same VRAM, so `--gpu`
mode (and small-VRAM users generally) can run long contexts without spilling to
CPU. Captured here as a future option, not yet wired in.

### Why

`--gpu` currently caps the raw context at **40960** tokens (≈6.5 GB on an 8 GB
card; ~50K is the measured spill cliff). The limit is the **fp16 KV cache** —
its bytes/token are what fill VRAM as the window grows. A further ~1.3 GB is
reserved by llama.cpp for compute/activation buffers and a fit-safety margin and
**cannot** be reclaimed for KV; pushing past the cliff makes Ollama offload whole
layers to CPU (slower), not pack more context. So a bigger raw `--num-ctx` is not
the lever — **smaller KV bytes/token** is.

### Approach

- Quantize the KV cache instead of keeping it fp16. With flash attention on,
  Ollama exposes this server-side:
  - `OLLAMA_FLASH_ATTENTION=1`
  - `OLLAMA_KV_CACHE_TYPE=q8_0` (≈½ the bytes/token → ~80K+ tokens in the same
    VRAM) or `q4_0` (≈¼ → larger still, at some quality cost).
- These are **environment settings on the Ollama server**, not per-request
  `options`, so the app can't set them on the existing `/api/chat` call. Likely
  shapes:
  1. **Document + detect** (simplest): README/`--help` note; on startup, if the
     KV type is observable, surface it in the banner (e.g. `kv q8_0`).
  2. **Managed server**: if `local_code` ever launches/manages its own Ollama
     instance, export the env vars there and bump the `--gpu` context to match.
  3. **Profiles**: a `--gpu-ctx large` style flag that assumes a quantized KV
     server and selects a bigger `kGpuFitNumCtx` (e.g. 81920) accordingly.

### Risks / notes

- Quantized KV trades a little accuracy for capacity; keep fp16 the default and
  make quantization opt-in.
- Exact token ceilings depend on the model's head/layer dims — re-measure per
  model (as done for the fp16 numbers), don't assume the ×2 / ×4 scaling is
  exact.
- Requires flash attention support for the model/quant combo; fall back to fp16
  if unavailable.

### Status: implemented. `--kv-cache q8_0|q4_0|f16` (and the picker's
`q) ornith-gpu-fit-large` shortcut) applies the type via a systemd drop-in +
service restart (sudo, idempotent), and a quantized cache enlarges the GPU-fit
context to `kGpuFitNumCtxQuant = 81920`. The 80K figure is an estimate — verify
and tune per the measurement procedure; fp16 `--gpu` stays at 40960.

## Other planned work (backlog)

- **Offline knowledge via Kiwix** — as a future option, query a local
  [Kiwix](https://kiwix.org) server (`kiwix-serve`) hosting ZIM archives
  (offline Wikipedia, Stack Exchange, dev docs, etc.). This would add an
  offline-first counterpart to web search: when no internet/SearXNG is
  available, the agent could still retrieve reference content. Likely shape: a
  `kiwix_search` tool (or a `--kiwix URL` source that `web_search` falls back to)
  hitting Kiwix's search/suggest API, with the installer optionally fetching
  `kiwix-serve` and a chosen ZIM. Fully local, no network required.
- **AMD/Intel GPU stats** in the status bar (`rocm-smi` / `intel_gpu_top`),
  alongside the existing NVIDIA path.
- **Optional sandboxing** for `run_command` (container / restricted shell).
- **Multi-file edits / patch application** as a higher-level tool.
- **Session save/restore** (persist the conversation transcript).
- **Configurable key bindings** for the TUI.

## Milestones

1. ✅ Core agent + tools + context management.
2. ✅ Planning mode, model bootstrap, weak-model robustness.
3. ✅ ncurses TUI with GPU status bar and category colors.
4. ✅ Web search via local SearXNG (`web_search` tool + installer).
5. ⏭ **Whisper STT voice input** (this document).
6. ⏳ Auto-compaction when the context window is nearly full.
7. ⏳ Broader GPU vendor support + session persistence.

---

## Claude Code-inspired feature additions

Eight features adapted from Claude Code, chosen to fit the weak-local-model,
compact, no-sandbox ethos. All driven through the existing tool schema (native
`tool_calls` + text-protocol fallback) and `Console` abstraction.

### 1. `edit_file` — targeted string replacement
A new tool `edit_file {path, old_string, new_string}`: read the file, require
`old_string` to occur **exactly once** (else error asking for more context),
replace it, preview a diff, confirm, write. Far cheaper and safer than
`write_file` rewriting a whole file — the prime cause of weak-model truncation.
Reuses the native/text tool plumbing; advertised alongside `write_file`, hidden
in planning mode.

### 2. Persistent permission allowlist (`y` / `N` / `a`)
`Console::confirm` returns `Confirm{No, Once, Always}`. Choosing **a**(lways)
records a rule in `PermissionStore` (persisted to `.local_code/permissions`) so
matching actions skip the prompt next time. Rules: commands by first token
(`cmd:git`), file writes coarse (`write`), notes (`remember`). Cuts friction
without abandoning the safety model.

### 3. Backup / undo checkpoints
Before every `write_file` / `edit_file`, `UndoStack` snapshots the file to
`.local_code/backups/`. `/undo` restores the most recent snapshot (or deletes a
newly created file). Cheap safety net given there is no sandbox.

### 4. Search tools — `find_files` / `search_code`
`find_files {pattern}` (glob via `fnmatch`) and `search_code {pattern, path?}`
(regex with substring fallback, `path:line: text`, capped). Read-only, available
in planning mode. Lets weak models locate code without guessing shell syntax.

### 5. Diff preview on write
A shared `preview_diff(old, new)` (common prefix/suffix trim + colored `-`/`+`
lines, capped) replaces the full-file dump shown before `write_file`, and powers
the `edit_file` preview.

### 6. Custom slash commands
At startup, scan `<root>/.local_code/commands/*.md`; each `foo.md` becomes
`/foo`, its body sent to the agent (with `$ARGS` replaced by trailing text).
User-defined reusable prompts, no rebuild needed.

### 7. Context-usage indicator + `/compact`
Status bar shows `ctx NN%` (window token estimate ÷ budget) via
`Console::set_ctx`. `/compact` forces `Conversation::compact()` — summarize all
turns now and clear them — instead of waiting for automatic eviction.

### 8. Runtime auto-accept toggle
`/yolo` flips `cfg.yolo` mid-session (the prompt/banner reflects it), so a user
can grant a hands-off stretch without restarting.

### New / changed files
- New: `src/diff.hpp`, `src/permissions.{hpp,cpp}`, `src/undo.{hpp,cpp}`.
- Tools: `src/tools.{hpp,cpp}` (ToolCtx, edit_file, search tools, allowlist/undo
  wiring, schema), `src/system_prompt.hpp`.
- IO: `src/io.hpp` (Confirm enum, set_ctx), `src/tui.*`, `src/plain_console.*`.
- Core: `src/conversation.*` (compact), `src/agent.*` (ToolCtx, ctx%),
  `src/main.cpp` (commands, /undo, /compact, /yolo), `CMakeLists.txt`.
- Tests: `tests/test_features.cpp` (diff, glob, permissions, edit mapping).
