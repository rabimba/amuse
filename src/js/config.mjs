import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const CONFIG_DIR = path.join(os.homedir(), '.amuse');
const CONFIG_FILE = path.join(CONFIG_DIR, 'config.json');

export function loadConfig() {
  let fileConfig = {};
  try {
    if (fs.existsSync(CONFIG_FILE)) {
      const content = fs.readFileSync(CONFIG_FILE, 'utf8');
      fileConfig = JSON.parse(content);
    }
  } catch (err) {
    console.error('[AmuseConfig] Error reading config file:', err.message);
  }

  // Env vars must override file values, so apply them after spreading fileConfig.
  const merged = { ...fileConfig };
  if (process.env.GEMINI_API_KEY) merged.apiKey = process.env.GEMINI_API_KEY;
  if (process.env.GEMINI_MODEL) merged.model = process.env.GEMINI_MODEL;

  merged.apiKey = merged.apiKey || '';
  const rawModel = merged.model || 'models/gemini-2.5-flash-native-audio-latest';
  merged.model = rawModel === 'models/gemini-2.0-flash-exp' ? 'models/gemini-2.5-flash-native-audio-latest' : rawModel;
  merged.voice = merged.voice || 'Puck'; // Puck, Charon, Kore, Fenrir, Aoede

  return merged;
}

export function saveConfig(updates) {
  try {
    if (!fs.existsSync(CONFIG_DIR)) {
      fs.mkdirSync(CONFIG_DIR, { recursive: true });
    }
    const current = loadConfig();
    const updated = { ...current, ...updates };
    fs.writeFileSync(CONFIG_FILE, JSON.stringify(updated, null, 2), 'utf8');
    return true;
  } catch (err) {
    console.error('[AmuseConfig] Error saving config:', err.message);
    return false;
  }
}
