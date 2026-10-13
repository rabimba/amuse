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

// Close HUD button
closeBtn.addEventListener('click', () => {
  postBridgeMessage('hideHUD');
});

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
  return str.replace(/[&<>"']/g, (m) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[m]);
}

// Public API called by native Cocoa layer
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
