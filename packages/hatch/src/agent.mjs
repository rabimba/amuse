import { loadHatchConfig } from './config.mjs';
import { loadTools, callTool } from './tools.mjs';
import { textMessage } from './format.mjs';
import { GeminiProvider } from './providers/gemini.mjs';
import { OpenAIProvider } from './providers/openai.mjs';

export function createProvider(cfg) {
  const p = cfg.providers[cfg.provider];
  if (!p) throw new Error(`unknown provider: ${cfg.provider}`);
  if (cfg.provider === 'gemini') return new GeminiProvider(p);
  if (cfg.provider === 'openai') return new OpenAIProvider(p);
  throw new Error(`no adapter for provider: ${cfg.provider}`);
}

export function buildSystemPrompt(toolCount) {
  const isLinux = process.platform === 'linux';
  const now = new Date().toString();
  return [
    'You are Amuse, a warm, capable personal AI agent running on the user\'s ' +
      (isLinux ? 'Linux' : 'macOS') + ' machine.',
    `Current time: ${now}.`,
    `You have ${toolCount} native tools. When the user asks for something actionable, call the right tool(s) and report honestly what happened. Never claim an action succeeded if the tool returned an error. Never call tools that are not in your list.`,
    'Only read/modify what the user asked about. Destructive or outward-facing actions (sending, deleting, posting) require the user\'s explicit request.',
    'SEAMLESS CONNECTOR UX: If the user asks about email, messages, files, calendar, etc. and a tool is missing or returns empty, do NOT just refuse and never ask the user to export/copy-paste data. Instead: call list_connectors to see what is available or detected on this machine, tell the user what you found (e.g. "Apple Mail is installed"), and offer the one-step path (e.g. connect_email_account with their address + an app-specific password). Ask for exactly what you need in one message.',
    'If a tool result contains a setup_hint, follow it and relay the offer naturally in your own words.'
  ].join('\n');
}

// Single user turn through the agent loop. Returns { text, toolTrace, messages }.
// onEvent(type, data): 'tool_call' | 'tool_result' | 'text'
export async function runTurn(provider, history, userText, tools, { maxRounds = 8, onEvent } = {}) {
  const messages = [...history, textMessage('user', userText)];
  const system = buildSystemPrompt(tools.length);

  for (let round = 0; round < maxRounds; round++) {
    const res = await provider.complete({ messages, tools, system });

    if (res.toolCalls.length === 0) {
      messages.push(textMessage('agent', res.text));
      return { text: res.text, messages };
    }

    // Record assistant tool calls
    messages.push({ role: 'agent', parts: res.toolCalls.map(tc => ({ type: 'tool_call', id: tc.id, name: tc.name, args: tc.args })) });

    // Execute tools (sequential in M1; parallel + Sentinel gating in M2)
    for (const tc of res.toolCalls) {
      onEvent?.('tool_call', tc);
      const output = callTool(tc.name, tc.args);
      onEvent?.('tool_result', { ...tc, output });
      messages.push({ role: 'tool', parts: [{ type: 'tool_result', id: tc.id, name: tc.name, output }] });
    }
  }
  const text = '(Reached the maximum number of tool steps for this turn.)';
  messages.push(textMessage('agent', text));
  return { text, messages };
}

export function createSession() {
  const cfg = loadHatchConfig();
  const provider = createProvider(cfg);
  const tools = loadTools();
  return { cfg, provider, tools, history: [] };
}
