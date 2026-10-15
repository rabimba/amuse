// Scenario smoke test for hatch (M1).
// Runs one agentic turn under each configured provider with an isolated HOME.
// Skips providers without credentials. Fails loudly on errors.

import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import os from 'node:os';
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const execFileAsync = promisify(execFile);

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '../..');

const TEST_HOME = fs.mkdtempSync(path.join(os.tmpdir(), 'amuse-hatch-home-'));

let failures = 0;
let skips = 0;

function check(label, ok, detail = '') {
  if (ok) console.log(`✓ PASS: ${label}`);
  else { failures++; console.error(`✗ FAIL: ${label}${detail ? ' — ' + detail : ''}`); }
}

const NOHIT = `zz-hatch-${Date.now()}`;
const PROMPT = `Call the search_messages tool with query "${NOHIT}" and limit 1. Then tell me in one sentence how many messages matched. You MUST call the tool.`;

async function runProvider(providerName) {
  const { stdout } = await execFileAsync(process.execPath, [
    path.join(ROOT, 'packages/hatch/bin/hatch.mjs'), 'ask', PROMPT
  ], {
    timeout: 180000,
    env: { ...process.env, HOME: TEST_HOME, AMUSE_PROVIDER: providerName }
  });
  return stdout;
}

function fileHasKey() {
  try {
    const p = path.join(os.homedir(), '.amuse', 'config.json');
    if (!fs.existsSync(p)) return false;
    const c = JSON.parse(fs.readFileSync(p, 'utf8'));
    return !!(c.apiKey || c.geminiApiKey);
  } catch { return false; }
}
const hasGemini = !!process.env.GEMINI_API_KEY || fileHasKey();
const hasOpenAI = !!process.env.OPENAI_API_KEY;

// Credential-free full-loop verification: mock OpenAI-compatible server that
// issues a tool call, asserts the tool result round-trips, then answers.
async function runMockOpenAI() {
  const http = await import('node:http');
  const requests = [];
  const srv = http.createServer((req, res) => {
    let body = '';
    req.on('data', d => body += d);
    req.on('end', () => {
      requests.push(JSON.parse(body));
      const round = requests.length;
      res.setHeader('Content-Type', 'application/json');
      if (round === 1) {
        // Assert tools were offered in OpenAI format
        const toolsOk = Array.isArray(requests[0].tools) && requests[0].tools.length > 0 &&
          requests[0].tools[0].type === 'function';
        check('mock: tools sent in OpenAI format', toolsOk);
        res.end(JSON.stringify({
          choices: [{
            message: {
              role: 'assistant',
              content: null,
              tool_calls: [{
                id: 'mock-call-1',
                type: 'function',
                function: { name: 'search_messages', arguments: JSON.stringify({ query: NOHIT, limit: 1 }) }
              }]
            },
            finish_reason: 'tool_calls'
          }]
        }));
      } else {
        // Round 2+: must contain a role:tool message with the real bridge result
        const toolMsg = requests[round - 1].messages?.find(m => m.role === 'tool' && m.tool_call_id === 'mock-call-1');
        check('mock: tool result round-tripped as role:tool', !!toolMsg);
        if (toolMsg) {
          let parsed = null;
          try { parsed = JSON.parse(toolMsg.content); } catch { /* not JSON */ }
          check('mock: bridge executed search_messages and returned structured output',
            parsed && typeof parsed === 'object' && ('messages' in parsed || 'error' in parsed),
            String(toolMsg.content).slice(0, 160));
        }
        res.end(JSON.stringify({
          choices: [{ message: { role: 'assistant', content: 'SMOKE_OK: 0 messages matched.' }, finish_reason: 'stop' }]
        }));
      }
    });
  });
  await new Promise(r => srv.listen(0, '127.0.0.1', r));
  const port = srv.address().port;
  try {
    const { stdout } = await execFileAsync(process.execPath, [
      path.join(ROOT, 'packages/hatch/bin/hatch.mjs'), 'ask', PROMPT
    ], {
      timeout: 120000,
      env: {
        ...process.env, HOME: TEST_HOME,
        AMUSE_PROVIDER: 'openai',
        OPENAI_API_KEY: 'mock-key',
        OPENAI_BASE_URL: `http://127.0.0.1:${port}`,
        OPENAI_MODEL: 'mock-model'
      }
    });
    check('mock: final answer returned', stdout.includes('SMOKE_OK'), stdout.slice(0, 200));
  } finally {
    srv.close();
  }
}

console.log('hatch smoke test (isolated HOME: ' + TEST_HOME + ')\n');

try {
  await runMockOpenAI();
} catch (err) {
  const stderr = err.stderr ? `\n  stderr: ${String(err.stderr).slice(0, 400)}` : '';
  check('mock openai-compatible loop', false, String(err.message).slice(0, 200) + stderr);
}

if (hasGemini) {
  try {
    const out = runProvider('gemini');
    check('gemini: final answer non-empty', out.trim().length > 10, out.slice(0, 200));
  } catch (err) {
    check('gemini: turn completes', false, String(err.message).slice(0, 300));
  }
} else {
  skips++;
  console.log('⊘ SKIP: gemini (no GEMINI_API_KEY / config)');
}

if (hasOpenAI) {
  try {
    const out = runProvider('openai');
    check('openai: final answer non-empty', out.trim().length > 10, out.slice(0, 200));
  } catch (err) {
    check('openai: turn completes', false, String(err.message).slice(0, 300));
  }
} else {
  skips++;
  console.log('⊘ SKIP: openai (no OPENAI_API_KEY)');
}

try { fs.rmSync(TEST_HOME, { recursive: true, force: true }); } catch {}

console.log('');
if (failures > 0) {
  console.error(`✗ SMOKE FAILED: ${failures} failure(s), ${skips} skipped`);
  process.exit(1);
}
if (skips > 0 && failures === 0) {
  console.log(`⚠ SMOKE OK with ${skips} provider(s) skipped (no credentials)`);
  process.exit(0);
}
console.log('✓ SMOKE PASSED');
