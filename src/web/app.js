// Muse (Amuse) Web HUD Client Logic
const chatFeed = document.getElementById('chatFeed');
const textInput = document.getElementById('textInput');
const sendBtn = document.getElementById('sendBtn');
const micToggleBtn = document.getElementById('micToggleBtn');
const micLabel = document.getElementById('micLabel');
const closeBtn = document.getElementById('closeBtn');
const statusOrb = document.getElementById('statusOrb');
const statusText = document.getElementById('statusText');
const statusDetails = document.getElementById('statusDetails');
const audioBar = document.getElementById('audioBar');
const liveClockPill = document.getElementById('liveClockPill');
const avatarPropBadge = document.getElementById('avatarPropBadge');

let isMicActive = false;
let streamingBubbles = { user: null, agent: null };

// Live System Clock in Header
function updateHeaderClock() {
  if (!liveClockPill) return;
  const now = new Date();
  try {
    liveClockPill.textContent = new Intl.DateTimeFormat('en-US', {
      hour: '2-digit',
      minute: '2-digit',
      second: '2-digit',
      hour12: true,
      timeZoneName: 'short'
    }).format(now);
  } catch {
    liveClockPill.textContent = now.toLocaleTimeString();
  }
}
updateHeaderClock();
setInterval(updateHeaderClock, 1000);

// Send bridge message to native host
function postBridgeMessage(type, payload = {}) {
  if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.amuseBridge) {
    window.webkit.messageHandlers.amuseBridge.postMessage({ type, payload });
  } else {
    console.log('[Bridge Simulated]', type, payload);
  }
}

// Submit a text prompt to Muse
function sendPromptToMuse(rawText) {
  const text = (rawText || '').trim();
  if (!text) return;
  setActiveNavTab('chat');
  appendMessage('user', text);
  window.setAgentStatus('thinking', 'Working on your request...');
  postBridgeMessage('sendTextMessage', { text });
}

function submitUserMessage() {
  const text = textInput.value.trim();
  if (!text) return;
  textInput.value = '';
  sendPromptToMuse(text);
}

sendBtn.addEventListener('click', submitUserMessage);
textInput.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    submitUserMessage();
  }
});

// Quick Prompt Suggestion Chips
document.querySelectorAll('.prompt-chip').forEach((chip) => {
  chip.addEventListener('click', () => {
    const prompt = chip.getAttribute('data-prompt');
    if (prompt) {
      sendPromptToMuse(prompt);
    }
  });
});

// Mic toggle
micToggleBtn.addEventListener('click', () => {
  isMicActive = !isMicActive;
  updateMicUI(isMicActive);
  postBridgeMessage('toggleVoice', { active: isMicActive });
});

function updateMicUI(active) {
  if (active) {
    micToggleBtn.classList.add('active');
    micLabel.textContent = 'Mute Voice';
    if (avatarPropBadge) avatarPropBadge.textContent = '🎙️';
  } else {
    micToggleBtn.classList.remove('active');
    micLabel.textContent = 'Talk Live';
    audioBar.style.width = '0%';
  }
}

// Navigation Tabs (Chat, Ideas, Goals, Library, Connectors, Screen)
const navTabs = document.querySelectorAll('.nav-tab[data-tab]');
function setActiveNavTab(tabName) {
  navTabs.forEach((btn) => {
    btn.classList.toggle('active', btn.getAttribute('data-tab') === tabName);
  });
}

const tabChat = document.getElementById('tabChat');
const tabFeed = document.getElementById('tabFeed');
const screenBtn = document.getElementById('screenBtn');
const memoryBtn = document.getElementById('memoryBtn');
const notesBtn = document.getElementById('notesBtn');
const connectorsBtn = document.getElementById('connectorsBtn');

if (tabChat) {
  tabChat.addEventListener('click', () => {
    setActiveNavTab('chat');
    chatFeed.scrollTop = chatFeed.scrollHeight;
  });
}

if (tabFeed) {
  tabFeed.addEventListener('click', () => {
    setActiveNavTab('feed');
    renderMuseIdeasFeed();
  });
}

if (screenBtn) {
  screenBtn.addEventListener('click', () => {
    if (avatarPropBadge) avatarPropBadge.textContent = '📸';
    window.setAgentStatus('thinking', 'Capturing screen & streaming to Gemini Vision...');
    postBridgeMessage('shareScreenSnapshot');
  });
}

if (memoryBtn) {
  memoryBtn.addEventListener('click', () => {
    setActiveNavTab('goals');
    if (avatarPropBadge) avatarPropBadge.textContent = '🧠';
    postBridgeMessage('showMemoryAndPlans');
  });
}

if (notesBtn) {
  notesBtn.addEventListener('click', () => {
    setActiveNavTab('library');
    if (avatarPropBadge) avatarPropBadge.textContent = '📝';
    postBridgeMessage('showNotesViewer');
  });
}

if (connectorsBtn) {
  connectorsBtn.addEventListener('click', () => {
    setActiveNavTab('connectors');
    if (avatarPropBadge) avatarPropBadge.textContent = '🔌';
    postBridgeMessage('showConnectorsStatus');
  });
}

