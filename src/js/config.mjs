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

  const apiKey = process.env.GEMINI_API_KEY || fileConfig.apiKey || '';
  const rawModel = process.env.GEMINI_MODEL || fileConfig.model || 'models/gemini-2.5-flash-native-audio-latest';
  const model = rawModel === 'models/gemini-2.0-flash-exp' ? 'models/gemini-2.5-flash-native-audio-latest' : rawModel;
  const voice = fileConfig.voice || 'Puck'; // Puck, Charon, Kore, Fenrir, Aoede

  return {
    apiKey,
    model,
    voice,
    ...fileConfig
  };
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
