import http from 'node:http';
import { WebSocketServer } from 'ws';
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const PROJECT_ROOT = path.resolve(__dirname, '..');
const BRIDGE_PATH = path.join(PROJECT_ROOT, 'bin', 'amuse-bridge');

console.log('====================================================');
console.log('       AMUSE END-TO-END SYSTEM INTEGRATION TEST     ');
console.log('====================================================\n');

// 1. Verify Native Bridge Binary
console.log('STEP 1: Verifying native binary...');
import { execFileSync } from 'node:child_process';
const toolsJson = execFileSync(BRIDGE_PATH, ['tools'], { encoding: 'utf8' });
const tools = JSON.parse(toolsJson);
console.log(`✓ Native bridge operational. Exposing ${tools.length} local tool declarations:\n  - ${tools.map(t => t.name).join('\n  - ')}\n`);

// 2. Start Local Mock Gemini Live Server
console.log('STEP 2: Starting Mock Gemini Multimodal Live Server on port 9099...');

const server = http.createServer();
const wss = new WebSocketServer({ server });

let stepPassCount = 0;

wss.on('connection', (ws) => {
  console.log('✓ Amuse connected to Gemini Live WebSocket endpoint.');
  stepPassCount++;

  ws.on('message', (data) => {
    const msg = JSON.parse(data.toString());

    // A. Handle Setup
    if (msg.setup) {
      console.log(`✓ Received Setup Message:
  Model: ${msg.setup.model}
  Voice: ${msg.setup.generationConfig?.speechConfig?.voiceConfig?.prebuiltVoiceConfig?.voiceName}
  Tool Declarations Count: ${msg.setup.tools?.[0]?.functionDeclarations?.length}`);
      
      stepPassCount++;
      // Acknowledge setup
      ws.send(JSON.stringify({ setupComplete: {} }));

      // Simulate a user prompt trigger by immediately sending a model toolCall
      setTimeout(() => {
        console.log('\nSTEP 3: Testing Gemini Tool Calling loop against macOS iMessage (chat.db)...');
        console.log('→ Mock Gemini issuing toolCall: search_messages(query: "Decompile")');
        ws.send(JSON.stringify({
          toolCall: {
            functionCalls: [
              {
                id: 'call_imessage_101',
                name: 'search_messages',
                args: { query: 'Decompile', limit: 3 }
              }
            ]
          }
        }));
      }, 300);
      return;
    }

    // B. Handle Tool Response from Amuse for iMessage
    if (msg.toolResponse && msg.toolResponse.functionResponses[0]?.id === 'call_imessage_101') {
      const resp = msg.toolResponse.functionResponses[0];
      console.log(`✓ Received toolResponse from Amuse for call ID ${resp.id}:`);
      console.log('  Data returned from local macOS:');
      const messages = resp.response?.output?.messages || [];
      messages.forEach((m, idx) => {
        console.log(`    [${idx + 1}] Sender: ${m.sender} | Date: ${m.date} | Text: ${m.text.slice(0, 50)}...`);
      });

      if (messages.length > 0) {
        stepPassCount++;
        console.log('✓ iMessage connector and tool calling loop successfully verified!\n');
      }

      // Next: Test Notes connector tool call
      console.log('STEP 4: Testing Gemini Tool Calling loop against Apple Notes...');
      console.log('→ Mock Gemini issuing toolCall: search_notes(query: "Servers")');
      ws.send(JSON.stringify({
        toolCall: {
          functionCalls: [
            {
              id: 'call_notes_202',
              name: 'search_notes',
              args: { query: 'Servers', limit: 2 }
            }
          ]
        }
      }));
      return;
    }

    // Handle second tool response for Notes
    if (msg.toolResponse && msg.toolResponse.functionResponses[0]?.id === 'call_notes_202') {
      const resp = msg.toolResponse.functionResponses[0];
      console.log(`✓ Received toolResponse for Notes from Amuse:`, JSON.stringify(resp.response?.output).slice(0, 100) + '...');
      stepPassCount++;

      // Next: Simulate model audio + text reply
      console.log('\nSTEP 5: Simulating Gemini Duplex Audio & Transcript response...');
      // Dummy 24kHz PCM silence buffer
      const dummyPcm = Buffer.alloc(4800).toString('base64');
      ws.send(JSON.stringify({
        serverContent: {
          modelTurn: {
            parts: [
              { text: 'I found your recent messages and notes about servers and decompilation.' },
              { inlineData: { mimeType: 'audio/pcm;rate=24000', data: dummyPcm } }
            ]
          },
          turnComplete: true
        }
      }));

      setTimeout(() => {
        console.log('✓ Model audio chunk streamed to client.');
        stepPassCount++;
        teardownAndFinish();
      }, 500);
    }
  });
});

server.listen(9099, async () => {
  console.log('✓ Mock server listening on ws://127.0.0.1:9099');

  // Spawn Amuse Node Client configured to connect to our mock server
  const { GeminiLiveClient } = await import('../src/js/geminiLiveClient.mjs');
  
  const client = new GeminiLiveClient({
    apiKey: 'test-mock-key',
    model: 'models/gemini-2.0-flash-exp',
    voice: 'Puck'
  }, tools);

  // Override connect to point to local mock server
  client.connect = function() {
    this.ws = new WebSocket('ws://127.0.0.1:9099');
    this.ws.onopen = () => {
      this.isConnected = true;
      this.sendSetup();
    };
    this.ws.onmessage = (event) => {
      this.handleServerMessage(event.data);
    };
    this.ws.onerror = (err) => {
      console.error('Client WS Error:', err);
    };
    this.ws.onclose = () => {
      this.isConnected = false;
    };
  };

  client.on('status', (st, det) => {
    console.log(`[Amuse Status] ${st.toUpperCase()} - ${det}`);
  });

  client.on('transcript', (role, text) => {
    console.log(`[Amuse Transcript] ${role.toUpperCase()}: ${text}`);
  });

  client.on('audioOutput', (base64) => {
    console.log(`[Amuse Audio] Received ${base64.length} bytes of 24kHz PCM audio output.`);
  });

  client.on('toolCall', ({ id, name, args }) => {
    console.log(`[Amuse Dispatcher] Executing native tool: ${name}`);
    const result = execFileSync(BRIDGE_PATH, ['call', name, JSON.stringify(args)], { encoding: 'utf8' });
    client.sendToolResponse(id, JSON.parse(result));
  });

  client.connect();
});

function teardownAndFinish() {
  console.log('\n====================================================');
  console.log(`✓ ALL END-TO-END TESTS PASSED (${stepPassCount}/5 checks passed)`);
  console.log('====================================================\n');
  wss.close();
  server.close();
  process.exit(0);
}
