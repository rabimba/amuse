import { EventEmitter } from 'node:events';
import WebSocket from 'ws';

export class GeminiLiveClient extends EventEmitter {
  constructor(config, toolDeclarations = [], startupMemories = '') {
    super();
    this.config = config;
    this.toolDeclarations = toolDeclarations;
    this.startupMemories = startupMemories;
    this.ws = null;
    this.isConnected = false;
    this.isSetupComplete = false;
    this.pendingToolNames = new Map();
    this.inputTranscriptBuffer = '';
    this.outputTranscriptBuffer = '';
  }

  connect() {
    if (!this.config.apiKey) {
      this.emit('status', 'idle', 'Please set your GEMINI_API_KEY using "amuse config --key <KEY>" or in ~/.amuse/config.json');
      return;
    }

    const host = 'generativelanguage.googleapis.com';
    const path = '/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent';
    const url = `wss://${host}${path}?key=${this.config.apiKey}`;

    this.emit('status', 'thinking', 'Connecting to Gemini Live API...');

    try {
      this.ws = new WebSocket(url);
    } catch (err) {
      this.emit('status', 'idle', `Connection failed: ${err.message}`);
      return;
    }

    this.ws.onopen = () => {
      this.isConnected = true;
      this.sendSetup();
    };

    this.ws.onmessage = (event) => {
      this.handleServerMessage(event.data);
    };

    this.ws.onerror = (err) => {
      console.error('[GeminiLive] WebSocket Error:', err.message || err);
      this.emit('status', 'idle', 'Connection error. Check API key.');
    };

    this.ws.onclose = (event) => {
      this.isConnected = false;
      this.isSetupComplete = false;
      this.emit('status', 'idle', `Disconnected (${event.code}). Double-tap Option to reconnect.`);
    };
  }

  getFormattedClockContext() {
    const now = new Date();
    const tz = Intl.DateTimeFormat().resolvedOptions().timeZone || 'Local';
    const fmt = (timeZone) => {
      try {
        return new Intl.DateTimeFormat('en-US', {
          timeZone,
          weekday: 'short',
          year: 'numeric',
          month: 'short',
          day: '2-digit',
          hour: '2-digit',
          minute: '2-digit',
          second: '2-digit',
          hour12: true,
          timeZoneName: 'short'
        }).format(now);
      } catch {
        return now.toString();
      }
    };
    const localStr = fmt(tz);
    const pacificStr = fmt('America/Los_Angeles');
    const centralStr = fmt('America/Chicago');
    const easternStr = fmt('America/New_York');
    const utcStr = now.toISOString();
    return `Local (${tz}): ${localStr} | Pacific (PT): ${pacificStr} | Central (CST/CDT): ${centralStr} | Eastern (ET): ${easternStr} | UTC: ${utcStr}`;
  }

