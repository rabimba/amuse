// OpenAI-compatible provider adapter (chat.completions). Covers OpenAI, Azure-style
// deployments, OpenRouter, Groq, DeepSeek, Ollama, llama.cpp, vLLM, LM Studio.

export class OpenAIProvider {
  constructor(cfg) {
    this.apiKey = cfg.apiKey;
    this.model = cfg.model;
    this.baseUrl = cfg.baseUrl;
    this.name = 'openai';
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
    const body = {
      model: this.model,
      messages: this.toOpenAIMessages(messages, system),
      temperature: 0.6
    };
    if (tools?.length) {
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

    const toolCalls = (msg.tool_calls || []).map(tc => {
      let args = {};
      try { args = JSON.parse(tc.function?.arguments || '{}'); } catch { /* keep {} */ }
      return { id: tc.id, name: tc.function?.name, args };
    });
    return { text: (msg.content || '').trim(), toolCalls, raw: data };
  }
}
