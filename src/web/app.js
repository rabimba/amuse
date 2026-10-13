// Amuse Web HUD Client Logic
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

let isMicActive = false;

// Send bridge message to native host
function postBridgeMessage(type, payload = {}) {
  if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.amuseBridge) {
    window.webkit.messageHandlers.amuseBridge.postMessage({ type, payload });
  } else {
    console.log('[Bridge Simulated]', type, payload);
  }
}

// User text submission
function submitUserMessage() {
  const text = textInput.value.trim();
  if (!text) return;
  textInput.value = '';
  
  appendMessage('user', text);
  postBridgeMessage('sendTextMessage', { text });
}

sendBtn.addEventListener('click', submitUserMessage);
textInput.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    submitUserMessage();
  }
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
  } else {
    micToggleBtn.classList.remove('active');
    micLabel.textContent = 'Start Voice';
    audioBar.style.width = '0%';
  }
}

const screenBtn = document.getElementById('screenBtn');
const memoryBtn = document.getElementById('memoryBtn');
const notesBtn = document.getElementById('notesBtn');
const connectorsBtn = document.getElementById('connectorsBtn');

if (screenBtn) {
  screenBtn.addEventListener('click', () => {
    postBridgeMessage('shareScreenSnapshot');
  });
}

if (memoryBtn) {
  memoryBtn.addEventListener('click', () => {
    postBridgeMessage('showMemoryAndPlans');
  });
}

if (notesBtn) {
  notesBtn.addEventListener('click', () => {
    postBridgeMessage('showNotesViewer');
  });
}

if (connectorsBtn) {
  connectorsBtn.addEventListener('click', () => {
    postBridgeMessage('showConnectorsStatus');
  });
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

// Appends message to chat log
function appendMessage(role, text) {
  const bubble = document.createElement('div');
  bubble.className = `message-bubble ${role}`;
  bubble.textContent = text;
  chatFeed.appendChild(bubble);
  chatFeed.scrollTop = chatFeed.scrollHeight;
}

function appendToolChip(toolName, summary) {
  const chip = document.createElement('div');
  chip.className = 'tool-chip';
  chip.innerHTML = `<span>⚡</span> <strong>${escapeHtml(toolName)}:</strong> ${escapeHtml(summary)}`;
  chatFeed.appendChild(chip);
  chatFeed.scrollTop = chatFeed.scrollHeight;
}

function escapeHtml(str) {
  return String(str || '').replace(/[&<>"']/g, (m) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[m]);
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
  
  switch(state) {
    case 'listening':
      statusText.textContent = 'Listening...';
      updateMicUI(true);
      break;
    case 'thinking':
      statusText.textContent = 'Thinking & Processing...';
      break;
    case 'speaking':
      statusText.textContent = 'Speaking...';
      break;
    case 'idle':
    default:
      statusText.textContent = 'Ready';
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
