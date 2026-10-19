# Amuse Linux End-to-End Runbook

Self-contained handoff for bringing up and testing Amuse on a Linux machine.
No prior conversation context needed. Read `docs/superpowers/specs/2026-10-14-amuse-muse-parity-design.md` for the master architecture plan (milestones M0–M9).

## What Amuse is

Self-hosted personal AI agent (Meta Muse parity goal). Two layers:
- **Native bridge** `bin/amuse-bridge` (C++/GTK3/WebKit2GTK, `src/native/linux/main_linux.cpp`): HUD, hotkey, audio, and ALL 33 tool declarations + execution. CLI: `tools` / `call <name> <json>`.
- **Orchestrator** (Node, spawned by the bridge over line-delimited JSON stdin/stdout): talks to the LLM and routes tool calls. Two implementations:
  - `src/js/index.mjs` — legacy Gemini Live (voice, WSS). Used when provider is gemini (default).
  - `src/js/hatchOrchestrator.mjs` — any-backend text chat via `packages/hatch` (OpenAI-compatible: Ollama/vLLM/LM Studio/OpenRouter/OpenAI; Gemini REST). Used when `provider: openai` in `~/.amuse/config.json` or `AMUSE_ORCHESTRATOR=hatch`.

## Current state (what's done)

- **M0 (safety):** dangerous tools refuse without opt-in env flags; no fake seed data; email send requires `confirm_send:true`; attachments/files restricted to $HOME minus sensitive dirs; web fetch blocks private IPs; credentials via stdin (not argv) and scrubbed from logs.
- **M1 (any-backend):** `packages/hatch` provider-agnostic agent loop; CLI `node packages/hatch/bin/hatch.mjs chat|ask`.
- **Linux wiring (this runbook's companion commit):** Linux bridge now picks orchestrator from config provider (`openai` → hatch), discovers node across nvm/Volta/system paths (`AMUSE_NODE` override), and `list_connectors` returns a per-connector `readiness` field probed live.

### Safety gates (default OFF — this is intentional)

| Flag | Unlocks |
|---|---|
| `AMUSE_ALLOW_SHELL=1` | `computer_control_action run_command` (arbitrary shell) |
| `AMUSE_ALLOW_CUSTOM_TOOLS=1` | `build_and_run_custom_tool` (writes+runs Python/Bash) |
| `AMUSE_ALLOW_GUI_CONTROL=1` | `gui_desktop_action` (synthetic keyboard/mouse) |
| `AMUSE_ALLOW_AUTO_SUBMIT=1` | `social_media_action auto_submit` |
| `AMUSE_AUTO_SOCIAL=1` | orchestrator canned screenshot→LinkedIn auto-chain |

A tool returning `{"error":"safety_gate"}` is CORRECT behavior, not a bug.

## Prerequisites (Ubuntu/Debian)

```bash
sudo apt update && sudo apt install -y build-essential pkg-config \
  libgtk-3-dev libwebkit2gtk-4.1-dev libjavascriptcoregtk-4.1-dev \
  libpulse-dev libsqlite3-dev libx11-dev libxi-dev libxtst-dev \
  ffmpeg libnotify-bin python3 python3-pip nodejs npm

# Optional: Playwright browser agent
pip3 install playwright && python3 -m playwright install chromium

# Optional: local LLM via Ollama (any OpenAI-compatible endpoint works)
curl -fsSL https://ollama.com/install.sh | sh
ollama pull gpt-oss:latest
```

Node must be ≥18 (fetch, ESM). If `node` isn't on PATH for the bridge: `export AMUSE_NODE=/path/to/node`.

## Build & test sequence

```bash
cd amuse
npm install            # if npm registry blocked by corp proxy: project .npmrc pins npmjs; ws is the only dep
npm run build          # g++ compile → bin/amuse-bridge

./bin/amuse test       # native diagnostics: read-only, isolated temp HOME, exit 1 on failure
npm run test:e2e       # mock Gemini WS + tool-call loop, real assertions
npm run test:hatch     # mock OpenAI-compatible full-loop + text-mode (no credentials)
```

### Live test with a local model

```bash
AMUSE_PROVIDER=openai OPENAI_BASE_URL=http://localhost:11434/v1 \
OPENAI_API_KEY=ollama OPENAI_MODEL=gpt-oss:latest npm run test:hatch:live
```

### Interactive HUD with any backend

```bash
./bin/amuse config --provider openai
./bin/amuse config --openai-base-url http://localhost:11434/v1
./bin/amuse config --openai-key ollama
./bin/amuse config --openai-model gpt-oss:latest
./bin/amuse start          # double-tap Alt toggles the HUD
```

If the model lacks native tool calling: `./bin/amuse config --openai-tool-mode text` (tools are described in the system prompt; the model emits ```tool_call blocks; hatch parses them).

### Legacy Gemini voice mode

```bash
./bin/amuse config --provider gemini
./bin/amuse config --key <GEMINI_API_KEY>
./bin/amuse start          # full-duplex voice via Gemini Live
```

## Expected results on first Linux run

- `./bin/amuse tools` → 33 tool declarations.
- `./bin/amuse test` → PASSED. Gated tools refuse with `safety_gate` (correct).
- `list_connectors` → each row has `readiness`:
  - `connected` = works now (notes, memory, files, clock, email, plans, web_search)
  - `local_stub_no_eds` = local SQLite store, Evolution Data Server not reachable (messages/calendar/reminders/contacts)
  - `missing_ffmpeg` / `missing_notify_send` / `missing_playwright` / `no_display` = install the named dependency
  - `available_no_oauth` = cloud connector (Gmail OAuth, Slack, Notion, GitHub...) — advertised, not implemented yet (milestone M4)
- `email_action list_accounts` → finds Thunderbird profiles if present; otherwise `setup_hint` guides connecting an IMAP account.
- HUD text chat with `provider=openai` works end-to-end; voice requires Gemini Live (`provider=gemini`).

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `npm install` 403 on tarball | Corp proxy blocks npm registry | Repo `.npmrc` pins npmjs.org; if still blocked, vendor `ws` from GitHub: `curl -L https://codeload.github.com/websockets/ws/tar.gz/refs/tags/8.22.0 | tar xz && mkdir -p node_modules/ws && cp -R ws-8.22.0/{lib,index.js,wrapper.mjs,package.json,LICENSE,README.md} node_modules/ws/` |
| Build error: `strcasecmp` | missing include | add `#include <strings.h>` to main_linux.cpp |
| Build error: GTK/webkit pkg-config | dev packages missing | see Prerequisites |
| HUD invisible | undecorated keep-above window behind others | `wmctrl -l`; double-tap Alt; check `$DISPLAY`/`$WAYLAND_DISPLAY` |
| "node not found" in log | PATH lookup failed | `AMUSE_NODE=$(which node) ./bin/amuse start` |
| Orchestrator launches Gemini mode | config provider not set | `./bin/amuse config --provider openai` or `AMUSE_ORCHESTRATOR=hatch ./bin/amuse start` |
| Tool returns `safety_gate` | intentional | set the AMUSE_ALLOW_* flag if you accept the risk |
| Model doesn't call tools | endpoint lacks native tool calling | `./bin/amuse config --openai-tool-mode text` |
| Alarms don't survive restart | known limitation (in-process threads) | proper persistent scheduler is milestone M6 |

## Known limitations on Linux (by design — see spec milestones)

- messages/calendar/reminders/contacts are local stores (no EDS sync) → M3
- cloud connectors listed but no OAuth → M4
- no sandbox yet (tools run with user privileges; dangerous ones are gated) → M2
- voice only via Gemini Live (local STT/TTS cascade for other providers) → M3

## What to do if something fails

1. Run the smallest reproducer: `./bin/amuse call <tool> '<json>'` directly — the error JSON is honest.
2. Check `/tmp/amuse-gui.log` (if started with output redirect) or stderr.
3. Compile errors: fix includes/types in `main_linux.cpp` (all new M0/M1 code is standard POSIX + sqlite3 + GTK).
4. Update `docs/superpowers/specs/2026-10-14-amuse-muse-parity-design.md` status trackers with findings.
