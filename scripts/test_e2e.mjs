import http from 'node:http';
import os from 'node:os';
import fs from 'node:fs';
import { WebSocketServer } from 'ws';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const PROJECT_ROOT = path.resolve(__dirname, '..');
const BRIDGE_PATH = path.join(PROJECT_ROOT, 'bin', 'amuse-bridge');

// Isolated HOME so tools that touch ~/.amuse never touch real personal data.
const TEST_HOME = fs.mkdtempSync(path.join(os.tmpdir(), 'amuse-e2e-home-'));

console.log('====================================================');
console.log('       AMUSE END-TO-END SYSTEM INTEGRATION TEST     ');
console.log(`       isolated HOME: ${TEST_HOME}`);
console.log('====================================================\n');

let failures = 0;
function check(label, ok, detail = '') {
  if (ok) {
    console.log(`✓ PASS: ${label}`);
  } else {
    failures++;
    console.error(`✗ FAIL: ${label}${detail ? ' — ' + detail : ''}`);
  }
}

// Global watchdog: never hang forever, never exit 0 by default
const watchdog = setTimeout(() => {
  console.error('\n✗ FAIL: test timed out after 60s');
  process.exit(1);
}, 60000);

// 1. Verify Native Bridge Binary
console.log('STEP 1: Verifying native binary...');
const { execFileSync } = await import('node:child_process');
const toolsJson = execFileSync(BRIDGE_PATH, ['tools'], { encoding: 'utf8', env: { ...process.env, HOME: TEST_HOME } });
const tools = JSON.parse(toolsJson);
check('native bridge returns tool declarations', Array.isArray(tools) && tools.length > 0, `got ${tools.length}`);
console.log(`  tools: ${tools.map(t => t.name).join(', ')}\n`);

// 2. Start Local Mock Gemini Live Server
console.log('STEP 2: Starting Mock Gemini Live Server on port 9099...');

const server = http.createServer();
const wss = new WebSocketServer({ server });

// A unique token that cannot match any real user data
const NOHIT = `zz-nohit-${Date.now()}`;

const state = {
  setupSeen: false,
  messagesChecked: false,
  notesChecked: false,
  audioSeen: false,
  transcriptSeen: false
};

function runTool(id, name, args) {
  return { id, name, args };
}

wss.on('connection', (ws) => {
  check('client connected to mock WS endpoint', true);

  ws.on('message', (data) => {
    let msg;
    try {
      msg = JSON.parse(data.toString());
    } catch {
      check('client sends valid JSON lines', false, data.toString().slice(0, 120));
      return;
    }

    if (msg.setup) {
      state.setupSeen = true;
      const declCount = msg.setup.tools?.[0]?.functionDeclarations?.length || 0;
      check('setup message has model + tool declarations', !!msg.setup.model && declCount > 0, `model=${msg.setup.model} tools=${declCount}`);
      ws.send(JSON.stringify({ setupComplete: {} }));

      setTimeout(() => {
        console.log('\nSTEP 3: tool-call loop — search_messages (no-hit query, read-only)');
        ws.send(JSON.stringify({ toolCall: { functionCalls: [runTool('call_msg_1', 'search_messages', { query: NOHIT, limit: 3 })] } }));
      }, 300);
      return;
    }

    if (msg.toolResponse) {
      const resp = msg.toolResponse.functionResponses?.[0];
      if (!resp) {
        check('toolResponse well-formed', false, 'missing functionResponses[0]');
        return;
      }
      const out = resp.response?.output;
      const outStr = typeof out === 'string' ? out : JSON.stringify(out);

      if (resp.id === 'call_msg_1') {
        let parsed = null;
        try { parsed = JSON.parse(outStr); } catch { /* macOS may send pre-parsed object */ }
        const output = parsed || out;
        const ok = output && typeof output === 'object' && ('messages' in output || 'error' in output);
        check('search_messages executed and returned structured result', ok, outStr.slice(0, 160));
        check('search_messages did NOT return Unknown tool', !(output && String(output.error || '').startsWith('Unknown tool')));
        state.messagesChecked = true;

        console.log('\nSTEP 4: tool-call loop — search_notes (no-hit query, read-only)');
        ws.send(JSON.stringify({ toolCall: { functionCalls: [runTool('call_notes_1', 'search_notes', { query: NOHIT, limit: 2 })] } }));
        return;
      }

      if (resp.id === 'call_notes_1') {
        let parsed = null;
        try { parsed = JSON.parse(outStr); } catch { /* may be pre-parsed */ }
        const output = parsed || out;
        const ok = output && typeof output === 'object' && ('notes' in output || 'error' in output);
        check('search_notes executed and returned structured result', ok, outStr.slice(0, 160));
        check('search_notes did NOT return Unknown tool', !(output && String(output.error || '').startsWith('Unknown tool')));
        state.notesChecked = true;

        console.log('\nSTEP 5: model audio + transcript reply');
        const dummyPcm = Buffer.alloc(4800).toString('base64');
        ws.send(JSON.stringify({
          serverContent: {
            modelTurn: {
              parts: [
                { text: 'Done — nothing matched, as expected for a no-hit query.' },
                { inlineData: { mimeType: 'audio/pcm;rate=24000', data: dummyPcm } }
              ]
            },
            turnComplete: true
          }
        }));
        setTimeout(teardownAndFinish, 800);
      }
      return;
    }
  });
});

server.listen(9099, async () => {
  console.log('✓ Mock server listening on ws://127.0.0.1:9099');

  const { GeminiLiveClient } = await import('../src/js/geminiLiveClient.mjs');

  const client = new GeminiLiveClient({
    apiKey: 'test-mock-key',
    model: 'models/gemini-2.5-flash-native-audio-latest',
    voice: 'Puck'
  }, tools);

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

  client.on('status', () => {});

  client.on('transcript', (role, text) => {
    console.log(`[transcript] ${role}: ${text}`);
    if (role === 'agent' && text.length > 0) state.transcriptSeen = true;
  });

  client.on('audioOutput', (base64) => {
    if (base64 && base64.length > 0) state.audioSeen = true;
  });

  client.on('toolCall', ({ id, name, args }) => {
    console.log(`[dispatcher] ${name}(${JSON.stringify(args)})`);
    try {
      const result = execFileSync(BRIDGE_PATH, ['call', name, JSON.stringify(args)], {
        encoding: 'utf8',
        env: { ...process.env, HOME: TEST_HOME },
        timeout: 30000
      });
      client.sendToolResponse(id, JSON.parse(result));
    } catch (err) {
      client.sendToolResponse(id, { status: 'error', error: String(err.message || err) });
    }
  });

  client.connect();
});

function teardownAndFinish() {
  clearTimeout(watchdog);
  check('agent transcript received', state.transcriptSeen);
  check('agent audio chunk received', state.audioSeen);

  const expected = ['setupSeen', 'messagesChecked', 'notesChecked', 'audioSeen', 'transcriptSeen'];
  const missing = expected.filter(k => !state[k]);

  console.log('\n====================================================');
  if (failures === 0 && missing.length === 0) {
    console.log('✓ ALL END-TO-END TESTS PASSED');
    console.log('====================================================\n');
    try { fs.rmSync(TEST_HOME, { recursive: true, force: true }); } catch {}
    wss.close();
    server.close();
    process.exit(0);
  } else {
    console.error(`✗ TESTS FAILED: ${failures} failed checks; missing steps: ${missing.join(', ') || 'none'}`);
    console.error('====================================================\n');
    wss.close();
    server.close();
    process.exit(1);
  }
}