  sendSetup() {
    const isLinux = process.platform === 'linux';
    const homeDir = process.env.HOME || '~';
    const clockContext = this.getFormattedClockContext();

    const systemPrompt = [
      `You are Amuse (with full Meta Muse feature parity), a proactive, agentic personal AI assistant running natively on the user's ${isLinux ? 'Ubuntu Linux desktop (X11/GNOME)' : 'macOS desktop'}.`,
      `CURRENT REAL-TIME SYSTEM CLOCK: ${clockContext}.`,
      `IMPORTANT TIME RULE: Always use the exact clock above (or call get_current_time_and_environment) when reasoning about times and timezones (such as 9 AM CST/CDT vs local time). Never guess or hallucinate whether a time has already passed.`,
      `IMPORTANT LINUX NOTES RULE: On this Linux machine, there is NO Apple "Notes app". Whenever you create or search notes with create_note, search_notes, or open_note, notes are saved as real Markdown files in ${homeDir}/Documents/AmuseNotes/ (and ${homeDir}/Notes/) and displayed as cards inside the Amuse HUD. Never tell the user to "open your Notes app"—instead, tell them the note is saved in ${homeDir}/Documents/AmuseNotes/ and displayed in the HUD, and offer to open it on their desktop with open_note.`,
      `PROACTIVE MUSE CONNECTORS & TOOLS RULE: You have 30 native tools across 20+ local and cloud connectors. Whenever the user asks you to perform a task, AUTOMATICALLY connect and use your tools instead of saying "I can't do that":`,
      `- Social Media & LinkedIn (e.g. "make or schedule a post on LinkedIn at 9 AM CST"): Call social_media_action and/or schedule_task to auto-connect LinkedIn, draft/schedule the post, copy to clipboard, or open the LinkedIn post composer.`,
      `- Screencasts, Screenshots & Live Screen Vision (e.g. "create/record a screencast", "take a screenshot", "look at my screen"): Call record_screencast or take_screenshot. Taking a screenshot also streams the screen image into your multimodal vision so you can see and describe what is on the user's screen!`,
      `- Alarms & Timers (e.g. "set an alarm for 9 AM CST" or "set a timer for 5 minutes"): Call set_alarm_or_timer. You CAN set real system alarms and timers with audio chimes and critical desktop notifications.`,
      `- Active Memory (e.g. remembering user preferences, habits, facts, projects): Call save_memory and recall_memories.`,
      `- Action Plans & Multi-Step Goals (e.g. travel planning, launch campaigns, project tracking): Call manage_action_plan to create and track multi-step plans.`,
      `- Dynamic Custom Tool Builder: If a task needs a custom script, calculation, scraper, or automation, call build_and_run_custom_tool to write and run a Python or Bash tool on the fly.`,
      `- Media, Volume & System Control (e.g. Spotify/music playback, system volume, active window inspection): Call media_and_system_control or computer_control_action.`,
      `- Notes, Calendar, Reminders, Messages, Contacts, Email, Files & Web Search: Use create_note, open_note, search_notes, create_calendar_event, create_reminder, send_message, email_action, manage_files, web_search_and_fetch, list_connectors, connect_service.`,
      this.startupMemories ? `ACTIVE MEMORY (Persisted User Facts & Preferences):\n${this.startupMemories}` : '',
      `Be concise, warm, action-oriented, and natural in spoken conversation.`
    ].filter(Boolean).join('\n');

    const setupMsg = {
      setup: {
        model: this.config.model || 'models/gemini-2.5-flash-native-audio-latest',
        generationConfig: {
          responseModalities: ['AUDIO'],
          speechConfig: {
            voiceConfig: {
              prebuiltVoiceConfig: {
                voiceName: this.config.voice || 'Puck'
              }
            }
          }
        },
        inputAudioTranscription: {},
        outputAudioTranscription: {},
        systemInstruction: {
          parts: [
            {
              text: systemPrompt
            }
          ]
        },
        tools: [
          {
            functionDeclarations: this.toolDeclarations
          }
        ]
      }
    };

    this.ws.send(JSON.stringify(setupMsg));
  }

  sendAudioChunk(base64PcmData) {
    if (!this.isConnected || !this.isSetupComplete) return;

    const msg = {
      realtimeInput: {
        mediaChunks: [
          {
            mimeType: 'audio/pcm;rate=16000',
            data: base64PcmData
          }
        ]
      }
    };

    this.ws.send(JSON.stringify(msg));
  }

  sendImageFrame(base64ImageData, mimeType = 'image/png') {
    if (!this.isConnected || !this.isSetupComplete || !base64ImageData) return;

    const msg = {
      realtimeInput: {
        mediaChunks: [
          {
            mimeType,
            data: base64ImageData
          }
        ]
      }
    };

    this.ws.send(JSON.stringify(msg));
  }

