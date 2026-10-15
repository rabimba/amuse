// Gemini provider adapter (REST generateContent). Canonical <-> Gemini formats.

function toGeminiSchema(node) {
  if (node === null || typeof node !== 'object') return node;
  if (Array.isArray(node)) return node.map(toGeminiSchema);
  const out = {};
  for (const [k, v] of Object.entries(node)) {
    out[k] = (k === 'type' && typeof v === 'string') ? v.toUpperCase() : toGeminiSchema(v);
  }
  return out;
}

function toGeminiContents(messages) {
  const contents = [];
  for (const m of messages) {
    if (m.role === 'system') continue; // system goes in systemInstruction
    const parts = [];
    for (const p of m.parts) {
      if (p.type === 'text') parts.push({ text: p.text });
      else if (p.type === 'image') parts.push({ inlineData: { mimeType: p.mimeType, data: p.dataBase64 } });
      else if (p.type === 'tool_call') parts.push({ functionCall: { name: p.name, args: p.args || {} } });
      else if (p.type === 'tool_result') parts.push({ functionResponse: { name: p.name, response: { output: p.output } } });
    }
    contents.push({ role: m.role === 'agent' ? 'model' : 'user', parts });
  }
  return contents;
}

export class GeminiProvider {
  constructor(cfg) {
    this.apiKey = cfg.apiKey;
    this.model = cfg.model;
    this.baseUrl = cfg.baseUrl;
    this.name = 'gemini';
  }

  async complete({ messages, tools, system }) {
    const body = {
      contents: toGeminiContents(messages),
      generationConfig: { temperature: 0.6 }
    };
    if (system) body.systemInstruction = { parts: [{ text: system }] };
    if (tools?.length) {
      body.tools = [{
        functionDeclarations: tools.map(t => ({
          name: t.name,
          description: t.description,
          parameters: toGeminiSchema(t.parameters)
        }))
      }];
    }

    const url = `${this.baseUrl}/v1beta/models/${this.model}:generateContent`;
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 90000);
    let res;
    try {
      res = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'x-goog-api-key': this.apiKey },
        body: JSON.stringify(body),
        signal: controller.signal
      });
    } finally {
      clearTimeout(timer);
    }
    if (!res.ok) {
      throw new Error(`gemini ${res.status}: ${(await res.text()).slice(0, 300)}`);
    }
    const data = await res.json();
    const parts = data.candidates?.[0]?.content?.parts || [];

    const text = parts.filter(p => p.text && !p.thought).map(p => p.text).join('\n').trim();
    const toolCalls = parts.filter(p => p.functionCall).map(p => ({
      id: `gem-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
      name: p.functionCall.name,
      args: p.functionCall.args || {}
    }));
    return { text, toolCalls, raw: data };
  }
}
