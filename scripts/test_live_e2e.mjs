import { execFileSync } from 'node:child_process';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';
import fs from 'node:fs';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const PROJECT_ROOT = path.resolve(__dirname, '..');
const BRIDGE_PATH = path.join(PROJECT_ROOT, 'bin', 'amuse-bridge');

// Isolated HOME so tool calls never touch real ~/.amuse state
const TEST_HOME = fs.mkdtempSync(path.join(os.tmpdir(), 'amuse-live-home-'));

console.log('================================================================');
console.log('       AMUSE LIVE GOOGLE GEMINI API END-TO-END TEST              ');
console.log('       (hits the real Gemini API; read-only connector queries)   ');
console.log('================================================================\n');

// 1. Load API Key
const configPath = path.join(os.homedir(), '.amuse', 'config.json');
let apiKey = process.env.GEMINI_API_KEY;
if (!apiKey && fs.existsSync(configPath)) {
  const conf = JSON.parse(fs.readFileSync(configPath, 'utf8'));
  apiKey = conf.apiKey;
}

if (!apiKey) {
  console.error('Error: No GEMINI_API_KEY found in environment or ~/.amuse/config.json');
  process.exit(1);
}

console.log('✓ API key found');

// 2. Fetch Tools from Native Bridge
console.log('\nSTEP 1: Fetching native function schemas from amuse-bridge...');
const toolsJson = execFileSync(BRIDGE_PATH, ['tools'], { encoding: 'utf8', env: { ...process.env, HOME: TEST_HOME } });
const toolDeclarations = JSON.parse(toolsJson);
console.log(`✓ amuse-bridge loaded ${toolDeclarations.length} native tools`);

// 3. Gemini REST helper — model list derived from configured default
const configuredModel = (process.env.GEMINI_MODEL || 'models/gemini-2.5-flash-native-audio-latest')
  .replace(/^models\//, '')
  .replace(/-native-audio-latest$/, ''); // native-audio models are Live-only
const candidateModels = [...new Set([configuredModel, 'gemini-2.5-flash', 'gemini-2.5-flash-lite'])];

async function callGemini(contents, tools = null) {
  let lastErr = null;
  for (const model of candidateModels) {
    try {
      const body = { contents };
      if (tools) {
        body.tools = [{ functionDeclarations: tools }];
      }

      const res = await fetch(`https://generativelanguage.googleapis.com/v1beta/models/${model}:generateContent`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'x-goog-api-key': apiKey },
        body: JSON.stringify(body)
      });

      const data = await res.json();
      if (data.candidates && data.candidates.length > 0) {
        return { model, data };
      }
      if (data.error && data.error.code !== 503) {
        throw new Error(data.error.message);
      }
    } catch (err) {
      lastErr = err;
      if (!err.message.includes('503') && !err.message.includes('high demand')) {
        throw err;
      }
    }
  }
  throw new Error(`All candidate models failed. Last error: ${lastErr?.message || 'unavailable'}`);
}

// Multi-turn loop that executes tools against the native bridge
async function resolveGemini(userPrompt) {
  console.log(`\nUser Prompt: "${userPrompt}"`);
  const history = [{ role: 'user', parts: [{ text: userPrompt }] }];
  let turns = 0;
  let toolsUsed = 0;

  while (turns++ < 5) {
    const res = await callGemini(history, toolDeclarations);
    const candidate = res.data.candidates[0].content;
    const callParts = candidate.parts.filter(p => p.functionCall);

    if (callParts.length === 0) {
      const answer = candidate.parts.map(p => p.text).filter(Boolean).join('\n');
      return { model: res.model, answer, toolsUsed };
    }

    for (const part of callParts) {
      const { name, args } = part.functionCall;
      console.log(`✓ Gemini (${res.model}) invoked native tool: ${name}(${JSON.stringify(args || {})})`);
      toolsUsed++;

      let toolResult;
      try {
        const out = execFileSync(BRIDGE_PATH, ['call', name, JSON.stringify(args || {})], {
          encoding: 'utf8',
          env: { ...process.env, HOME: TEST_HOME },
          timeout: 60000
        });
        toolResult = JSON.parse(out);
      } catch (err) {
        toolResult = { error: `Tool ${name} failed: ${err.message}` };
      }

      history.push({ role: 'model', parts: [part] });
      history.push({
        role: 'user',
        parts: [{
          functionResponse: {
            name: name,
            response: { output: toolResult }
          }
        }]
      });
    }
  }
  throw new Error('Exceeded maximum tool loop turns.');
}

