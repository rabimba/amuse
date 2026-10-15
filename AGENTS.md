# AGENTS.md — Amuse

Personal AI agent (Gemini Multimodal Live API) with two-process design: native GUI binary + Node.js orchestrator. macOS (Objective-C/Cocoa) and Linux (C++/GTK/WebKit2GTK) supported. No CI; `main` branch; Conventional Commits (`feat(scope): ...`).

**Master plan:** `docs/superpowers/specs/2026-10-14-amuse-muse-parity-design.md` — full Muse-parity rebuild (Rust host daemon + sandboxed TS harness + Sentinel safety plane + Swift/GTK node apps), milestones M0–M9. Currently in **M1 (skeleton)** after M0 (legacy hardening) landed. Read the spec first; it tracks per-task status and is written so a fresh session can resume.

## Monorepo (new, M1+)

- `packages/hatch/` — provider-agnostic agent harness (TS/ESM, zero deps). `bin/hatch.mjs chat|ask`. Provider adapters: `src/providers/gemini.mjs` (REST), `src/providers/openai.mjs` (any OpenAI-compatible base URL: OpenAI/Ollama/vLLM/…). Config: `AMUSE_PROVIDER`, `OPENAI_API_KEY/BASE_URL/MODEL`, `GEMINI_API_KEY/MODEL` > `~/.amuse/config.json`. Tool execution currently shells to legacy `bin/amuse-bridge call` (replaced by Sentinel in M2).
- `crates/amused/` — Rust host daemon (skeleton: compiles, resolves data dir). Gateway/store/scheduler/Sentinel/authd come in M1–M2.
- `tests/scenarios/hatch_smoke.mjs` — `npm run test:hatch`: mock-provider full-loop check (no credentials needed) + live variants when keys present.
- `apps/macos/`, `rootfs/` — placeholders for M3/M2.
- Legacy lives in `src/` until M3.

## Commands

```bash
npm run build      # Compile native bridge -> bin/amuse-bridge (clang on macOS, g++/pkg-config on Linux)
./run.sh           # Full 6-stage E2E: deps, build, config check, real connector calls, mock WS test, live Gemini + HUD
./run.sh --gui     # Same, but keeps interactive HUD running
npm test           # ./bin/amuse test (read-only, isolated HOME) + mock WS E2E (real assertions, exit codes)
npm run test:e2e   # Mock Gemini WS server (port 9099) with isolated HOME; fails loudly on errors
npm run test:live  # Live Google Gemini API — needs API key + network; read-only no-hit queries
./bin/amuse call <tool> '<json>'   # Invoke a single native tool directly (best focused verification)
./bin/amuse tools                  # Dump all tool declaration schemas
./bin/amuse config --key <KEY>     # Save key to ~/.amuse/config.json
```

There is no lint, typecheck, or unit-test framework. Verification = build + `test:e2e` + targeted `bin/amuse call`.

Headless scripted HUD run (avoids a blocking GUI): `AMUSE_STARTUP_PROMPT="..." AMUSE_EXIT_ON_READY=1 AMUSE_AUTO_EXIT_MS=18000 ./bin/amuse start` (Linux only — these env hooks do not exist in the macOS binary).

## Safety gates (added in M0 — dangerous tools refuse without opt-in)

`AMUSE_ALLOW_SHELL=1` (run_command) · `AMUSE_ALLOW_CUSTOM_TOOLS=1` (build_and_run_custom_tool) · `AMUSE_ALLOW_GUI_CONTROL=1` (gui_desktop_action) · `AMUSE_ALLOW_AUTO_SUBMIT=1` (social auto-submit) · `AMUSE_AUTO_SOCIAL=1` (canned screenshot→LinkedIn auto-chain). `email_action send` requires `confirm_send:true`; attachments/files limited to $HOME minus sensitive dirs; web fetch refuses private/loopback IPs.

## Architecture

