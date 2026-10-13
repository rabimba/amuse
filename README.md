# Amuse (Agent Muse) 🔮

**Amuse** is a personal AI agent for macOS powered by the **Google Gemini Multimodal Live API** through **Google AI Studio**.

It is inspired by the architecture and reverse engineering of Meta's Muse (Project "Hatch"), featuring a floating liquid-glass HUD, real-time duplex voice, global double-tap Option summoning, and zero-cloud-sync local Apple ecosystem grounding.

---

## Architecture Overview

```
                      ┌──────────────────────────────────────┐
                      │            User Interface            │
                      │   - Double-Tap Option Key Monitor    │
                      │   - Liquid Glass Floating HUD        │
                      │   - Pulsing Reactive Avatar Orb      │
                      └──────────────────┬───────────────────┘
                                         │
                   ┌─────────────────────┴─────────────────────┐
                   ▼                                           ▼
┌──────────────────────────────────────┐     ┌──────────────────────────────────┐
│      Native macOS Engine (Cocoa)     │     │      Gemini Live Client (Node)   │
│ - AudioEngine (16kHz in / 24kHz out) │◄───►│ - Bi-directional WebSocket API   │
│ - HotkeyMonitor (Double-tap Option)  │     │ - Real-time Full Duplex Audio    │
│ - WebKit HUD Container               │     │ - Function Calling Dispatch Loop │
└──────────────────┬───────────────────┘     └─────────────────┬────────────────┘
                   │                                           │
                   ▼                                           ▼
┌──────────────────────────────────────┐     ┌──────────────────────────────────┐
│        Local Apple Connectors        │     │         Google AI Studio         │
│ - iMessages (~/Library/Messages)     │     │   - gemini-2.0-flash-exp         │
│ - Apple Notes (AppleScript)          │     │   - Natural Multimodal Voices    │
│ - Apple Calendar & Reminders         │     │     (Puck, Charon, Kore, etc.)   │
│ - macOS Contacts (Contacts.framework)│     └──────────────────────────────────┘
└──────────────────────────────────────┘
```

---

## Features

- **Ambient Floating HUD**: Translucent liquid-glass panel (`NSVisualEffectView` + WebKit) that floats above all apps without stealing active focus.
- **Global Hotkey Summoning**: Double-tap the `Option` key anytime to summon or dismiss Amuse.
- **Real-Time Duplex Voice**: Direct integration with the Gemini Multimodal Live API (`v1alpha.GenerativeService.BidiGenerateContent`) for sub-second, natural voice conversation with interruption handling.
- **Local Apple Data Grounding**:
  - **iMessage**: Reads `~/Library/Messages/chat.db` (SQLite) directly to search and retrieve text messages and contacts.
  - **Apple Notes**: Searches, reads, and creates notes in Notes.app.
  - **Apple Calendar**: Reads upcoming schedules and creates calendar events.
  - **Apple Reminders**: Checks pending to-dos and creates reminders.
  - **Contacts**: Resolves phone numbers, emails, and names.
- **Tool Calling Execution Loop**: When Gemini needs your schedule or messages, it invokes the local tool, receives the data in milliseconds, and speaks the answer naturally.

---

## Quick Start

### 1. Build the Native Bridge
```bash
./scripts/build.sh
```

### 2. Configure Your Gemini API Key
Get your free API key from [Google AI Studio](https://aistudio.google.com/):
```bash
./bin/amuse config --key YOUR_GEMINI_API_KEY
```

Optional: Select a voice profile (`Puck`, `Charon`, `Kore`, `Fenrir`, `Aoede`):
```bash
./bin/amuse config --voice Puck
```

### 3. Run System Diagnostics
Verify that your local Apple connectors are responding:
```bash
./bin/amuse test
```

### 4. Launch Amuse
```bash
./bin/amuse
```
- Press **Double-tap Option (`⌥⌥`)** anytime to toggle the HUD.
- Click **Start Voice** (or speak into the mic) to talk with Gemini in real time.
- Or type questions in the bottom bar like:
  - *"What are my recent iMessages?"*
  - *"Create a note titled Project Ideas with bullet points for our AI agent."*
  - *"Do I have any meetings scheduled this week?"*

---

## CLI Commands

| Command | Description |
|---|---|
| `./bin/amuse` | Launches the floating HUD and background Gemini Live agent |
| `./bin/amuse config --key <KEY>` | Sets the Google AI Studio Gemini API key in `~/.amuse/config.json` |
| `./bin/amuse config --voice <VOICE>` | Configures the Gemini Live voice personality |
| `./bin/amuse test` | Tests all local connectors (iMessage, Notes, Calendar, Contacts) |
| `./bin/amuse tools` | Prints Gemini function declaration schemas |
| `./bin/amuse call <tool> [args]` | Directly invokes a local connector tool with JSON arguments |

---

## Project Structure

- `src/native/connectors/`: Native macOS connectors (`MessagesConnector`, `NotesConnector`, `CalendarConnector`, `ContactsConnector`, `LocalConnectorsManager`).
- `src/native/AudioEngine.m`: 16kHz linear PCM audio capture and 24kHz buffer playback engine.
- `src/native/HotkeyMonitor.m`: Double-tap `Option` global event monitor.
- `src/native/AmuseHUD.m`: Liquid-glass floating HUD window controller.
- `src/js/geminiLiveClient.mjs`: WebSocket client for Gemini Multimodal Live API.
- `src/js/index.mjs`: Orchestrator connecting the native host with Gemini.
- `src/web/`: Web workspace containing the animated status orb, chat transcript, and controls.
- `bin/amuse`: CLI runner and manager.
