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
review and submission. Fully local, consistent with the project's ethos.

### Approach: local whisper.cpp

Use [whisper.cpp](https://github.com/ggerganov/whisper.cpp) — a C/C++ GGML
implementation of OpenAI Whisper that runs locally on CPU or GPU. It matches
this project's local-first, C++ design and needs no network or Python.

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

- `whisper.cpp` (`whisper-cli` binary + a GGML model such as `base.en`).
- An audio capture tool (`arecord`/ALSA, or `ffmpeg`).
- A microphone. On the GPU box, whisper.cpp can use CUDA for fast transcription.

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
6. ⏳ Broader GPU vendor support + session persistence.
