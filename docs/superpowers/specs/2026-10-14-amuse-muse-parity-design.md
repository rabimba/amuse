# Amuse → Muse Parity Rebuild — Design & Master Plan

**Date:** 2026-10-14
**Status:** Approved by user (all sections), implementation in progress
**Goal:** Reimplement Meta Muse (https://ai.meta.com/muse) 1:1 in capability, self-hosted, running natively and efficiently on Linux and macOS, working with any LLM backend (first: Gemini API + OpenAI-compatible API).

This document is the single source of truth. A fresh session should read this file, then `AGENTS.md`, then the milestone tables below to know exactly what to do next.

---

## 1. What Muse is (research summary)

Sources: Meta launch post (about.fb.com 2026-09-08), "How We Designed Muse" (introducing.muse.ai), security architecture report (research.meta.ai, summarized by contextosai.com and forkast.news because the original blocks automated reads), TechCrunch (09-18 Mac app, 09-23 Connect announcements), Wikipedia, DataCamp, usecarly.com, and ~3,300 crowd-sourced real use cases (everyai-com/muse-use-cases capability pack; ~700 noise cases about an unrelated product were excluded), 156-case curated list (ezcat207/awesome-meta-muse), Tom's Guide Reddit roundup. The r/MuseAgent "26 use cases" thread itself could not be fetched (Reddit blocks scraping); substitutes above cover its content.

### Muse architecture (from Meta's security report, via third-party reviews)
- Per-user cloud **Secure VM**. Agent harness ("**Hatch**") runs in a `systemd-nspawn` **runtime cell** (root→unprivileged mapping, filtered syscalls, reduced capabilities, own Debian FS + veth).
- Host-side services outside the cell:
  - **Sentinel** — sole authority for connector actions + network egress; allow / deny / ask-user.
  - **hatch-authd** — credential store; agent only gets surrogate references; real secret inserted just-in-time at network boundary.
  - **privsep workers** — built-in connector logic runs in separate workers; worker identity (cgroup) tied to credential allowlists.
  - **hatch-safety** — independent classifiers around inference.
  - Durable state in PostgreSQL outside the cell; constrained proxies for inference/telemetry.
  - **eBPF taint tracking** (cgroup + LSM hooks): a process that read user data loses automatic egress privileges.
- Browser = controlled **browser subagent**: accessibility-tree snapshots only, no raw JS, no CDP; broker behind Sentinel. Email connector strips OTP codes / password-reset / magic links.
- **Mac app ("Endo")** is a device node: local connectors (Messages/iMessage, Notes, Calendar, Reminders, Contacts, Mail, Files, media), background sync to the VM, computer-use on a virtual display w/ PiP view + agent cursor + SoM/AX perception + blocked apps/sites, dictation into any app, double-Option summon, custom shortcuts, quick chat, video avatar, "Spaces" (webview overlays via custom scheme), onboarding + permissions UI.

### Muse user-facing features (parity checklist)
1. One long main chat + **side chats**; interruptible; multiple tasks at once; not turn-by-turn.
2. **Subagents**, scheduled jobs (cron), event triggers; keeps working after app closed.
3. **Proactive messages** with high bar; user-tunable; quiet hours.
4. **Goals tab** (plans, progress), **Ideas tab** (generated suggestions), **Feed**, **activity log** (audit trail), editable **memory files**, "forget" command.
5. **Approval cards** for sensitive/irreversible actions; default allows plain browsing.
6. **Artifacts**: documents, PDFs, slides, sheets, live dashboards, small web apps ("Spaces").
7. Connectors: Gmail, Google Calendar/Workspace, Outlook, Slack, GitHub, Notion, Granola, Spotify, Plaid, Shopify, QuickBooks, Asana, Box, Canva, Dropbox, Figma, HighLevel, Klaviyo, Stripe, Zoom, Ticketmaster, OpenTable, Peloton, Apple Health, Function Health, Facebook/Instagram, smart home. Custom API connectors built by the agent from user credentials; browser fallback when no API. Third-party dev connector platform.
8. **Outbound phone calls** (IVR navigation, hold, human handoff, transcripts).
9. Agent's **own email address**; **WhatsApp** channel; **payments** (one-time virtual cards via Stripe Link; Shop Pay; PayPal); 1Password support.
10. Custom name/avatar/personality; Realtime Avatar video chat; wake-word on glasses (hardware, out of scope).
11. MCP connectors (in the wild: Robinhood via MCP etc.).

### Use-case demand ranking (from 2,626 real cases)
Browser automation ~50% · bookings 22% · payments ~20% · email read 14–20% · research/quotes 12% · refunds/claims 12% · scheduled/recurring 10% · artifacts/dashboards 9% · approval gates 9% · social media 8% · documents 8% · bill negotiation 6–9% · subscription cancel 6.4% · gov forms 6% · finance data 6% · watchers 6% · code exec/VM 6% · phone calls 5.5% explicit · image understanding 5.4% · calendar 5% · marketplace selling 4.4% · proactive digests 3.4% · MCP 3% · audio/podcast 2.7% · subagents 2.6%. Native-Mac-app control <1%.

---

## 2. Decisions (approved by user 2026-10-14)

| # | Decision | Choice |
|---|---|---|
| D1 | Deployment topology | **Daemon + nodes, local-first.** `amused` agent-host daemon runs as launchd/systemd user service on the workstation; Mac/Linux apps are "nodes". Same daemon can later run on an always-on Linux box/VPS for true 24/7 background work. |
| D2 | Stack | **Hybrid.** `hatch` agent harness in **TypeScript/Node** (runs *inside* sandbox). Privileged host services in **Rust**: `amused` supervisor, `sentinel`, `authd`, sandbox launcher. macOS node app in **Swift**; Linux node app in **Rust + GTK4**. Shared web UI (TS, Preact/Svelte, Vite). |
| D3 | Sandbox | Linux: **bubblewrap + Landlock + seccomp + private netns**; optional eBPF taint hardening later. macOS: **Seatbelt (`sandbox-exec`) only**; taint enforced at task level by Sentinel (no eBPF on mac). |
| D4 | Action model | **Typed effects + sandboxed workspace.** Typed connector calls classified read/write/send/delete/spend/auth/exec gated by Sentinel; sandboxed shell/code/files/skills with all egress via Sentinel proxy; brokered browser subagent; node-routed local tools. External **MCP servers as a connector type** behind Sentinel. |
| D5 | v1 external services | IN: **messaging channels, password manager (1Password/Bitwarden), realtime video avatar**. DEFERRED (designed-for): phone calls, payments, agent email address, mobile/web client, Confidential-VM-at-rest. |

---

## 3. Target architecture

```
┌──────── Node apps: Amuse.app (Swift) / amuse-gtk (Rust+GTK4) ────────────┐
│ HUD (shared web UI) · voice + AEC · hotkey · dictation · avatar ·        │
│ local connectors (EventKit/EDS, Contacts, Messages, Notes, Files…) ·     │
│ desktop computer-use · notifications · permission onboarding             │
└─────────▲── JSON-RPC 2.0 over WebSocket (UDS now; TLS+auth when remote) ──┘
┌─────────┴───────── amused (Rust; launchd agent / systemd --user) ─────────┐
│ gateway · store (SQLite WAL, event-sourced) · scheduler · model proxy     │
│   (holds API keys, allowlisted provider endpoints, usage metering)        │
│ safety (injection classifiers, OTP/reset-link filter)                     │
│ SENTINEL: policy engine · approval broker · hash-chained ledger ·         │
│           egress proxy (per-process tokens, taint tracking)               │
│ AUTHD: vault (master key in Keychain/Secret Service; XChaCha20-Poly1305)  │
│        · surrogate tokens · OAuth refresh · 1Password/Bitwarden CLI       │
│ launcher → sandboxed:                                                     │
│   ├─ hatch (TS): agent loop, subagents, workspace shell/code/skills       │
│   ├─ privsep workers: one per connector (own creds + domains)             │
│   ├─ browserd (Playwright Chromium): a11y snapshots, autofill, PiP stream │
│   └─ channel workers: Telegram / WhatsApp / Signal / …                    │
└───────────────────────────────────────────────────────────────────────────┘
```

**Trust rules:** hatch holds no keys and no direct network. Linux cell: netns has only loopback → forwarder → Sentinel UDS; Landlock → workspace only. macOS cell: Seatbelt denies all network except proxy port + UDS; denies Apple Events, pasteboard, keychain, TCC services; file writes only workspace/tmp; no reads of `~/Library`, `~/.ssh`. Every external effect = typed request to Sentinel `{task, actor, tool, connector.method, action_class, target, payload_hash, summary, reversible, taint}` → allow/ask/deny.

**Data:** macOS `~/Library/Application Support/Amuse/`, Linux `$XDG_DATA_HOME/amuse/` containing `workspace/` (agent home; `memory/*.md` user-editable), `artifacts/`, `store.db`, `ledger/`. Import from legacy `~/.amuse`.

**Efficiency:** Rust host ~20MB; hatch ~100MB; Chromium/workers spawned on demand, idle-stopped; event-driven (no polling); audio native.

**Packaging:** macOS signed/notarized `.app` embedding amused + bundled node runtime + hatch (stable TCC identity; SMAppService login item). Linux deb/rpm/AUR + systemd user unit (no Flatpak — nested bwrap impossible).

### 3.1 Model/provider layer (hatch adapters + amused model proxy)
- Canonical internal format: message parts {text,image,audio,file,tool_call,tool_result,thinking}; JSON Schema tools; adapters convert (Gemini UPPERCASE types; keep Gemini thought signatures across turns).
- Providers v1: `gemini` (REST + Live WSS), `openai`-compatible (chat.completions+tools; optional Realtime, /audio/transcriptions, /audio/speech — covers OpenAI, Azure, OpenRouter, Groq, DeepSeek, Ollama, llama.cpp, vLLM, LM Studio, locally-served Muse Glimmer). Capability record per model {tools, parallel, vision, audio, ctx, realtime}; text-based tool-call shim for local models without native tools.
- Roles → `provider:model` + fallback chain: `agent`, `fast` (titles/summaries/memory extraction/proactivity/idea ranking), `vision`, `guard` (injection/sensitive-data classifier, may be small local model), `embed`, `image` (Gemini image models / /v1/images), `voice` (realtime S2S) or `stt`+`tts`.
- Voice = frontend only; realtime model delegates work to main brain via `delegate` tool (Muse's S2S→Hatch bridge pattern). Non-realtime path: native VAD (Silero/WebRTC) → STT (provider or local whisper.cpp Metal/Vulkan) → agent → streaming TTS (provider or local Kokoro/Piper). Barge-in: node detects speech during playback → `audio_output_clear` + response.cancel. Capture 16kHz mono, resample to 24k when provider requires; playback 24k.
- Keys: authd; header auth (never `?key=`); env vars `GEMINI_API_KEY`/`OPENAI_API_KEY`/`OPENAI_BASE_URL` override file config. Retries/backoff on 429/5xx; failover chain; real timeouts/cancellation; usage meter + budgets.

### 3.2 Harness (hatch)
- Event-sourced sessions (append-only event log in store = replay/resume + audit + UI stream source).
- Threads: `main` + side chats (own context, shared memory); inbox queue (multi-message); steer-at-step-boundary; Stop cancels.
- Tasks: `task.spawn(goal, scope, budget)` background subagents; per-task step/token/wall budgets; loop detection; structured tool errors; approval wait pauses only that task.
- Tool manifest: {name, description, schema, action_class, data_origin, binding, timeout, idempotency}. Progressive disclosure (`tools.search`, `connector.enable`). Shell `amuse <connector> <method>` CLI calls the same effect API (scripts get same policy).
- Context: persona+profile, retrieved memories, active goals, fresh clock every turn, capability summary; `fast`-model checkpoint summaries; big outputs → workspace files + excerpts; untrusted wrapping `<untrusted src=…>`.
- Memory: `workspace/memory/{profile,preferences,routines,people/*,journal/*}.md` + index (provenance event id, confirmed|inferred, sensitivity, expiry) + embeddings; turn-end `fast` extraction proposals; untrusted-sourced facts quarantined until confirmed; sensitive ask-first; `forget` = files+index+embeddings+tombstone; retrieval every turn.
- Persona: name/avatar/style/proactivity in `persona.toml` (editable via Settings or chat).
- Built-in subagents: browser, research, coder, connector-builder (writes custom API connectors from user creds; approval before first run).
- Self-built skills: `workspace/skills/<name>/` (manifest + TS/py/sh) run inside cell; lifecycle draft → approve → registered (hash-pinned; edit ⇒ re-approval; revocable).

### 3.3 Safety plane
- Sandbox profiles per class: `cell`, `worker:<connector>`, `browser`, `channel:<name>`.
  - Linux: bwrap (user/pid/mount/net ns), bundled read-only Debian rootfs (~150MB, versioned) + workspace overlay (pip/npm/apt-installable by agent; registries auto-allowed clean domains), seccomp (deny ptrace/mount/bpf/keyctl/kexec), Landlock workspace-only, no_new_privs, cgroup limits via `systemd-run --user`, netns loopback-only + proxy forwarder. Optional eBPF (aya) taint via privileged helper.
  - macOS: Seatbelt SBPL deny-default; exec allowlist (bundled python/node, /usr/bin); file-write workspace+/tmp; network tcp only 127.0.0.1 proxy + UDS; deny mach-lookup (appleevents, pasteboard, keychain, TCC); deny-read `~/Library`, `~/.ssh`, keychains; rlimits/taskpolicy; conformance test per macOS release (sandbox-exec deprecated risk).
- Sentinel policy (deterministic, stored host-side, editable only via UI): precedence deny > ask > allow. Defaults: read=allow; write/reversible=allow; send/delete/spend/auth/irreversible=ask. Standing permissions ("may reply to Mom on WhatsApp"), per-connector level (read-only|read+send), blocked sites/apps, rate limits, budgets. `guard` model may only escalate risk, never approve.
- Approval cards in HUD + native notifications + messaging channels (buttons); show normalized effect (recipient/body/amount/domain); approve once / always-this-pattern / edit / deny; approval bound to payload hash, executor re-verifies; bulk = one card listing N items; expiry. Description≠effect ⇒ reject + preserve evidence.
- Ledger: append-only hash-chained (requests, decisions, results, ambiguous outcomes; reconcile before retry); exportable.
- Egress proxy: per-process token attribution; auto-allow known-safe (package registries); taint ⇒ new dest or upload (POST to non-allowlisted) requires approval.
- Kill switch: hotkey/tray/channel → pause all tasks, revoke in-flight tokens.
- authd: master key in Keychain/Secret Service (fallback passphrase+argon2); XChaCha20-Poly1305 entries {secret, bound_domains, allowed_callers, scopes}; OAuth PKCE via system browser + loopback; surrogate `AMUSE_SECRET_…` placeholders substituted by proxy only on bound hosts (TLS intercept w/ local CA trusted only inside cell for those hosts); 1Password `op` / Bitwarden `bw` fetch-at-use; browserd autofill via CDP `Input.insertText` (value never observed; password fields masked in snapshots).
- Injection defenses: provenance labels everywhere; `guard` ensemble on web/email/files; OTP/reset/magic-link stripping in email unless in approved login flow; instruction hierarchy in system prompt; effects gated regardless of model belief.
- Legacy dangers removed: unsandboxed run_command, blind-focus typing, unconfirmed email send/attachments, passwords in argv/logs.

### 3.4 Connectors
- Manifest {kind: oauth|apikey|node|mcp|custom, scopes per level, methods (typed, action_class), allowed_domains, rate limits, sync capability}. Runtime: privsep worker per connector (TS + `@amuse/connector-sdk`), creds from authd, network limited to manifest domains.
- Local mirror (host-side store; SQLite FTS5 + embeddings) for search/triggers; incremental sync (Gmail historyId, Graph delta, CalDAV sync-token, IMAP IDLE, chat.db rowid watermark, EDS view signals). Nothing syncs unless connector enabled (Muse's iMessage incident is the cautionary tale).
- Fallback chain: built-in → connector-builder custom API → MCP → browser recipe.
- Catalog waves:
  - **A (core personal):** Gmail API, Microsoft Graph (Outlook), generic IMAP/SMTP (iCloud/Fastmail/O365), Thunderbird; Google Calendar, Graph Calendar, CalDAV, local EventKit/EDS; Google People, Graph contacts, CardDAV, local Contacts/EDS; Apple Notes+Reminders (mac), Markdown notes, Google Tasks, Todoist, Things; local files (granted folders), Drive, Dropbox, OneDrive; iMessage/SMS (mac node; decode attributedBody; send via Messages), Slack, Discord.
  - **B (Muse launch list):** Spotify, Notion, GitHub, Granola, Google Docs/Sheets/Slides, Strava, Oura, Peloton, Home Assistant (smart home), YouTube; finance via Plaid (dev account) or SimpleFIN; Apple Health via export-file import or Health Auto Export webhook (no Health store on Mac); OpenTable/Ticketmaster/Function Health via browser recipes.
  - **C (small business):** Asana, Box, Canva, Figma, HighLevel, QuickBooks, Klaviyo, Shopify, Stripe, Zoom, Facebook/Instagram Graph (business; personal via browser).
- Channels (v1): Telegram bot (long-poll, no public URL; approval buttons) · WhatsApp (official Cloud API needs business acct + public webhook via tunnel; unofficial Baileys linked-device is ToS-risky → opt-in w/ warning) · Signal (signal-cli linked device) · iMessage via mac node · Slack/Discord bots. Channels map to threads; carry approval buttons.
- MCP: stdio servers in worker sandbox; Streamable HTTP remote via egress proxy; OAuth via authd; action_class from tool annotations (readOnlyHint/destructiveHint/openWorldHint) + user override.
- OAuth setup wizard gotchas: Google — user creates own OAuth client, must publish "In production" (unverified) else refresh tokens die in 7 days; Microsoft — personal app registration.

### 3.5 Browser + desktop control
- browserd: Playwright Chromium/Chrome-channel in `browser` sandbox; persistent `browser-profiles/<identity>` (logins survive); CDP-screencast live view to HUD (both OSes, no virtual display needed); takeover = HUD mouse/keyboard forwarded (Input.dispatch*), agent pauses while user drives.
- Broker API (narrow): open, snapshot (a11y tree + stable numbered refs; optional screenshot + SoM marks for vision), click, type, select, scroll, wait, back, extract (markdown, untrusted-labeled), upload/download (workspace-mediated), fill_credential (authd). NO raw JS eval, NO raw CDP.
- Gating: navigation through proxy policy; uploads = send; irreversible detection (submit/checkout/Place-order/Send via heuristics + guard model) → effect request w/ screenshot + extracted amount/recipient → approval card. Plain browsing free (Muse default). CAPTCHA/2FA/3DS → pause + notify w/ live-view link → resume. Receipts: before/after screenshot + a11y diff per step → ledger.
- Bot detection: real Chrome channel, persistent profile, human timing; optional UA "AI agent" suffix setting (Amazon blocked Muse for not identifying).
- Desktop computer-use via node ("run your Mac"):
  - macOS: AX tree (AXUIElementCopyAttributeValue, budgeted walks) + ScreenCaptureKit window shots + SoM; act preferred via AX actions (AXPress/AXSetValue — background-safe); else CGEventPostToPid; foreground-only ops need "foreground permit" + visible agent cursor; pause while user typing (input idle). Experimental: private CGVirtualDisplay API + PiP.
  - Linux: AT-SPI2 (D-Bus) + screenshots (XShm / PipeWire ScreenCast portal); AT-SPI doAction/setText preferred; XTest w/ focus verification (X11) / RemoteDesktop portal or libei (Wayland); background mode = nested headless compositor (cage/weston-headless/Xvfb) streamed PiP.
  - Safety: every action = effect request; blocked apps deny-by-default (terminals, password managers, Keychain Access, System Settings); typing into terminal denied; never paste into unverified focus; Esc-Esc / corner-mouse stop; receipts.
- "Look at my screen": explicit user trigger only + capture indicator; optional 1fps share during voice (Live/Realtime video input); frame joins same turn (fixes 400ms race).

### 3.6 Autonomy
- Goals tab entity {title, why, success criteria, plan steps, linked tasks/schedules/artifacts, progress log}; per-goal schedule: review → next steps → update → notify-decision.
- Scheduler in amused, SQLite-persisted: RRULE/cron w/ TZ, one-shot, intervals; task template + tool scope + budget + report thread + notify policy; catch-up on wake (run-once|skip); optional keep-awake while pending (caffeinate/systemd-inhibit).
- Triggers: connector sync events (email match, message from X, event starting ±offset), local webhook endpoint (LAN/Tailscale), file watches. Deterministic filter → optional fast-model check → task.
- Watchers (monitor→detect→act): {target URL/connector query, extract, condition, interval, expires, action notify|act(approval)} + heartbeat cadence + failure alert after N errors + final proof-of-state + dedup.
- Proactive engine: candidates (task completions, triggers, goal runs, ideas, watchers) → rules + fast-model scoring (novelty, urgency, proactivity dial off|low|normal|high, quiet hours, user-busy from Focus/DND/fullscreen/input-idle) → chat bubble / native notification w/ actions / channel / digest.
- Ideas tab: daily + onboarding idea generation from memory/goals/connectors + 12 reusable loops from use-case corpus; Run button; dismiss/run feedback tunes.
- Feed v1: digest cards, watcher heartbeats, RSS/news sources, connector highlights, morning-briefing artifact.
- Onboarding "Chief of Staff" built-in task: review connections → learn read-only (90d email/calendar) → brief (facts/inferences/gaps) → ~20 tailored questions (5 at a time) → draft delegation rules (written to Sentinel only via UI confirmation) → weekly self-review job (reviews ledger/outcomes/corrections; proposes memory/policy tweaks; policy changes require approval). Nothing sent/changed during onboarding.

### 3.7 Artifacts (Spaces)
- Types: document (md→PDF/DOCX), PDF, sheet (csv/xlsx), slides, image, audio (TTS podcast), video (ffmpeg), code project; Space = small web app (HTML/JS/CSS); Dashboard = Space + data bindings + refresh schedule.
- Tools `artifact.create/update/snapshot`; `workspace/artifacts/<id>/` + manifest (type/title/entry/version/datasources/refresh); content-versioned rollback.
- Cell toolchain: Typst (fast PDFs) + browserd print-to-pdf; pandas, matplotlib, openpyxl, python-docx, python-pptx, ffmpeg. Images via `image` model role.
- Viewer: HUD/full-window via custom scheme `amuse-artifact://<id>/` (WKURLSchemeHandler / WebKitGTK URI scheme — mirrors EndoSpacesSchemeHandler); CSP no-network default; JS bridge `window.amuse` = read own data snapshot / request refresh / send message to agent only; artifact buttons → message → normal Sentinel flow.
- Live dashboards: scheduled refresh job runs cell script pulling via connectors (policy-gated) → writes `data/*.json`; viewer reloads.
- Chat: artifact cards w/ offscreen-rendered thumbnails. Sharing v1: export static bundle/PDF; publish (GitHub Pages/Cloudflare) as approved connector effect.

### 3.8 Native apps
- macOS Amuse.app (Swift/AppKit+SwiftUI): menu bar item; HUD panel that CAN take key focus (fixes borderless-panel typing); quick chat + full window. Shared web UI (TS+Preact/Svelte+Vite from bundle); `WKScriptMessageHandlerWithReply`; node app holds gateway connection (token in Keychain + peer-cred check; NO unauthenticated localhost endpoints — Muse's dictation-hijack bug lesson). Audio: AVAudioEngine voice-processing (AEC/NS), Silero/WebRTC VAD, correct Int16↔Float32 playback conversion (fixes today's mixer bug), barge-in output clear. Hotkeys: double-Option + custom recorder + PTT. Dictation into any app: hold chord → streaming STT → AXSelectedText insertion (fallback paste+restore clipboard). Local connectors: EventKit (with real requestAccess + Info.plist strings), Contacts, Messages (FDA + AppleScript send), Notes/Mail (AppleScript/ScriptingBridge), Files (security-scoped bookmarks), Shortcuts run (covers HomeKit), Photos (optional). Permission onboarding w/ live TCC status + deep links. SMAppService login item; Sparkle updates; notifications w/ actions.
- Linux amuse-gtk (Rust gtk4-rs+libadwaita+webkit6): tray (StatusNotifierItem); HUD gtk4-layer-shell (Wayland) / keep-above (X11). PipeWire capture + webrtc-audio-processing AEC + VAD. Hotkeys: XInput2 (X11, port today's) / GlobalShortcuts portal (Wayland; no double-tap-modifier → configurable shortcut or opt-in evdev/input-group). Dictation: AT-SPI / libei (Wayland) / XTest (X11). Connectors: EDS D-Bus (calendar/contacts/tasks), GNOME Online Accounts reuse, Thunderbird, portal file access, freedesktop notifications w/ actions, MPRIS any player.
- Shared UI views: Chat (bubbles, out-of-turn, working-on line, approval cards, browser live-view PiP) · Activity · Goals · Ideas · Feed · Artifacts · Memory (edit files) · Connectors (levels) · Settings (providers/roles, voice, persona, proactivity/quiet hours, policies/blocked lists, usage meter). Markdown render, selectable text, a11y labels.
- Avatar (behind one renderer interface): Tier1 default = rigged VRM (three.js three-vrm) or Rive/Live2D; states idle/listening/thinking/speaking/working; lipsync from amplitude or TTS viseme timings; user picks/imports; 30fps only while visible. Tier2 (in scope v1) = single-portrait talking-head (MuseTalk/LivePortrait/Ditto) on local Metal(MPS)/CUDA `avatar` service or remote streaming provider; video-call mode only (full window, voice, optional camera input to realtime model).

### 3.9 Repo layout, testing, migration
```
crates/      amused · sentinel · authd · sandbox (bwrap/Landlock/seccomp, Seatbelt profiles)
             amuse-proto · amuse-gtk
apps/macos/  Amuse.app (Swift)
packages/    hatch · providers · connector-sdk · connectors/* · browserd · channels/* · ui · cli
rootfs/      Debian base-system build for the Linux sandbox
tests/scenarios/   end-to-end scenario suite
legacy/      today's src/, deleted once the new build matches it
```
Protocol defined once (JSON Schema) → generated Rust/TS/Swift types (stops IPC drift).

Testing (CI added; tests must FAIL on failure):
- cargo test / vitest / XCTest.
- Provider conformance: recorded fixtures + mock servers (Gemini REST+Live, OpenAI chat+Realtime).
- Sandbox escape suite (both OSes): no raw socket, no ~/.ssh, no writes outside workspace, no authd store read, blocked binaries denied.
- Sentinel: policy matrix, approval-hash binding, taint transitions; injection→action w/ malicious fixtures.
- Scenario E2E (hermetic; fake Gmail/Calendar + local fixture sites for shop-checkout/support-chat/appointment; scripted approvals; run under both providers — recorded in CI, live nightly opt-in; separate AMUSE_HOME test dir). Scenarios from the 12 loops + launch demos (school-email→family-calendar; subscription audit; price watch; refund chase; trip-plan artifact).
- `docs/parity.md`: Muse capability → milestone → test mapping.

Migration from legacy:
- Port: macOS EventKit/Contacts/chat.db logic → Swift app; XInput2 hotkey → GTK app; PulseAudio → PipeWire; email_connector.py → TS IMAP/SMTP connectors; browser_agent.py → browserd; geminiLiveClient → providers/gemini.
- Drop: fake seed data, hardcoded personal data.
- Data: import real memories/notes from local_data.db; email_accounts.json → authd then delete plaintext; custom_tools/ → draft skills requiring approval.

---

## 4. Current-state findings (from 2026-10-14 audit, file:line)

**macOS (9 tools, all read-mostly; real):** LocalConnectorsManager.m:121-228 decl, :27-119 dispatch. search_messages/get_recent_messages (chat.db read; misses attributedBody-only texts) · search_contacts (never requests permission) · get_calendar_events/create_calendar_event (:72-73 unparseable ISO silently creates at "now") · get_reminders/create_reminder (:88-90 no POSIX locale) · search_notes/create_note (NotesConnector.m:66 `paragraph 1 of nText` outside try → one empty note fails whole search). NO memory/time/screenshot/alarms/email/files/shell/social tools on mac.
**macOS IPC bugs:** main.m:131-141 stdout reader drops partial lines (audio lost) · main.m:151-163 sendToNode no lock · no transcript_delta handling · tool chips lack toolName (:206) · HUD buttons (Goals/Library/Connectors/Screen/drag) dead on mac · AmuseHUD.m:94-99 escapes only `'` · main.m:22 bundle path look for Contents/Resources/web but no bundle built.
**Linux (33 tools):** FAKE/stub: messages (seeded :2171-2180), send_message (nothing sent :2393), calendar (seeded :2209, ignores days_ahead :2505), create_calendar_event ICS missing DTSTART/DTEND :1898, reminders (seeded :2218), contacts (seeded :2198 + real Thunderbird), connect_service cosmetic :2295, "20 connectors" = seeded rows :2122-2148. REAL: time, alarms (in-process, 24h cap :2674, unparseable→60s silent fire :1856, not persisted), notes (SQLite→md but clobbers disk edits :1862/:2074/:2785), memory, plans (CRUD only), email_action (Thunderbird+IMAP/SMTP via scripts/email_connector.py; sends unconfirmed; any-file attachment), connect_email_account (plaintext ~/.amuse/email_accounts.json; password in argv + stderr log :4282), screencast/screenshot (ffmpeg x11grab; screenshot `streamed:true` hardcoded :3256; 400ms vision race :3225), manage_files (no path restriction :3269), computer_control_action (popen arbitrary :3347), web_search_and_fetch (SSRF; broken grep -P :3363), schedule_task (declared command type does nothing :3427), build_and_run_custom_tool (unsandboxed popen :3620; tools never registered; no timeout), media control (pactl; MPRIS hardwired spotify), browser_agent_action (headless Playwright, no profile; file upload exfil), gui_desktop_action (XTest blind :3704; backspace bug :339).
**Linux systemic:** global recursive dbMutex_ held during whole tool run (:1489) → HUD freezes · spawnDetachedProcess waitpid(WNOHANG) → zombies (:218) · clipboard python3 -c splice injection (:260-264) · tool_call_response embeds raw JSON → truncation breaks IPC (:4288) · runDirectTool dead-but-dangerous (:4047) · tool args+passwords logged stderr (:4282) · AMUSE_STARTUP_PROMPT/EXIT env only on Linux → run.sh step6 hangs on mac.
**Node:** config.mjs:24-29 fileConfig spread last → env never overrides · geminiLiveClient.mjs:349 REST hardcoded gemini-2.5-flash · :435-438 REST tool timeout fake success · :506-537 unconfirmed social auto-chain w/ canned marketing text + "linekdin" typo · system prompt :162-176 hardcodes personal emails/counts + "33 tools" claim + Linux notes rule on mac · no interrupted/goAway/toolCallCancellation handling · Live context lost on reconnect; REST history 16 entries, no tool rounds · index.mjs:80 console.log pollutes stdout IPC.
**Tests:** test_e2e.mjs always prints PASS (stepPassCount unchecked :182); hits real personal data; depends on seeded fake rows ("Decompile" :2174). test_live_e2e.mjs REST-only, wrong model names (:38-43), TTS-fail still passes. bin/amuse test greps only. run.sh writes real state (save_memory, custom tool, screenshot, social schedule :53-74).
**README:** stale (30 tools, gemini-3.1-flash-live-preview, "VM", buttons).

---

## 5. Milestones (each = spec → plan → implement cycle; M0/M1 planned in detail below)

| # | Milestone | Done when |
|---|---|---|
| M0 | Legacy hardening | nothing executes code unconfirmed; no fake/personal data; tests honest; mac IPC fixed |
| M1 | Skeleton: monorepo, proto, amused (gateway/store/model proxy), hatch loop (text+tools) on Gemini + OpenAI-compat, CLI chat | same scenario passes on both providers, mac+linux |
| M2 | Safety core: sandbox launcher, Sentinel (policy/approvals/ledger/egress/taint), authd | escape suite green; every effect logged |
| M3 | Native apps v1: shared UI (chat/approvals/activity/settings), voice (realtime + cascade), hotkey, notifications, wave-A local connectors | legacy/ deleted |
| M4 | Wave-A cloud connectors, MCP, Telegram, 1Password/Bitwarden | |
| M5 | browserd (live view/takeover/gating), connector-builder subagent | |
| M6 | Autonomy: goals, scheduler, triggers, watchers, proactive, ideas, feed, onboarding, memory lifecycle, side chats/tasks UI | |
| M7 | Artifacts/Spaces/dashboards, image role | |
| M8 | Desktop computer-use mac+linux, dictation, avatar tier 1 | |
| M9 | WhatsApp/Signal/iMessage/Slack/Discord channels; wave B/C connectors; avatar tier 2; eBPF hardened mode | |

Deferred: phone calls, payments, agent email address, mobile/web client, Confidential-VM-at-rest.

---

## 6. M0 implementation plan (legacy hardening) — status tracker

Legend: `[ ]` todo · `[x]` done. File references relative to repo root.

### M0.1 config fix (`src/js/config.mjs`)
- [ ] env `GEMINI_API_KEY`/`GEMINI_MODEL` override file values; model normalization runs AFTER merge.

### M0.2 system prompt + client (`src/js/geminiLiveClient.mjs`)
- [ ] Remove hardcoded personal emails/counts (lines ~169-176) → generic ("the user's connected email accounts").
- [ ] Replace "33 native tools" claim with actual count passed in.
- [ ] Linux notes rule only when platform is linux (pass platform into prompt builder).
- [ ] Disable social auto-chain (lines ~506-537) behind `AMUSE_AUTO_SOCIAL=1` (default OFF).
- [ ] REST tool timeout (:435-438) returns `{status:"error", error:"tool_timeout"}` instead of fake completed.
- [ ] REST auth via `x-goog-api-key` header instead of `?key=` (keep `?key=` only for WSS).

### M0.3 IPC pollution (`src/js/index.mjs`)
- [ ] :80 console.log → console.error.

### M0.4 macOS IPC (`src/native/main.m`)
- [ ] stdout reader (:131-141): accumulate buffer, split lines, keep partial.
- [ ] sendToNode (:151-163): add NSLock.

### M0.5 macOS connectors
- [ ] NotesConnector.m:66: wrap `paragraph 1 of nText` in try.
- [ ] LocalConnectorsManager.m:72-73: return error when date unparseable (no silent "now").

### M0.6 Linux safety gates (`src/native/linux/main_linux.cpp`)
- [ ] `computer_control_action run_command` → require `AMUSE_ALLOW_SHELL=1` else error w/ instructions.
- [ ] `build_and_run_custom_tool` → require `AMUSE_ALLOW_CUSTOM_TOOLS=1`; add 30s timeout (alarm/poll).
- [ ] `gui_desktop_action` (click/type/paste) → require `AMUSE_ALLOW_GUI_CONTROL=1`.
- [ ] `social_media_action auto_submit` → require `AMUSE_ALLOW_AUTO_SUBMIT=1`.
- [ ] `email_action send` → require `confirm_send:true` param (else return preview requiring confirmation); attachments limited to $HOME minus denylist (~/.ssh ~/.gnupg ~/.aws ~/.amuse ~/Library).
- [ ] `manage_files` → deny paths outside $HOME (except /tmp), deny denylisted dirs, deny overwriting dotfiles directly under $HOME.
- [ ] `web_search_and_fetch` → block localhost/RFC1918/link-local targets.
- [ ] `list_connectors` → bind category with sqlite3_prepare/bind (no concat).
- [ ] `connect_email_account` → password via stdin JSON to python (not argv); never log arg values for keys containing "pass"/"token"/"secret" (:4282 sanitizer).
- [ ] `scripts/email_connector.py` → read args from stdin when argv[1] == "-".

### M0.7 Linux correctness
- [ ] Delete fake seed inserts (:2150-2225 memories/messages/notes/contacts/calendar/reminders/plans seeds). Keep schema. (Tests must create own fixtures.)
- [ ] Notes: only write .md when content differs; skip syncAllNotesToDisk in `call` CLI mode.
- [ ] Alarm: unparseable time → error (no 60s silent fire); re-arm persisted alarms at daemon start.
- [ ] take_screenshot: set `streamed_to_gemini_vision` from actual send result.
- [ ] Clipboard fallback (:260-264): pass text via stdin, not string splice.
- [ ] signal(SIGCHLD, SIG_IGN) at startup (zombie fix :218).
- [ ] tool_call_response (:4288): send output as JSON string (consistent w/ mac), never raw-embed.

### M0.8 Honest tests
- [ ] `scripts/test_e2e.mjs`: real assertions (tool response received, transcript non-empty, audio chunk received); exit 1 on failure; run bridge calls with `HOME=<tmpdir>`; create fixtures (create_note) in tmp HOME; no dependence on fake seeds.
- [ ] `bin/amuse test`: same temp-HOME approach; check JSON `status` fields; exit non-zero on failure.
- [ ] `scripts/test_live_e2e.mjs`: model names from config default; TTS failure ≠ pass; exit code reflects failures.

### M0.9 Docs + UI de-personalization
- [ ] `src/web/index.html:66`, `src/web/app.js:167`, `main_linux.cpp:1144`: remove hardcoded personal stats/emails.
- [ ] README.md: honest tool counts (9 mac read-mostly / 33 linux w/ stubs listed), correct default model, safety flags table, known-limitations section, pointer to this spec.
- [ ] AGENTS.md: safety flags, honest test description, roadmap pointer.

### M0 verify + commit
- [ ] `npm run build` clean (mac).
- [ ] `npm run test:e2e` green.
- [ ] `bin/amuse call` smoke: search_messages, create_note (tmp HOME), gated tools refuse without flags.
- [ ] Commit `feat(...)`: M0 hardening.

## 7. M1 implementation plan (skeleton)

- [ ] Monorepo layout (crates/, packages/, apps/, rootfs/, tests/) added alongside legacy (legacy stays until M3).
- [ ] `packages/proto`: JSON Schema protocol + TS/Rust typegen (message parts, tool manifest, effect request, gateway JSON-RPC).
- [ ] `crates/amused`: minimal — config load, gateway WS (UDS), store (SQLite WAL, event log append/read), model proxy (header auth injection for gemini+openai endpoints only), spawns hatch.
- [ ] `packages/hatch`: provider adapters (gemini REST generateContent + stream; openai chat.completions + stream), canonical message format, tool schema conversion, agent loop (turn → tool calls → results → final), tool execution via LEGACY bridge CLI (`bin/amuse-bridge call`) as temporary backend, CLI chat entry.
- [ ] `packages/cli`: `amuse chat`, `amuse call <tool>`.
- [ ] Scenario smoke test: "create a note titled X with body Y, then read it back" runs under both providers (recorded/mocked in CI; live opt-in) on mac.
- [ ] AGENTS.md updated for monorepo.
