import { EventEmitter } from 'node:events';

export class GeminiLiveClient extends EventEmitter {
  constructor(config, toolDeclarations = []) {
    super();
    this.config = config;
    this.toolDeclarations = toolDeclarations;
    this.ws = null;
    this.isConnected = false;
    this.isSetupComplete = false;
  }

  connect() {
    if (!this.config.apiKey) {
      this.emit('status', 'idle', 'Please set your GEMINI_API_KEY using "amuse config --key <KEY>" or in ~/.amuse/config.json');
      return;
    }

    const host = 'generativelanguage.googleapis.com';
    const path = '/ws/google.ai.generativelanguage.v1alpha.GenerativeService.BidiGenerateContent';
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

  sendSetup() {
    const setupMsg = {
      setup: {
        model: this.config.model || 'models/gemini-2.0-flash-exp',
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
        systemInstruction: {
          parts: [
            {
              text: 'You are Amuse, an intelligent and helpful personal assistant for macOS. You have direct access to the user\'s local macOS calendar, reminders, contacts, notes, and messages through local tools. Use your tools whenever the user asks about their schedule, contacts, messages, or to-dos. Be concise, friendly, and natural in spoken conversation.'
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

  sendTextMessage(text) {
    if (!this.isConnected) {
      this.connect();
      // Queue once setup completes
      this.once('ready', () => this.sendTextMessage(text));
      return;
    }

    const msg = {
      clientContent: {
        turns: [
          {
            role: 'user',
            parts: [
              { text: text }
            ]
          }
        ],
        turnComplete: true
      }
    };

    this.ws.send(JSON.stringify(msg));
    this.emit('status', 'thinking', 'Thinking...');
  }

  sendToolResponse(callId, outputData) {
    if (!this.isConnected) return;

    let outputObj = outputData;
    if (typeof outputData === 'string') {
      try {
        outputObj = JSON.parse(outputData);
      } catch {
        outputObj = { output: outputData };
      }
    }

    const msg = {
      toolResponse: {
        functionResponses: [
          {
            id: callId,
            response: {
              output: outputObj
            }
          }
        ]
      }
    };

    this.ws.send(JSON.stringify(msg));
  }

  handleServerMessage(raw) {
    let msg;
    try {
      msg = JSON.parse(raw);
    } catch (err) {
      console.error('[GeminiLive] Failed to parse server message:', err);
      return;
    }

    // 1. Setup complete response
    if (msg.setupComplete) {
      this.isSetupComplete = true;
      this.emit('ready');
      this.emit('status', 'idle', 'Connected. Speak or type a request.');
      return;
    }

    // 2. Server Content (Audio / Text)
    if (msg.serverContent) {
      const modelTurn = msg.serverContent.modelTurn;
      if (modelTurn && modelTurn.parts) {
        for (const part of modelTurn.parts) {
          // Audio output
          if (part.inlineData && part.inlineData.mimeType?.startsWith('audio/pcm')) {
            this.emit('audioOutput', part.inlineData.data);
          }
          // Text transcript
          if (part.text) {
            this.emit('transcript', 'agent', part.text);
          }
        }
      }

      if (msg.serverContent.turnComplete) {
        this.emit('status', 'idle', 'Ready');
      }
    }

    // 3. Tool Call from Gemini
    if (msg.toolCall && msg.toolCall.functionCalls) {
      for (const call of msg.toolCall.functionCalls) {
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
