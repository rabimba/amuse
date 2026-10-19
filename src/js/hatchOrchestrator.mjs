// hatch-based orchestrator: same line-delimited JSON IPC as index.mjs, but the
// agent loop runs through packages/hatch with any provider (gemini REST,
// openai-compatible, local Ollama…). Text chat only — realtime voice requires
// the Gemini Live orchestrator (index.mjs).
import readline from 'node:readline';
import { createSession, runTurn } from '../../packages/hatch/src/agent.mjs';

function sendToNative(obj) {
  process.stdout.write(JSON.stringify(obj) + '\n');
}

let session;
try {
  session = createSession();
  sendToNative({
    type: 'agent_status',
    status: 'idle',
    details: `Ready — ${session.cfg.provider}/${session.provider.model} (${session.tools.length} tools). Voice is disabled with this backend; type below.`
  });
} catch (err) {
  sendToNative({ type: 'agent_status', status: 'error', details: `hatch init failed: ${err.message}` });
}

const rl = readline.createInterface({ input: process.stdin, terminal: false });

// If the native host dies, our stdin pipe closes — exit instead of becoming
// an orphan serving stale tools/history.
rl.on('close', () => process.exit(0));
process.stdin.on('end', () => process.exit(0));

rl.on('line', async (line) => {
  if (!line || !line.trim()) return;
  let msg;
  try {
    msg = JSON.parse(line);
  } catch {
    return;
  }

  switch (msg.type) {
    case 'user_text_input': {
      if (!session) {
        sendToNative({ type: 'agent_status', status: 'error', details: 'hatch session not initialized' });
        return;
      }
      sendToNative({ type: 'agent_status', status: 'thinking', details: 'Thinking…' });
      try {
        const { text, messages } = await runTurn(session.provider, session.history, msg.text, session.tools, {
          onEvent: (type, data) => {
            if (type === 'tool_call') {
              sendToNative({ type: 'agent_status', status: 'thinking', details: `Working with ${data.name}…` });
            } else if (type === 'tool_result') {
              const ok = !(data.output && (data.output.error || data.output.status === 'error'));
              sendToNative({
                type: 'transcript',
                role: 'tool',
                text: `${ok ? 'Executed' : 'Failed'} ${data.name}`
              });
            }
          }
        });
        session.history = messages;
        // Trim history so old turns (and any old refusals) stop anchoring the
        // model, and context stays bounded for local models.
        if (session.history.length > 30) {
          session.history = session.history.slice(-30);
        }
        if (text) sendToNative({ type: 'transcript', role: 'agent', text });
        sendToNative({ type: 'agent_status', status: 'idle', details: 'Ready' });
      } catch (err) {
        sendToNative({ type: 'transcript', role: 'agent', text: `Something went wrong: ${err.message}` });
        sendToNative({ type: 'agent_status', status: 'error', details: err.message.slice(0, 120) });
      }
      break;
    }
    case 'toggleVoice':
    case 'audio_input_chunk':
      // No STT in this backend — ignore audio, HUD was told at startup.
      break;
    case 'voice_stopped':
      break;
    case 'image_input_frame':
      break;
    default:
      console.error('[hatchOrchestrator] unhandled:', msg.type);
  }
});
