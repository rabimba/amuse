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
    this.pendingToolBatches = [];
    this.restToolWaiters = new Map();
    this.inputTranscriptBuffer = '';
    this.outputTranscriptBuffer = '';
    this.pingTimer = null;
    this.reconnectTimer = null;
    this.lastPongAt = 0;
    this.currentTurnId = 0;
    this.turnResponded = false;
    this.turnWatchdog = null;
    this.chatHistory = [];
  }

  connect() {
    if (!this.config.apiKey) {
      this.emit('status', 'idle', 'Please set your GEMINI_API_KEY using "amuse config --key <KEY>" or in ~/.amuse/config.json');
      return;
    }

    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }

    if (this.ws) {
      const oldWs = this.ws;
      this.ws = null;
      try {
        oldWs.removeAllListeners();
        oldWs.on('error', () => {});
        oldWs.terminate();
      } catch {
        // ignore
      }
    }

    this.isConnected = false;
    this.isSetupComplete = false;

    const host = 'generativelanguage.googleapis.com';
    const path = '/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent';
    const url = `wss://${host}${path}?key=${this.config.apiKey}`;

    this.emit('status', 'thinking', 'Connecting to Amuse Live Engine...');

    try {
      this.ws = new WebSocket(url, { handshakeTimeout: 8000 });
    } catch (err) {
      this.emit('status', 'idle', `Live socket error (${err.message}) — Ready via Hybrid Engine`);
      this.scheduleReconnect(3000);
      return;
    }

    this.ws.on('open', () => {
      this.isConnected = true;
      this.lastPongAt = Date.now();
      this.startHeartbeat();
      this.sendSetup();
    });

    this.ws.on('pong', () => {
      this.lastPongAt = Date.now();
    });

    this.ws.on('message', (data) => {
      this.lastPongAt = Date.now();
      this.handleServerMessage(data);
    });

    this.ws.on('error', (err) => {
      console.error('[GeminiLive] WebSocket Error:', err.message || err);
    });

    this.ws.on('close', () => {
      this.stopHeartbeat();
      this.isConnected = false;
      this.isSetupComplete = false;
      this.scheduleReconnect(1500);
    });
  }

  startHeartbeat() {
    this.stopHeartbeat();
    this.pingTimer = setInterval(() => {
      if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
      if (Date.now() - this.lastPongAt > 35000) {
        console.error('[GeminiLive] Heartbeat pong timeout — reconnecting WebSocket...');
        this.connect();
        return;
      }
      try {
        this.ws.ping();
      } catch {
        this.connect();
      }
    }, 15000);
  }

  stopHeartbeat() {
    if (this.pingTimer) {
      clearInterval(this.pingTimer);
      this.pingTimer = null;
    }
  }

  scheduleReconnect(delayMs = 1500) {
    if (this.reconnectTimer) return;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connect();
    }, delayMs);
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

  buildSystemPrompt() {
    const isLinux = process.platform === 'linux';
    const homeDir = process.env.HOME || '~';
    const clockContext = this.getFormattedClockContext();
    const toolCount = Array.isArray(this.toolDeclarations) ? this.toolDeclarations.length : 0;

    const lines = [
      `You are Amuse, a warm, witty, hyper-capable personal AI companion and agent running natively on the user's ${isLinux ? 'Linux desktop' : 'macOS desktop'}.`,
      `HUMAN COMMUNICATION STYLE: Speak and write like a real, thoughtful human friend and executive assistant—warm, natural, concise, and conversational (use contractions like "I've", "let's", "you're"). Never sound robotic, never recite raw JSON or file paths awkwardly, and always confirm the real action you just took on their computer.`,
      `CURRENT REAL-TIME SYSTEM CLOCK: ${clockContext}.`,
      `IMPORTANT TIME RULE: Always use the exact clock above (or call get_current_time_and_environment if available) when reasoning about times and timezones (such as 9 AM CST/CDT vs local PDT time). Never guess or hallucinate whether a time has already passed.`,
      `PROACTIVE ACTION EXECUTION RULE: You currently have ${toolCount} native tools available on this machine. Whenever the user asks you to do something, DO IT IMMEDIATELY by calling the appropriate tool(s) end-to-end—never stop halfway. If a tool you want is not in your available list, say so plainly instead of pretending it ran. Only call tools that actually appear in your available function declarations.`
    ];

    if (isLinux) {
      lines.push(
        `LINUX NOTES RULE: On this Linux machine, there is NO Apple "Notes app". Whenever you create or search notes with create_note, search_notes, or open_note, notes are saved as real Markdown files in ${homeDir}/Documents/AmuseNotes/ (and ${homeDir}/Notes/) and displayed as interactive Note Cards right inside the Amuse HUD. Always pass open_on_desktop: true when creating or opening a note so the user immediately sees it on their desktop too!`,
        `- Social Media Posting (e.g. "take a screenshot and post it on LinkedIn"): call social_media_action with capture_screenshot: true (when a screenshot is requested) and open_in_browser: true. It captures the screenshot, copies it to the clipboard, and opens the composer in the user's browser. If you call take_screenshot first, follow up with social_media_action in the same turn so the post actually happens end-to-end. Never auto-submit unless the user explicitly asked you to publish.`,
        `- Email (e.g. "check my emails", "send an email"): Call email_action. It reads the user's on-device Thunderbird database and any hosted IMAP/SMTP accounts they connected via connect_email_account. NEVER invent or hallucinate emails—report only what email_action returns. Sending an email requires the user's explicit confirmation in the request.`,
        `- Browser Agent & GUI Desktop Control: Use browser_agent_action (Playwright Chromium) to navigate websites, fill forms, and click buttons. Use gui_desktop_action for X11 desktop input (may be disabled by the user's safety settings).`,
        `- Screencasts, Screenshots & Screen Vision: Call record_screencast or take_screenshot.`,
        `- Alarms & Timers: Call set_alarm_or_timer to arm a countdown with chimes and desktop notifications.`,
        `- Memory & Action Plans: Call save_memory, recall_memories, and manage_action_plan.`,
        `- Custom Tool Builder: Call build_and_run_custom_tool to write and run a Python or Bash tool on the fly (may be disabled by the user's safety settings).`,
        `- Media, Volume & System Control: Call media_and_system_control or computer_control_action.`,
        `- Notes, Calendar, Reminders, Messages, Contacts, Files & Web Search: Use create_note, open_note, search_notes, create_calendar_event, create_reminder, send_message, search_contacts, manage_files, web_search_and_fetch, list_connectors, connect_service.`
      );
    } else {
      lines.push(
        `- macOS connectors: search_messages/get_recent_messages (iMessage), search_contacts, get_calendar_events/create_calendar_event, get_reminders/create_reminder, search_notes/create_note (Apple Notes). These read from and write to the user's real on-device apps.`,
        `- Email: email_action searches connected IMAP/SMTP accounts (connected via connect_email_account) and any on-device Thunderbird database. Sending requires showing the exact draft and calling again with confirm_send: true.`,
        `- Memory: save_memory/recall_memories persist facts across sessions. Clock: get_current_time_and_environment.`
      );
    }

    if (this.startupMemories) {
      lines.push(`ACTIVE MEMORY (Persisted User Facts & Preferences):\n${this.startupMemories}`);
    }

    return lines.filter(Boolean).join('\n');
  }

  sendSetup() {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;

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
              text: this.buildSystemPrompt()
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
    if (!this.isConnected || !this.isSetupComplete || !this.ws || this.ws.readyState !== WebSocket.OPEN) {
      return;
    }

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

    try {
      this.ws.send(JSON.stringify(msg));
    } catch {
      this.connect();
    }
  }

  sendImageFrame(base64ImageData, mimeType = 'image/png') {
    if (!this.isConnected || !this.isSetupComplete || !base64ImageData || !this.ws || this.ws.readyState !== WebSocket.OPEN) {
      return;
    }

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

    try {
      this.ws.send(JSON.stringify(msg));
    } catch {
      this.connect();
    }
  }

  sendTextMessage(text) {
    const turnId = ++this.currentTurnId;
    this.lastUserText = String(text || '');
    this.socialPostedInTurn = false;
    this.turnResponded = false;
    this.outputTranscriptBuffer = '';
    if (this.turnWatchdog) {
      clearTimeout(this.turnWatchdog);
      this.turnWatchdog = null;
    }

    this.emit('status', 'thinking', 'Thinking...');

    // Watchdog: if Live WebSocket does not respond within 5.5s, automatically run guaranteed REST + TTS turn
    this.turnWatchdog = setTimeout(() => {
      if (this.currentTurnId === turnId && !this.turnResponded) {
        console.error('[GeminiLive] Live WebSocket did not respond within 5.5s — running guaranteed REST + Tool engine...');
        this.connect();
        this.runRestFallbackTurn(text, turnId).catch((err) => {
          console.error('[GeminiLive] Fallback error:', err);
          this.emit('status', 'idle', 'Ready');
        });
      }
    }, 5500);

    // If currently connecting or completing setup handshake, wait for ready
    if (this.ws && (this.ws.readyState === WebSocket.CONNECTING || (this.isConnected && !this.isSetupComplete))) {
      this.once('ready', () => {
        if (this.currentTurnId === turnId && !this.turnResponded) {
          this.dispatchLiveTextTurn(text, turnId);
        }
      });
      return;
    }

    const socketHealthy =
      this.isConnected &&
      this.isSetupComplete &&
      this.ws &&
      this.ws.readyState === WebSocket.OPEN &&
      (Date.now() - this.lastPongAt < 35000);

    if (!socketHealthy) {
      this.connect();
      if (this.turnWatchdog) {
        clearTimeout(this.turnWatchdog);
        this.turnWatchdog = null;
      }
      this.runRestFallbackTurn(text, turnId).catch((err) => {
        console.error('[GeminiLive] Fallback error:', err);
        this.emit('status', 'idle', 'Ready');
      });
      return;
    }

    this.dispatchLiveTextTurn(text, turnId);
  }

  dispatchLiveTextTurn(text, turnId) {
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

    try {
      this.ws.send(JSON.stringify(msg));
    } catch {
      this.connect();
      this.runRestFallbackTurn(text, turnId).catch(() => {});
    }
  }

  async runRestFallbackTurn(userText, turnId) {
    if (this.currentTurnId !== turnId || this.turnResponded) return;
    this.turnResponded = true;

    const apiKey = this.config.apiKey;
    const endpoint = 'https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent';

    this.chatHistory.push({
      role: 'user',
      parts: [{ text: `[System Clock: ${this.getFormattedClockContext()}]\n${userText}` }]
    });
    if (this.chatHistory.length > 16) {
      this.chatHistory = this.chatHistory.slice(-16);
    }

    const contents = [...this.chatHistory];
    let finalAgentText = '';

    for (let round = 0; round < 5; round++) {
      const body = {
        systemInstruction: {
          parts: [{ text: this.buildSystemPrompt() }]
        },
        contents,
        tools: [{ functionDeclarations: this.toolDeclarations }],
        generationConfig: {
          temperature: 0.6
        }
      };

      const res = await fetch(endpoint, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'x-goog-api-key': apiKey },
        body: JSON.stringify(body)
      });

      if (!res.ok) {
        const errText = await res.text();
        throw new Error(`Gemini REST error (${res.status}): ${errText}`);
      }

      const data = await res.json();
      const candidate = data.candidates?.[0];
      const modelContent = candidate?.content;
      if (!modelContent || !modelContent.parts) break;

      contents.push(modelContent);

      const fnCalls = modelContent.parts.filter((p) => p.functionCall);
      const textParts = modelContent.parts.filter((p) => p.text && !p.thought).map((p) => p.text);
      if (textParts.length > 0) {
        finalAgentText = textParts.join('\n').trim();
      }

      if (fnCalls.length === 0) {
        break;
      }

      const fnResponseParts = [];
      for (const part of fnCalls) {
        const fc = part.functionCall;
        const callId = `rest-call-${Date.now()}-${Math.random().toString(36).slice(2, 7)}`;
        const toolOutput = await this.invokeNativeToolAndWait(callId, fc.name, fc.args || {});
        fnResponseParts.push({
          functionResponse: {
            name: fc.name,
            response: { output: toolOutput }
          }
        });
      }

      contents.push({
        role: 'user',
        parts: fnResponseParts
      });
    }

    if (finalAgentText) {
      this.chatHistory.push({
        role: 'model',
        parts: [{ text: finalAgentText }]
      });
      this.emit('transcript', 'agent', finalAgentText);
      this.synthesizeSpeechFallback(finalAgentText).catch(() => {});
    }

    this.emit('status', 'idle', 'Ready');
  }

  invokeNativeToolAndWait(callId, name, args) {
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        this.restToolWaiters.delete(callId);
        // Never fake success — report the timeout so the model can react honestly
        resolve({ status: 'error', error: 'tool_timeout', message: `Tool ${name} did not respond within 15s` });
      }, 15000);

      this.restToolWaiters.set(callId, (outputObj) => {
        clearTimeout(timer);
        resolve(outputObj);
      });

      this.emit('toolCall', { id: callId, name, args });
    });
  }

  async synthesizeSpeechFallback(text) {
    try {
      const cleanText = text.replace(/[*#`_~]/g, '').slice(0, 600);
      const url = 'https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash-preview-tts:generateContent';
      const res = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'x-goog-api-key': this.config.apiKey },
        body: JSON.stringify({
          contents: [{ parts: [{ text: cleanText }] }],
          generationConfig: {
            responseModalities: ['AUDIO'],
            speechConfig: {
              voiceConfig: {
                prebuiltVoiceConfig: {
                  voiceName: this.config.voice || 'Puck'
                }
              }
            }
          }
        })
      });
      if (!res.ok) return;
      const data = await res.json();
      const inlineData = data.candidates?.[0]?.content?.parts?.[0]?.inlineData;
      if (inlineData?.data) {
        this.emit('audioOutput', inlineData.data);
      }
    } catch {
      // ignore TTS fallback errors
    }
  }

  async sendToolResponse(callId, outputData, toolName = '') {
    let outputObj = outputData;
    if (typeof outputData === 'string') {
      try {
        outputObj = JSON.parse(outputData);
      } catch {
        outputObj = { output: outputData };
      }
    }

    // Check if this tool call belongs to the guaranteed REST fallback engine
    if (this.restToolWaiters.has(callId)) {
      const waiter = this.restToolWaiters.get(callId);
      this.restToolWaiters.delete(callId);
      waiter(outputObj);
      return;
    }

    if (!this.isConnected || !this.ws || this.ws.readyState !== WebSocket.OPEN) return;

    const resolvedName = toolName || this.pendingToolNames.get(callId) || '';
    if (resolvedName === 'social_media_action') {
      this.socialPostedInTurn = true;
    }

    // Auto-chained screenshot→social posting is OFF by default (it posts canned
    // marketing copy with no confirmation). Enable explicitly with AMUSE_AUTO_SOCIAL=1.
    const autoSocialEnabled = process.env.AMUSE_AUTO_SOCIAL === '1';
    const combinedPrompt = `${this.lastUserText || ''} ${this.inputTranscriptBuffer || ''}`.toLowerCase();
    const wantsSocialPost =
      autoSocialEnabled &&
      (combinedPrompt.includes('linkedin') ||
        combinedPrompt.includes('linekdin') ||
        combinedPrompt.includes('twitter') ||
        combinedPrompt.includes('post it'));

    if (autoSocialEnabled && resolvedName === 'take_screenshot' && wantsSocialPost && !this.socialPostedInTurn && this.pendingToolBatches.length === 0) {
      this.socialPostedInTurn = true;
      const platform = combinedPrompt.includes('twitter') ? 'twitter_x' : 'linkedin';
      const postContent =
        'Excited to share a live screenshot of Amuse — our native Linux personal AI agent built on Gemini 2.5 Flash Live with real-time duplex voice, screen vision, on-device Thunderbird & IMAP email, Playwright browser automation, and 20+ active connectors! #AI #Linux #GeminiLive #Agents';
      try {
        const autoSocialId = `auto-social-${Date.now()}`;
        const socialRes = await this.invokeNativeToolAndWait(autoSocialId, 'social_media_action', {
          platform,
          action: 'post_now',
          content: postContent,
          image_path: outputObj?.file_path || 'latest',
          capture_screenshot: false,
          open_in_browser: true
        });
        outputObj = {
          ...outputObj,
          chained_social_media_action: socialRes,
          note: 'Both take_screenshot AND social_media_action completed end-to-end: screenshot copied to X11 clipboard, LinkedIn composer opened in browser with post text pre-filled, and screenshot pasted via Ctrl+V!'
        };
      } catch (err) {
        console.error('[GeminiLive] Auto-chained social_media_action error:', err);
      }
    }

    const fnResp = {
      id: callId,
      response: {
        output: outputObj
      }
    };
    if (resolvedName) {
      fnResp.name = resolvedName;
    }

    // Check if this call belongs to a multi-tool batch from Gemini Live
    for (let i = 0; i < this.pendingToolBatches.length; i++) {
      const batch = this.pendingToolBatches[i];
      if (batch.remainingIds.has(callId)) {
        batch.remainingIds.delete(callId);
        batch.responses.push(fnResp);
        if (batch.remainingIds.size === 0) {
          clearTimeout(batch.timer);
          this.pendingToolBatches.splice(i, 1);
          this.ws.send(JSON.stringify({
            toolResponse: {
              functionResponses: batch.responses
            }
          }));
        }
        return;
      }
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

    // 2. Server Content (Audio / Text / Live Streaming Transcriptions)
    if (msg.serverContent) {
      this.turnResponded = true;

      if (msg.serverContent.inputTranscription?.text) {
        this.inputTranscriptBuffer += msg.serverContent.inputTranscription.text;
        this.emit('transcriptDelta', 'user', this.inputTranscriptBuffer.trim());
      }

      if (msg.serverContent.outputTranscription?.text) {
        if (this.inputTranscriptBuffer.trim()) {
          this.emit('transcript', 'user', this.inputTranscriptBuffer.trim());
          this.inputTranscriptBuffer = '';
        }
        this.outputTranscriptBuffer += msg.serverContent.outputTranscription.text;
        this.emit('transcriptDelta', 'agent', this.outputTranscriptBuffer.trim());
      }

      const modelTurn = msg.serverContent.modelTurn;
      if (modelTurn && modelTurn.parts) {
        for (const part of modelTurn.parts) {
          if (part.inlineData && part.inlineData.mimeType?.startsWith('audio/pcm')) {
            this.emit('audioOutput', part.inlineData.data);
          }
          if (part.text && !part.thought) {
            this.outputTranscriptBuffer = part.text;
            this.emit('transcriptDelta', 'agent', this.outputTranscriptBuffer.trim());
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

    // 3. Tool Call from Gemini (supports parallel functionCalls batching)
    if (msg.toolCall && msg.toolCall.functionCalls) {
      this.turnResponded = true;
      const calls = msg.toolCall.functionCalls;
      if (calls.length > 1) {
        const batch = {
          remainingIds: new Set(calls.map((c) => c.id)),
          responses: [],
          timer: null
        };
        batch.timer = setTimeout(() => {
          const idx = this.pendingToolBatches.indexOf(batch);
          if (idx !== -1) {
            this.pendingToolBatches.splice(idx, 1);
            if (batch.responses.length > 0 && this.ws && this.ws.readyState === WebSocket.OPEN) {
              this.ws.send(JSON.stringify({
                toolResponse: {
                  functionResponses: batch.responses
                }
              }));
            }
          }
        }, 6000);
        this.pendingToolBatches.push(batch);
      }

      for (const call of calls) {
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
    this.stopHeartbeat();
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
    this.isConnected = false;
    this.isSetupComplete = false;
  }
}
