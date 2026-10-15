# Amuse 🔮

**Amuse** is a self-hosted personal AI agent for macOS and Linux, powered by the **Google Gemini Multimodal Live API** (default model: `gemini-2.5-flash-native-audio-latest`) through Google AI Studio.

It is being rebuilt for full feature parity with Meta's personal AI agent **Muse** (`ai.meta.com/muse/`), with any-LLM-backend support, a sandboxed action runtime, and an approval/audit safety layer. **See the master plan:** [`docs/superpowers/specs/2026-10-14-amuse-muse-parity-design.md`](docs/superpowers/specs/2026-10-14-amuse-muse-parity-design.md).

> **Status (honest):** the code in `src/` is the legacy prototype (milestone M0). It has a real voice HUD loop and some genuinely working connectors, but several Linux "connectors" were previously seeded with fake data (removed in M0) and cloud connectors (Slack, Notion, GitHub, Google, WhatsApp, LinkedIn API) are **not** actually connected yet — the registry now lists them as `available`, not `connected`.

---

## What works today

### macOS (9 tools — real, read-mostly)
| Tool | Notes |
|---|---|
| `search_messages` / `get_recent_messages` | Reads iMessage `chat.db` (needs Full Disk Access). Misses messages stored only in `attributedBody`. |
| `search_contacts` | Contacts.framework (permission must be pre-granted to the host terminal). |
| `get_calendar_events` / `create_calendar_event` | EventKit. |
| `get_reminders` / `create_reminder` | EventKit. |
| `search_notes` / `create_note` | Apple Notes via AppleScript (needs Automation consent). |

### Linux (33 tools — mixed)
Real: multi-timezone clock, alarms/timers (in-process; not persisted across restart; >24h capped), Markdown notes, active memory, action plans (CRUD), email (Thunderbird DB + IMAP/SMTP via `scripts/email_connector.py`), screenshots/screencasts (ffmpeg x11grab), web search/fetch, MPRIS/pactl media control, Playwright browser agent (headless, not logged in).
Stub/local-only: messages, calendar, reminders, contacts stores are local SQLite (no Evolution/EDS sync yet); `send_message` only logs locally; `connect_service` is cosmetic.

## Safety gates (M0)

Dangerous capabilities are **disabled by default** and require explicit environment opt-in:

| Flag | Unlocks |
|---|---|
| `AMUSE_ALLOW_SHELL=1` | `computer_control_action run_command` (arbitrary shell) |
| `AMUSE_ALLOW_CUSTOM_TOOLS=1` | `build_and_run_custom_tool` (writes + executes Python/Bash) |
| `AMUSE_ALLOW_GUI_CONTROL=1` | `gui_desktop_action` (synthetic keyboard/mouse into any window) |
| `AMUSE_ALLOW_AUTO_SUBMIT=1` | `social_media_action auto_submit` |
| `AMUSE_AUTO_SOCIAL=1` | Orchestrator's canned auto screenshot→LinkedIn chain |

Additionally: `email_action send` requires `confirm_send: true` (returns the draft for user confirmation otherwise); email attachments and file writes are restricted to the home directory minus sensitive locations (`~/.ssh`, `~/.gnupg`, `~/.aws`, `~/.amuse`, keyrings, `~/Library`); `web_search_and_fetch` refuses loopback/private addresses; tool arguments with secret-looking keys are scrubbed from logs.

---

## Quick Start

```bash
./run.sh                 # 6-stage build + verification + live HUD run
./run.sh --gui           # keep the interactive HUD running
./bin/amuse config --key YOUR_GEMINI_API_KEY
./bin/amuse              # launch HUD (Double-tap Option/Alt to toggle)
```

## CLI Commands

| Command | Description |
|---|---|
| `./bin/amuse test` | Read-only connector diagnostics with an isolated HOME; exits non-zero on failure |
| `./bin/amuse tools` | Prints the tool declaration schemas (9 on macOS, 33 on Linux) |
| `./bin/amuse call <tool> [args]` | Directly invoke a native tool with JSON args |
| `npm run test:e2e` | Mock-Gemini WebSocket E2E (isolated HOME, real assertions) |
| `npm run test:live` | Live Gemini API E2E (needs API key; read-only no-hit queries) |

## Architecture (legacy M0)

```
Native bridge (bin/amuse-bridge)
  macOS: Obj-C/Cocoa (src/native/*.m + connectors/*.m)
  Linux: C++/GTK/WebKit2GTK (src/native/linux/main_linux.cpp)
        │ line-delimited JSON over stdin/stdout
Node orchestrator (src/js/) — Gemini Live WSS client + REST fallback
HUD web UI (src/web/) loaded via file:// — run from repo root
```

The target architecture (Rust host daemon + sandboxed TS harness + Sentinel policy/egress/vault + Swift/GTK node apps) is specified in the design doc linked above.