const NOHIT = `zz-live-${Date.now()}`;
let passedTests = 0;
let failedTests = 0;

async function expect(label, fn) {
  try {
    await fn();
    passedTests++;
    console.log(`✓ PASS: ${label}`);
  } catch (err) {
    failedTests++;
    console.error(`✗ FAIL: ${label} — ${err.message}`);
  }
}

async function runLiveTest() {
  // TEST A: tool-call loop against messages (no-hit query = no private data in logs)
  console.log('\nSTEP 2: Live tool-calling loop (messages, no-hit query)...');
  await expect('messages tool loop', async () => {
    const res = await resolveGemini(`Search my messages for the exact token "${NOHIT}" and tell me if anything matched. You must call search_messages.`);
    console.log('--- response ---\n' + res.answer + '\n----------------');
    if (res.toolsUsed < 1) throw new Error('model never called a tool');
    if (!res.answer || res.answer.length < 5) throw new Error('empty final answer');
  });

  // TEST B: tool-call loop against notes
  console.log('\nSTEP 3: Live tool-calling loop (notes, no-hit query)...');
  await expect('notes tool loop', async () => {
    const res = await resolveGemini(`Search my notes for the exact token "${NOHIT}". You must call search_notes, then tell me whether anything matched.`);
    console.log('--- response ---\n' + res.answer + '\n----------------');
    if (res.toolsUsed < 1) throw new Error('model never called a tool');
    if (!res.answer || res.answer.length < 5) throw new Error('empty final answer');
  });

  // TEST C: TTS generation (failure IS a failure)
  console.log('\nSTEP 4: Live TTS generation...');
  await expect('TTS generates audio', async () => {
    const ttsRes = await fetch('https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash-preview-tts:generateContent', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'x-goog-api-key': apiKey },
      body: JSON.stringify({
        contents: [{ parts: [{ text: 'Amuse live test.' }] }],
        generationConfig: {
          responseModalities: ['AUDIO'],
          speechConfig: { voiceConfig: { prebuiltVoiceConfig: { voiceName: 'Puck' } } }
        }
      })
    });
    const ttsData = await ttsRes.json();
    const audioPart = ttsData.candidates?.[0]?.content?.parts?.find(p => p.inlineData);
    if (!audioPart) {
      throw new Error(`no audio in response: ${JSON.stringify(ttsData).slice(0, 200)}`);
    }
    const audioBytes = Buffer.from(audioPart.inlineData.data, 'base64');
    console.log(`  generated ${audioBytes.length} bytes (${audioPart.inlineData.mimeType})`);
    if (audioBytes.length < 1000) throw new Error('audio suspiciously short');
  });

  console.log('\n================================================================');
  if (failedTests === 0) {
    console.log(`✓ ALL LIVE GEMINI END-TO-END TESTS PASSED (${passedTests}/3)`);
  } else {
    console.error(`✗ LIVE TESTS: ${passedTests} passed, ${failedTests} FAILED`);
  }
  console.log('================================================================\n');
  try { fs.rmSync(TEST_HOME, { recursive: true, force: true }); } catch {}
  process.exit(failedTests === 0 ? 0 : 1);
}

runLiveTest().catch(err => {
  console.error('\n✗ Live E2E test crashed:', err);
  process.exit(1);
});