- **Native bridge** (`bin/amuse-bridge`, built from `src/native/`): owns the HUD, hotkeys, audio, and ALL tool declarations + execution. macOS entry: `src/native/main.m` + `src/native/connectors/*.m`. Linux entry: `src/native/linux/main_linux.cpp` (single large file, `tools`/`call` CLI at bottom).
- **Node orchestrator** (`src/js/index.mjs`): Gemini Live WS client. Does NOT define tools — it shells out to `bin/amuse-bridge tools` / `bridge call <name> <json>` (`src/js/toolRegistry.mjs`). **Editing tools in native code requires rebuilding the bridge** for Node to see changes.
- **IPC**: native spawns `node src/js/index.mjs`; line-delimited JSON over stdin/stdout. Native→Node: `audio_input_chunk`, `image_input_frame`, `user_text_input`, `tool_call_response`. Node→native: `agent_status`, `transcript`/`transcript_delta`, `audio_output_chunk`, `tool_call_request`. Keep this protocol in sync across `index.mjs`, `main.m`, `main_linux.cpp`.
- **HUD web UI**: `src/web/` (vanilla JS), loaded by the native binary via `file://<cwd>/src/web/index.html` — run from repo root or the HUD gets a blank window.
- `geminiLiveClient.mjs` is a hybrid engine: live `BidiGenerateContent` WSS with REST fallback. Default model `models/gemini-2.5-flash-native-audio-latest` (config.mjs).

## Configuration & data (all real, user-owned)

- `~/.amuse/config.json` (apiKey, model, voice). Env overrides: `GEMINI_API_KEY`, `GEMINI_MODEL` (env wins over file since M0).
- `~/.amuse/local_data.db` (SQLite: memory, notes), `~/.amuse/custom_tools/`, `~/.amuse/email_accounts.json` (plaintext passwords — moving to a vault in the rebuild).
- `~/Documents/AmuseNotes/` (+ `~/Notes` symlink), `~/Pictures/Amuse/`, `~/Videos/Amuse/`.
- Linux helpers invoked from native code via repo-relative paths: `scripts/browser_agent.py` (needs Playwright; re-execs itself under pyenv shim), `scripts/email_connector.py` (Thunderbird SQLite + IMAP/SMTP; reads JSON args from stdin when argv[1] == `-`). Don't move these scripts.

## Gotchas

- `bin/amuse-bridge` is a gitignored build artifact; `bin/amuse` auto-builds it if missing. If tool behavior looks stale, rebuild.
- **Tests:** `bin/amuse test` uses an isolated temp HOME and read-only no-hit queries (macOS may still need Full Disk Access/Automation consent). `test:e2e` uses mock WS + isolated HOME and exits non-zero on failure. `test:live`/`run.sh` hit real cloud APIs. Prefer `bin/amuse call` with harmless args when verifying.
- **Linux connector reality:** messages/calendar/reminders/contacts are local SQLite stores (no EDS sync yet); cloud connectors (Slack/Notion/GitHub/Google/WhatsApp/LinkedIn API) are listed as `available`, NOT connected. The 20-connectors "connected" seeding was removed in M0.
- macOS Node discovery is hardcoded in `src/native/main.m` (`~/.nvm/versions/node/v24.21.0/bin/node`, `/usr/local/bin/node`, `/opt/homebrew/bin/node`). If the user's node lives elsewhere the orchestrator silently fails to launch. `run.sh`/`bin/amuse` source NVM first.
- Linux build requires dev packages: `gtk+-3.0 webkit2gtk-4.1 javascriptcoregtk-4.1 libpulse-simple sqlite3 x11 xi xtst` (via pkg-config), plus ffmpeg for screencasts, `notify-send`, PulseAudio/PipeWire.
- Only dependency is `ws`; pure ESM (`"type": "module"`, `.mjs`). No devDependencies — don't assume test/lint libs exist.
- `example/` contains Meta Muse reverse-engineering artifacts (symbols, demangled output, extraction scripts) — reference material, not app code. Do not import from it.