  sendTextMessage(text) {
    if (!this.isConnected) {
      if (!this.ws || this.ws.readyState > 1) {
        this.connect();
      }
      this.once('ready', () => this.sendTextMessage(text));
      return;
    }
    if (!this.isSetupComplete) {
      this.once('ready', () => this.sendTextMessage(text));
      return;
    }

    const msg = {
      clientContent: {
        turns: [
          {
            role: 'user',
            parts: [
              { text: `[System Clock: ${this.getFormattedClockContext()}]\n${text}` }
            ]
          }
        ],
        turnComplete: true
      }
    };

    this.ws.send(JSON.stringify(msg));
    this.emit('status', 'thinking', 'Thinking...');
  }

  sendToolResponse(callId, outputData, toolName = '') {
    if (!this.isConnected) return;

    let outputObj = outputData;
    if (typeof outputData === 'string') {
      try {
        outputObj = JSON.parse(outputData);
      } catch {
        outputObj = { output: outputData };
      }
    }

    const resolvedName = toolName || this.pendingToolNames.get(callId) || '';
    const fnResp = {
      id: callId,
      response: {
        output: outputObj
      }
    };
    if (resolvedName) {
      fnResp.name = resolvedName;
    }

    const msg = {
      toolResponse: {
        functionResponses: [fnResp]
      }
    };

    this.ws.send(JSON.stringify(msg));
  }

  handleServerMessage(raw) {
    if (typeof Blob !== 'undefined' && raw instanceof Blob) {
      raw.text().then((text) => this.handleServerMessage(text));
      return;
    }

    let msg;
    try {
      const text = typeof raw === 'string' ? raw : Buffer.from(raw).toString('utf8');
      msg = JSON.parse(text);
    } catch (err) {
      console.error('[GeminiLive] Failed to parse server message:', err);
      return;
    }

    // 1. Setup complete response
    if (msg.setupComplete) {
      this.isSetupComplete = true;
      this.emit('status', 'idle', 'Connected. Speak or type a request.');
      this.emit('ready');
      return;
    }

    // 2. Server Content (Audio / Text / Transcriptions)
    if (msg.serverContent) {
      if (msg.serverContent.inputTranscription?.text) {
        this.inputTranscriptBuffer += msg.serverContent.inputTranscription.text;
      }

      if (msg.serverContent.outputTranscription?.text) {
        if (this.inputTranscriptBuffer.trim()) {
          this.emit('transcript', 'user', this.inputTranscriptBuffer.trim());
          this.inputTranscriptBuffer = '';
        }
        this.outputTranscriptBuffer += msg.serverContent.outputTranscription.text;
      }

      const modelTurn = msg.serverContent.modelTurn;
      if (modelTurn && modelTurn.parts) {
        for (const part of modelTurn.parts) {
          // Audio output
          if (part.inlineData && part.inlineData.mimeType?.startsWith('audio/pcm')) {
            this.emit('audioOutput', part.inlineData.data);
          }
          // Text transcript (ignore internal chain-of-thought parts)
          if (part.text && !part.thought) {
            this.emit('transcript', 'agent', part.text);
          }
        }
      }

      if (msg.serverContent.turnComplete) {
        if (this.inputTranscriptBuffer.trim()) {
          this.emit('transcript', 'user', this.inputTranscriptBuffer.trim());
          this.inputTranscriptBuffer = '';
        }
        if (this.outputTranscriptBuffer.trim()) {
          this.emit('transcript', 'agent', this.outputTranscriptBuffer.trim());
          this.outputTranscriptBuffer = '';
        }
        this.emit('status', 'idle', 'Ready');
      }
    }

    // 3. Tool Call from Gemini
    if (msg.toolCall && msg.toolCall.functionCalls) {
      for (const call of msg.toolCall.functionCalls) {
        if (call.id && call.name) {
          this.pendingToolNames.set(call.id, call.name);
        }
        this.emit('toolCall', {
          id: call.id,
          name: call.name,
          args: call.args || {}
        });
      }
    }
  }

  disconnect() {
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
    this.isConnected = false;
    this.isSetupComplete = false;
  }
}
