import readline from 'node:readline';
import { loadConfig } from './config.mjs';
import { getToolDeclarations, getStartupMemories } from './toolRegistry.mjs';
import { GeminiLiveClient } from './geminiLiveClient.mjs';

const config = loadConfig();
const tools = getToolDeclarations();
const startupMemories = getStartupMemories();
const client = new GeminiLiveClient(config, tools, startupMemories);

// Helper to send line-delimited JSON to native host
function sendToNative(obj) {
  process.stdout.write(JSON.stringify(obj) + '\n');
}

// Client event bindings
client.on('status', (status, details) => {
  sendToNative({ type: 'agent_status', status, details });
});

client.on('transcriptDelta', (role, text) => {
  sendToNative({ type: 'transcript_delta', role, text });
});

client.on('transcript', (role, text) => {
  sendToNative({ type: 'transcript', role, text });
});

client.on('audioOutput', (base64Data) => {
  sendToNative({ type: 'audio_output_chunk', data: base64Data });
});

client.on('toolCall', ({ id, name, args }) => {
  sendToNative({
    type: 'tool_call_request',
    callId: id,
    toolName: name,
    arguments: args
  });
});

// Auto-connect
client.connect();

// Handle messages from native host
const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout,
  terminal: false
});

rl.on('line', (line) => {
  if (!line || !line.trim()) return;
  try {
    const msg = JSON.parse(line);
    handleNativeMessage(msg);
  } catch (err) {
    console.error('[AmuseOrchestrator] Error parsing native line:', err);
  }
});

function handleNativeMessage(msg) {
  switch (msg.type) {
    case 'audio_input_chunk':
      client.sendAudioChunk(msg.data);
      break;
    case 'image_input_frame':
      client.sendImageFrame(msg.data, msg.mimeType || 'image/png');
      break;
    case 'user_text_input':
      client.sendTextMessage(msg.text);
      break;
    case 'voice_stopped':
      // User paused mic
      break;
    case 'tool_call_response':
      client.sendToolResponse(msg.callId, msg.output, msg.toolName);
      break;
    default:
      console.log('[AmuseOrchestrator] Unhandled message:', msg.type);
  }
}
