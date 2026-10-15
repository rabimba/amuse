#!/usr/bin/env node
import readline from 'node:readline';
import { createSession, runTurn } from '../src/agent.mjs';

const cmd = process.argv[2] || 'chat';

if (cmd === 'chat') {
  const session = createSession();
  const { cfg, provider, tools } = session;
  console.error(`hatch chat — provider=${cfg.provider} model=${provider.model} tools=${tools.length}`);
  console.error('(type your message; "exit" to quit)');

  const rl = readline.createInterface({ input: process.stdin, output: process.stdout, terminal: process.stdin.isTTY });
  const ask = () => new Promise(resolve => rl.question('you> ', resolve));

  while (true) {
    const input = (await ask()).trim();
    if (!input || input === 'exit' || input === 'quit') break;
    try {
      const { text, messages } = await runTurn(provider, session.history, input, tools, {
        onEvent: (type, data) => {
          if (type === 'tool_call') console.error(`  [tool] ${data.name}(${JSON.stringify(data.args)})`);
          if (type === 'tool_result') {
            const s = JSON.stringify(data.output);
            console.error(`  [tool] result: ${s.length > 200 ? s.slice(0, 200) + '…' : s}`);
          }
        }
      });
      session.history = messages;
      console.log(`amuse> ${text}\n`);
    } catch (err) {
      console.error(`error: ${err.message}`);
    }
  }
  rl.close();
} else if (cmd === 'ask') {
  // Non-interactive: hatch ask "prompt"
  const prompt = process.argv.slice(3).join(' ');
  if (!prompt) {
    console.error('usage: hatch ask "prompt"');
    process.exit(2);
  }
  const session = createSession();
  const { text } = await runTurn(session.provider, session.history, prompt, session.tools, {
    onEvent: (type, data) => {
      if (type === 'tool_call') console.error(`  [tool] ${data.name}`);
      if (type === 'tool_result') console.error(`  [tool] done (${JSON.stringify(data.output).length} bytes)`);
    }
  });
  console.log(text);
} else {
  console.error('usage: hatch [chat|ask "prompt"]');
  process.exit(2);
}