function renderMuseIdeasFeed() {
  const ideas = [
    {
      title: '✉️ Proactive Idea: Summarize Real Thunderbird & IMAP Emails',
      badge: 'EMAIL',
      subtitle: 'On-Device Thunderbird + Hosted IMAP/SMTP',
      body: 'Query your on-device Thunderbird inbox and any connected IMAP accounts, or connect a new hosted email account.',
      prompt: 'Can you check my real emails from Thunderbird and summarize the latest ones?'
    },
    {
      title: '📸 Proactive Idea: Screenshot & Post on LinkedIn End-to-End',
      badge: 'SOCIAL + X11',
      subtitle: 'Auto-Screenshot + X11 Clipboard Image + Ctrl+V Composer Paste',
      body: 'Capture a screenshot of your screen and Amuse HUD, open the LinkedIn post composer with text pre-filled, and automatically paste the screenshot image into the post.',
      prompt: 'Take a screenshot of you and our interaction and post it on LinkedIn saying what we are able to do.'
    },
    {
      title: '🌐 Proactive Idea: Playwright Browser Agent Inspection',
      badge: 'BROWSER VM',
      subtitle: 'Live Chromium DOM & Screenshot Automation',
      body: 'Launch Playwright Chromium to inspect interactive buttons/inputs on a webpage and capture a full render.',
      prompt: 'Use browser_agent_action to navigate to https://news.ycombinator.com and tell me the top stories.'
    },
    {
      title: '🎬 Proactive Idea: 5-Second Desktop Screencast',
      badge: 'VIDEO',
      subtitle: 'FFmpeg H.264 MP4 Recording',
      body: 'Record a 5-second MP4 video screencast of your active Linux desktop to ~/Videos/Amuse/.',
      prompt: 'Record a 5 second screencast of my desktop called Demo Capture.'
    },
    {
      title: '🛠️ Proactive Idea: Build & Run Custom Python Tool',
      badge: 'BUILDER',
      subtitle: 'Dynamic Tool Builder (~/.amuse/custom_tools)',
      body: 'Write and execute a custom Python diagnostic script that inspects CPU, RAM, and disk usage.',
      prompt: 'Build and run a custom Python tool called resource_check that prints my current disk space and load average.'
    }
  ];

  ideas.forEach((idea) => {
    const el = document.createElement('div');
    el.className = 'rich-card card-info';
    el.innerHTML = `
      <div class="card-header">
        <span class="card-title">${escapeHtml(idea.title)}</span>
        <span class="card-badge">${escapeHtml(idea.badge)}</span>
      </div>
      <div class="card-subtitle">${escapeHtml(idea.subtitle)}</div>
      <div class="card-body">${escapeHtml(idea.body)}</div>
      <div class="card-actions">
        <button class="card-action-btn">Run with Amuse →</button>
      </div>
    `;
    el.querySelector('.card-action-btn').addEventListener('click', () => {
      sendPromptToMuse(idea.prompt);
    });
    chatFeed.appendChild(el);
  });
  chatFeed.scrollTop = chatFeed.scrollHeight;
}

// Close HUD button
closeBtn.addEventListener('click', () => {
  postBridgeMessage('hideHUD');
});

// Allow dragging the borderless HUD window by its header
const hudHeader = document.querySelector('.hud-header');
if (hudHeader) {
  hudHeader.addEventListener('mousedown', (e) => {
    if (e.target.closest('button')) return;
    postBridgeMessage('startDrag', {
      button: e.button + 1,
      screenX: Math.round(e.screenX),
      screenY: Math.round(e.screenY)
    });
  });
}

