// Canonical message format for hatch (provider-agnostic).
//
// Message: { role: 'system'|'user'|'agent'|'tool', parts: Part[] }
// Part:
//   { type: 'text', text }
//   { type: 'image', mimeType, dataBase64 }
//   { type: 'tool_call', id, name, args }
//   { type: 'tool_result', id, name, output }   // output = object (already parsed)
//
// Tool declaration: { name, description, parameters: <JSON Schema, lowercase types> }

let counter = 0;
export function newCallId() {
  return `call-${Date.now()}-${++counter}`;
}

export function textMessage(role, text) {
  return { role, parts: [{ type: 'text', text }] };
}
