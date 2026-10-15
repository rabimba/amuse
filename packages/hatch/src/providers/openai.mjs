// OpenAI-compatible provider adapter (chat.completions). Covers OpenAI, Azure-style
// deployments, OpenRouter, Groq, DeepSeek, Ollama, llama.cpp, vLLM, LM Studio.

export class OpenAIProvider {
  constructor(cfg) {
    this.apiKey = cfg.apiKey;
    this.model = cfg.model;
    this.baseUrl = cfg.baseUrl;
    this.toolMode = cfg.toolMode || 'native';
    this.name = 'openai';
  }

  // In text mode we describe tools in the system prompt and parse fenced blocks back.
  toolInstructions(tools) {
    if (!tools?.length) return '';
    const specs = tools.map(t =>
      `- ${t.name}: ${t.description}\n  parameters JSON Schema: ${JSON.stringify(t.parameters)}`
    ).join('\n');
    return [
      '',
      'TOOLS: You can call tools by emitting a fenced block exactly like:',
      '```tool_call',
      '{"name": "tool_name", "args": { ... }}',
      '```',
      'Emit one block per call. After the user relays the tool result, continue.',
      'Available tools:',
      specs
    ].join('\n');
  }

  parseTextToolCalls(text) {
    const toolCalls = [];
    const re = /```tool_call\s*\n([\s\S]*?)```/g;
    let m;
    let clean = text || '';
    while ((m = re.exec(text)) !== null) {
      try {
        const obj = JSON.parse(m[1]);
        if (obj && typeof obj.name === 'string') {
          toolCalls.push({
            id: `txt-${Date.now()}-${toolCalls.length}`,
            name: obj.name,
            args: obj.args || {}
          });
        }
      } catch { /* malformed block — leave in text */ }
    }
    if (toolCalls.length) clean = clean.replace(re, '').trim();
    return { text: clean, toolCalls };
  }

  toOpenAIMessages(messages, system) {
    const out = [];
    if (system) out.push({ role: 'system', content: system });
    for (const m of messages) {
      if (m.role === 'system') continue;
      const contentParts = [];
      const toolCalls = [];
      const toolResults = [];
      for (const p of m.parts) {
        if (p.type === 'text') {
          contentParts.push({ type: 'text', text: p.text });
        } else if (p.type === 'image') {
          contentParts.push({ type: 'image_url', image_url: { url: `data:${p.mimeType};base64,${p.dataBase64}` } });
        } else if (p.type === 'tool_call') {
          toolCalls.push({ id: p.id, type: 'function', function: { name: p.name, arguments: JSON.stringify(p.args || {}) } });
        } else if (p.type === 'tool_result') {
          toolResults.push({ role: 'tool', tool_call_id: p.id, content: JSON.stringify(p.output) });
        }
      }
      if (toolCalls.length) {
        const text = contentParts.filter(c => c.type === 'text').map(c => c.text).join('\n');
        out.push({ role: 'assistant', content: text || null, tool_calls: toolCalls });
      } else if (toolResults.length) {
        out.push(...toolResults);
      } else if (contentParts.length) {
        const onlyText = contentParts.every(c => c.type === 'text');
        out.push({
          role: m.role === 'agent' ? 'assistant' : 'user',
          content: onlyText ? contentParts.map(c => c.text).join('\n') : contentParts
        });
      }
    }
    return out;
  }

  async complete({ messages, tools, system }) {
    const useText = this.toolMode === 'text';
    const sysText = (system || '') + (useText ? this.toolInstructions(tools) : '');
    const body = {
      model: this.model,
      messages: this.toOpenAIMessages(messages, sysText),
      temperature: 0.6
    };
    if (tools?.length && !useText) {
      body.tools = tools.map(t => ({
        type: 'function',
        function: { name: t.name, description: t.description, parameters: t.parameters }
      }));
    }

    const url = `${this.baseUrl}/chat/completions`;
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 90000);
    let res;
    try {
      res = await fetch(url, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          ...(this.apiKey ? { Authorization: `Bearer ${this.apiKey}` } : {})
        },
        body: JSON.stringify(body),
        signal: controller.signal
      });
    } finally {
      clearTimeout(timer);
    }
    if (!res.ok) {
      throw new Error(`openai ${res.status}: ${(await res.text()).slice(0, 300)}`);
    }
    const data = await res.json();
    const msg = data.choices?.[0]?.message || {};

    if (this.toolMode === 'text') {
      const parsed = this.parseTextToolCalls(msg.content || '');
      return { text: parsed.text, toolCalls: parsed.toolCalls, raw: data };
    }

    const toolCalls = (msg.tool_calls || []).map(tc => {
      let args = {};
      try { args = JSON.parse(tc.function?.arguments || '{}'); } catch { /* keep {} */ }
      return { id: tc.id, name: tc.function?.name, args };
    });
    return { text: (msg.content || '').trim(), toolCalls, raw: data };
  }
}