function escapeHtml(str) {
  return String(str || '').replace(/[&<>"']/g, (m) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[m]);
}

// Live Streaming Transcript Delta (word-by-word)
window.updateTranscriptDelta = function(msg) {
  if (!msg || !msg.text) return;
  const role = msg.role || 'agent';
  let bubble = streamingBubbles[role];
  if (!bubble || !bubble.isConnected) {
    bubble = document.createElement('div');
    bubble.className = `message-bubble ${role} streaming`;
    chatFeed.appendChild(bubble);
    streamingBubbles[role] = bubble;
  }
  bubble.textContent = msg.text;
  chatFeed.scrollTop = chatFeed.scrollHeight;
};

// Finalized message to chat log
function appendMessage(role, text) {
  if (streamingBubbles[role] && streamingBubbles[role].isConnected) {
    streamingBubbles[role].textContent = text;
    streamingBubbles[role].classList.remove('streaming');
    streamingBubbles[role] = null;
    chatFeed.scrollTop = chatFeed.scrollHeight;
    return;
  }
  const bubble = document.createElement('div');
  bubble.className = `message-bubble ${role}`;
  bubble.textContent = text;
  chatFeed.appendChild(bubble);
  chatFeed.scrollTop = chatFeed.scrollHeight;
}

function getToolPropIcon(toolName) {
  const t = String(toolName || '').toLowerCase();
  if (t.includes('note')) return '📝';
  if (t.includes('alarm') || t.includes('timer') || t.includes('schedule') || t.includes('time')) return '⏰';
  if (t.includes('screen') || t.includes('photo')) return '📸';
  if (t.includes('social') || t.includes('linkedin')) return '🚀';
  if (t.includes('email') || t.includes('mail')) return '✉️';
  if (t.includes('browser') || t.includes('web')) return '🌐';
  if (t.includes('gui') || t.includes('mouse') || t.includes('key')) return '🖱️';
  if (t.includes('memory') || t.includes('plan')) return '🧠';
  if (t.includes('custom_tool') || t.includes('computer') || t.includes('file')) return '💻';
  if (t.includes('calendar') || t.includes('reminder')) return '📅';
  if (t.includes('message') || t.includes('contact')) return '💬';
  return '⚡';
}

function appendToolChip(toolName, summary) {
  const icon = getToolPropIcon(toolName);
  if (avatarPropBadge) avatarPropBadge.textContent = icon;
  const chip = document.createElement('div');
  chip.className = 'tool-chip';
  chip.innerHTML = `<span>${icon}</span> <strong>${escapeHtml(toolName)}:</strong> ${escapeHtml(summary)}`;
  chatFeed.appendChild(chip);
  chatFeed.scrollTop = chatFeed.scrollHeight;
}

window.openDesktopPath = function(filePath) {
  postBridgeMessage('openFile', { path: filePath });
};

window.appendRichCard = function(card) {
  if (!card) return;
  const el = document.createElement('div');
  el.className = `rich-card card-${escapeHtml(card.cardType || 'info')}`;

  const badge = card.badge ? `<span class="card-badge">${escapeHtml(card.badge)}</span>` : '';
  const subtitle = card.subtitle ? `<div class="card-subtitle">${escapeHtml(card.subtitle)}</div>` : '';
  const body = card.body ? `<div class="card-body">${escapeHtml(card.body)}</div>` : '';

  // Inline media preview for screenshots (.png) and screencasts (.mp4)
  let mediaHtml = '';
  if (card.filePath) {
    const fp = String(card.filePath);
    if (fp.endsWith('.png') || fp.endsWith('.jpg')) {
      const fileUrl = fp.startsWith('file://') ? fp : `file://${fp}`;
      mediaHtml = `<div class="card-media-preview"><img src="${escapeHtml(fileUrl)}" alt="Captured Screen" /></div>`;
    } else if (fp.endsWith('.mp4') || fp.endsWith('.webm')) {
      const fileUrl = fp.startsWith('file://') ? fp : `file://${fp}`;
      mediaHtml = `<div class="card-media-preview"><video src="${escapeHtml(fileUrl)}" controls preload="metadata"></video></div>`;
    }
  }

  let actionHtml = '';
  if (card.filePath) {
    const safePath = escapeHtml(card.filePath);
    actionHtml = `<div class="card-actions"><button class="card-action-btn" data-path="${safePath}">${escapeHtml(card.actionLabel || 'Open on Desktop')}</button></div>`;
  }

  el.innerHTML = `
    <div class="card-header">
      <span class="card-title">${escapeHtml(card.title || 'Amuse')}</span>
      ${badge}
    </div>
    ${subtitle}
    ${mediaHtml}
    ${body}
    ${actionHtml}
  `;

  const btn = el.querySelector('.card-action-btn');
  if (btn && card.filePath) {
    btn.addEventListener('click', () => {
      postBridgeMessage('openFile', { path: card.filePath });
    });
  }

  chatFeed.appendChild(el);
  chatFeed.scrollTop = chatFeed.scrollHeight;
};

// Public API called by native layer
window.setAgentStatus = function(state, details) {
  statusOrb.className = `status-orb state-${state}`;

  switch (state) {
    case 'listening':
      statusText.textContent = 'Listening to you...';
      if (avatarPropBadge) avatarPropBadge.textContent = '🎙️';
      updateMicUI(true);
      break;
    case 'thinking':
      statusText.textContent = 'Amuse is working...';
      if (details && avatarPropBadge) {
        avatarPropBadge.textContent = getToolPropIcon(details);
      }
      break;
    case 'speaking':
      statusText.textContent = 'Amuse is speaking...';
      if (avatarPropBadge) avatarPropBadge.textContent = '🔊';
      break;
    case 'idle':
    default:
      statusText.textContent = 'Ready to help';
      if (avatarPropBadge) avatarPropBadge.textContent = '✨';
      break;
  }

  if (details) {
    statusDetails.textContent = details;
  }
};

window.appendTranscriptMessage = function(msg) {
  if (!msg) return;
  if (msg.role === 'tool') {
    appendToolChip(msg.toolName || 'Tool', msg.text || '');
  } else {
    appendMessage(msg.role || 'agent', msg.text || '');
  }
};

window.updateAudioLevel = function(level) {
  const pct = Math.min(100, Math.max(0, level * 100));
  audioBar.style.width = `${pct}%`;
};

window.onHUDVisibilityChanged = function(visible) {
  if (visible) {
    textInput.focus();
  }
};
