// Live test against a real OpenAI-compatible endpoint (or Gemini).
// Credentials come from env or ~/.amuse/config.json (set via bin/amuse config).
//
//   npm run test:hatch:live
//   AMUSE_PROVIDER=openai OPENAI_BASE_URL=... OPENAI_API_KEY=... OPENAI_MODEL=... node tests/scenarios/hatch_live.mjs
//   AMUSE_TOOL_MODE=text   # if the endpoint has no native tool calling

import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import os from 'node:os';
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const execFileAsync = promisify(execFile);
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '../..');
const TEST_HOME = fs.mkdtempSync(path.join(os.tmpdir(), 'amuse-hatch-live-'));

function fileCfg() {
  try {
    const p = path.join(os.homedir(), '.amuse', 'config.json');
    if (fs.existsSync(p)) return JSON.parse(fs.readFileSync(p, 'utf8'));
  } catch { /* ignore */ }
  return {};
}

const file = fileCfg();
const provider = process.env.AMUSE_PROVIDER || file.provider || (process.env.OPENAI_API_KEY || file.openaiApiKey ? 'openai' : 'gemini');
const hasKey = provider === 'openai'
  ? !!(process.env.OPENAI_API_KEY || file.openaiApiKey)
  : !!(process.env.GEMINI_API_KEY || file.apiKey || file.geminiApiKey);

const model = provider === 'openai'
  ? (process.env.OPENAI_MODEL || file.openaiModel || 'gpt-4o-mini')
  : (process.env.GEMINI_MODEL || 'gemini-2.5-flash');

console.log('================================================================');
console.log('  AMUSE HATCH LIVE TEST');
console.log(`  provider=${provider} model=${model} toolMode=${process.env.AMUSE_TOOL_MODE || file.openaiToolMode || 'native'}`);
console.log('================================================================\n');

if (!hasKey) {
  console.error(`No credentials for provider "${provider}". Set them via:`);
  console.error('  ./bin/amuse config --openai-base-url <URL> --openai-key <KEY> --openai-model <MODEL>');
  console.error('  ./bin/amuse config --provider openai');
  console.error('or env: OPENAI_API_KEY / OPENAI_BASE_URL / OPENAI_MODEL / AMUSE_PROVIDER');
  process.exit(2);
}

const NOHIT = `zz-live-${Date.now()}`;
const PROMPT = `Call the search_messages tool with query "${NOHIT}" and limit 1, then tell me in one short sentence how many messages matched. You MUST call the tool first.`;

let failures = 0;
function check(label, ok, detail = '') {
  if (ok) console.log(`✓ PASS: ${label}`);
  else { failures++; console.error(`✗ FAIL: ${label}${detail ? ' — ' + detail : ''}`); }
}

try {
  const { stdout, stderr } = await execFileAsync(process.execPath, [
    path.join(ROOT, 'packages/hatch/bin/hatch.mjs'), 'ask', PROMPT
  ], {
    timeout: 600000,
    env: { ...process.env, HOME: TEST_HOME, AMUSE_PROVIDER: provider }
  });

  console.log('--- agent answer ---');
  console.log(stdout.trim());
  console.log('--------------------\n');

  check('tool was called', stderr.includes('[tool] search_messages'), stderr.split('\n').filter(l => l.includes('[tool]')).join(' | ') || 'no tool lines');
  check('final answer non-empty', stdout.trim().length > 5);
  check('answer does not claim fake matches', !/\b([1-9]\d*)\s+messages?\s+matched/i.test(stdout));
} catch (err) {
  const se = err.stderr ? String(err.stderr).slice(0, 600) : '';
  check('live turn completes', false, `${err.message}\n${se}`);
  if (se.includes('tool') === false && (process.env.AMUSE_TOOL_MODE || file.openaiToolMode) !== 'text') {
    console.error('\nHint: if this endpoint lacks native tool calling, retry with AMUSE_TOOL_MODE=text');
  }
}

try { fs.rmSync(TEST_HOME, { recursive: true, force: true }); } catch {}

console.log('');
if (failures > 0) {
  console.error(`✗ LIVE TEST FAILED (${failures})`);
  process.exit(1);
}
console.log('✓ LIVE TEST PASSED');
