import path from 'node:path';
import os from 'node:os';
import fs from 'node:fs';

// Provider config resolution. Precedence: env > ~/.amuse/config.json > defaults.
export function loadHatchConfig() {
  let file = {};
  try {
    const p = path.join(os.homedir(), '.amuse', 'config.json');
    if (fs.existsSync(p)) file = JSON.parse(fs.readFileSync(p, 'utf8'));
  } catch { /* ignore */ }

  const providers = {
    gemini: {
      apiKey: process.env.GEMINI_API_KEY || file.geminiApiKey || file.apiKey || '',
      model: process.env.GEMINI_MODEL || file.geminiModel || 'gemini-2.5-flash',
      baseUrl: 'https://generativelanguage.googleapis.com'
    },
    openai: {
      apiKey: process.env.OPENAI_API_KEY || file.openaiApiKey || '',
      model: process.env.OPENAI_MODEL || file.openaiModel || 'gpt-4o-mini',
      baseUrl: (process.env.OPENAI_BASE_URL || file.openaiBaseUrl || 'https://api.openai.com/v1').replace(/\/$/, ''),
      // 'native' = OpenAI tool calling; 'text' = model emits ```tool_call blocks
      // (for local/compatible endpoints without native tool support)
      toolMode: process.env.AMUSE_TOOL_MODE || file.openaiToolMode || 'native'
    }
  };

  const requested = process.env.AMUSE_PROVIDER || file.provider || '';
  let provider = requested;
  if (!provider) {
    provider = providers.gemini.apiKey ? 'gemini' : (providers.openai.apiKey ? 'openai' : 'gemini');
  }
  return { provider, providers, systemExtra: file.systemExtra || '' };
}
