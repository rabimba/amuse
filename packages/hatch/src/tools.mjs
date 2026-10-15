import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const BRIDGE = path.resolve(__dirname, '../../../bin/amuse-bridge');

// Native bridge emits Gemini-style schemas (UPPERCASE types). Canonical form is
// lowercase JSON Schema.
export function normalizeSchema(node) {
  if (node === null || typeof node !== 'object') return node;
  if (Array.isArray(node)) return node.map(normalizeSchema);
  const out = {};
  for (const [k, v] of Object.entries(node)) {
    if (k === 'type' && typeof v === 'string') {
      out[k] = v.toLowerCase();
    } else {
      out[k] = normalizeSchema(v);
    }
  }
  return out;
}

export function loadTools() {
  const raw = execFileSync(BRIDGE, ['tools'], { encoding: 'utf8' });
  const decls = JSON.parse(raw);
  return decls.map(d => ({
    name: d.name,
    description: d.description || '',
    parameters: normalizeSchema(d.parameters || { type: 'object', properties: {} })
  }));
}

// M1: tool execution goes through the legacy native bridge CLI.
// M2+ will route this through Sentinel instead.
export function callTool(name, args, { timeoutMs = 60000 } = {}) {
  try {
    const out = execFileSync(BRIDGE, ['call', name, JSON.stringify(args || {})], {
      encoding: 'utf8',
      timeout: timeoutMs
    });
    try {
      return JSON.parse(out);
    } catch {
      return { output: out.trim() };
    }
  } catch (err) {
    return { status: 'error', error: `tool_exec_failed`, message: String(err.message || err) };
  }
}
