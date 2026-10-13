# Amuse (Agent Muse) 🔮

**Amuse** is a personal AI agent for macOS and Linux powered by the **Google Gemini Multimodal Live API** (`gemini-3.1-flash-live-preview`) through **Google AI Studio**.

It is engineered for full feature parity with Meta's personal AI agent **Muse** (`ai.meta.com/muse/`, Project "Hatch" / `Endo.*`), featuring a floating liquid-glass HUD, real-time full-duplex voice & live desktop screen vision, global double-tap Option/Alt summoning, **Active Memory**, **Action Plans**, **Dynamic Custom Tool Building**, **30 native tools**, and **20 auto-connecting local & cloud connectors**.

---

## Architecture Overview

```
                      ┌──────────────────────────────────────┐
                      │            User Interface            │
                      │   - Double-Tap Option/Alt Monitor    │
                      │   - Liquid Glass Floating HUD        │
                      │   - Pulsing Reactive Avatar Orb      │
                      │   - Rich Interactive Cards (Notes,   │
                      │     Alarms, Screen, Memory, Plans)   │
                      └──────────────────┬───────────────────┘
                                         │
                   ┌─────────────────────┴─────────────────────┐
                   ▼                                           ▼
┌──────────────────────────────────────┐     ┌──────────────────────────────────┐
│   Native Engine (macOS / Linux)      │     │      Gemini Live Client (Node)   │
│ - AudioEngine (16kHz in / 24kHz out) │◄───►│ - Bi-directional WebSocket API   │
│ - HotkeyMonitor (Double-tap Opt/Alt) │     │ - Real-time Full Duplex Audio    │
│ - WebKit / WebKit2GTK HUD Container  │     │ - Live Screen Vision Frame Input │
│ - Background Alarm & Task Scheduler  │     │ - Real-time Multi-TZ Grounding   │
└──────────────────┬───────────────────┘     └─────────────────┬────────────────┘
                   │                                           │
                   ▼                                           ▼
┌──────────────────────────────────────┐     ┌──────────────────────────────────┐
│  30 Tools & 20 Auto-Connectors       │     │         Google AI Studio         │
│ - Markdown Notes (~/Documents/       │     │ - gemini-3.1-flash-live-preview  │
│   AmuseNotes/ & ~/Notes/)            │     │ - Natural Multimodal Voices      │
│ - GNOME/Evolution Calendar & Tasks   │     │   (Puck, Charon, Kore, etc.)     │
│ - Alarms, Timers & Audio Chimes      │     └──────────────────────────────────┘
│ - Screen Vision & FFmpeg Screencasts │
│ - Active Memory & Action Plans       │
│ - Dynamic Python/Bash Tool Builder   │
│ - LinkedIn, Gmail, Slack, GitHub,    │
│   Notion, WhatsApp, Spotify, Files   │
└──────────────────────────────────────┘
```

---

## Muse Feature Parity (30 Native Tools & 20 Auto-Connecting Services)

Amuse automatically connects and executes tools across **20 local and cloud connectors** without requiring manual setup:

1. **Real-Time Multi-Timezone Clock (`get_current_time_and_environment`)**:
   - Injects live system clock and multi-timezone reference (`America/Los_Angeles` PDT, `America/Chicago` CST/CDT, `America/New_York` ET, `UTC`) into every turn so time-sensitive requests (e.g., *"9 AM CST"*) are always accurate.
2. **Linux Markdown Notes & HUD Note Cards (`create_note`, `search_notes`, `open_note`)**:
   - Since Linux does not have Apple's `Notes.app`, Amuse saves every note both in SQLite and as a real `.md` file in `~/Documents/AmuseNotes/` (symlinked at `~/Notes/`), renders an interactive **Note Card** directly inside the HUD (`📝 Notes` button), and can open notes on the desktop via `xdg-open`.
3. **Alarms, Timers & Scheduled Workflows (`set_alarm_or_timer`, `list_alarms_and_timers`, `schedule_task`)**:
   - Arms background countdown threads that play a synthesized 24kHz PCM harmonic chime via PulseAudio/PipeWire and trigger critical desktop notifications (`notify-send -u critical`).
4. **Live Multimodal Screen Vision & Screencasts (`take_screenshot`, `record_screencast`)**:
   - Captures desktop screenshots (`~/Pictures/Amuse/*.png`) and streams the PNG frame directly into the live Gemini Multimodal WebSocket (`realtimeInput.mediaChunks`) so Gemini can see your screen (`🖥️ Screen` button).
   - Records H.264 `.mp4` desktop screencasts (`~/Videos/Amuse/*.mp4`) immediately or on a schedule using `ffmpeg -f x11grab`.
5. **Social Media & LinkedIn Connector (`social_media_action`)**:
   - Auto-connects to LinkedIn / X / Facebook to draft, copy to clipboard, schedule posts (with alarm + calendar entry), and launch the browser composer.
6. **Active Memory & Action Plans (`save_memory`, `recall_memories`, `manage_action_plan`)**:
   - Persists user preferences, personal facts, and multi-step autonomous Action Plans across sessions in `~/.amuse/local_data.db` and `~/Documents/AmuseNotes/Amuse_Active_Memory.md` (`🧠 Memory` button).
7. **Dynamic Custom Tool Builder (`build_and_run_custom_tool`)**:
   - Dynamically writes, saves, and executes custom Python or Bash automation scripts in `~/.amuse/custom_tools/`.
8. **Full-Disk File Assistant & Desktop Control (`manage_files`, `computer_control_action`, `media_and_system_control`)**:
   - List, search, read, write, move, or trash files; launch desktop apps/URLs; control system audio volume and Spotify/MPRIS playback.
9. **Messages, Contacts, Calendar, Reminders, Email & Web Search**:
   - `search_messages`, `get_recent_messages`, `send_message`, `search_contacts`, `create_contact`, `get_calendar_events`, `create_calendar_event` (synced with GNOME/Evolution `calendar.ics`), `get_reminders`, `create_reminder` (`tasks.ics`), `email_action` (Gmail), and `web_search_and_fetch`.

---

## Quick Start

### 1. Run Everything End-to-End (Single Command)
```bash
./run.sh
```
To launch and keep the interactive HUD running continuously:
```bash
./run.sh --gui
```

### 2. Configure Your Gemini API Key
```bash
./bin/amuse config --key YOUR_GEMINI_API_KEY
```

### 3. Launch Amuse
```bash
./bin/amuse
```
- Press **Double-tap Option/Alt (`⌥⌥`)** anytime to toggle the HUD.
- Use the top HUD quick buttons: **`🖥️ Screen`**, **`🧠 Memory`**, **`📝 Notes`**, and **`🔌 Connectors`**.

---

## CLI Commands

| Command | Description |
|---|---|
| `./run.sh` | Runs the complete 6-stage build, connector, and live Gemini E2E suite |
| `./run.sh --gui` | Runs verification and keeps the interactive HUD running |
| `./bin/amuse` | Launches the floating HUD and background Gemini Live agent |
| `./bin/amuse config --key <KEY>` | Sets the Google AI Studio Gemini API key in `~/.amuse/config.json` |
| `./bin/amuse config --voice <VOICE>` | Configures the Gemini Live voice personality |
| `./bin/amuse test` | Tests local & cloud connectors |
| `./bin/amuse tools` | Prints all 30 Gemini function declaration schemas |
| `./bin/amuse call <tool> [args]` | Directly invokes any of the 30 native Muse tools with JSON arguments |
