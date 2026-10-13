import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const BRIDGE_PATH = path.resolve(__dirname, '../../bin/amuse-bridge');

export function getToolDeclarations() {
  try {
    const stdout = execFileSync(BRIDGE_PATH, ['tools'], { encoding: 'utf8' });
    return JSON.parse(stdout);
  } catch (err) {
    console.error('[AmuseTools] Error loading tool declarations from bridge:', err.message);
    return [];
  }
}

export function executeNativeTool(toolName, args) {
  try {
    const argsJson = JSON.stringify(args || {});
    const stdout = execFileSync(BRIDGE_PATH, ['call', toolName, argsJson], { encoding: 'utf8' });
    return JSON.parse(stdout);
  } catch (err) {
    console.error(`[AmuseTools] Error executing tool ${toolName}:`, err.message);
    return { error: err.message };
  }
}

export function getStartupMemories() {
  try {
    const res = executeNativeTool('recall_memories', {});
    if (res && Array.isArray(res.memories)) {
      return res.memories.map((m) => `- [${m.category || 'general'}] ${m.key}: ${m.value}`).join('\n');
    }
  } catch {
    // ignore if bridge not built yet
  }
  return '';
}
