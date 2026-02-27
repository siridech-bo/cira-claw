<script setup lang="ts">
import { ref, nextTick, onMounted, onUnmounted, watch } from 'vue';

// Declare mermaid on window for TypeScript
declare global {
  interface Window {
    mermaid?: {
      initialize: (config: Record<string, unknown>) => void;
      run: (config: { querySelector: string }) => Promise<void>;
    };
  }
}

interface Message {
  id: string;
  role: 'user' | 'assistant';
  content: string;
  timestamp: Date;
  images?: string[];
}

interface ChatResponse {
  type: 'response' | 'error' | 'pong' | 'typing';
  content?: string;
  images?: string[];
}

const messages = ref<Message[]>([]);
const inputText = ref('');
const sending = ref(false);
const connected = ref(false);
const messagesContainer = ref<HTMLElement | null>(null);

let ws: WebSocket | null = null;
let reconnectTimer: number | null = null;

function getWebSocketUrl(): string {
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${protocol}//${window.location.host}/chat`;
}

function connect() {
  if (ws?.readyState === WebSocket.OPEN) return;

  const url = getWebSocketUrl();
  ws = new WebSocket(url);

  ws.onopen = () => {
    connected.value = true;
    if (reconnectTimer) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
  };

  ws.onclose = () => {
    connected.value = false;
    sending.value = false;
    // Reconnect after 3 seconds
    reconnectTimer = window.setTimeout(connect, 3000);
  };

  ws.onerror = () => {
    // Error will trigger onclose
  };

  ws.onmessage = (event) => {
    try {
      const response = JSON.parse(event.data) as ChatResponse;
      handleResponse(response);
    } catch {
      console.error('Failed to parse WebSocket message');
    }
  };
}

function handleResponse(response: ChatResponse) {
  switch (response.type) {
    case 'response':
      sending.value = false;
      if (response.content) {
        messages.value.push({
          id: `assistant-${Date.now()}`,
          role: 'assistant',
          content: response.content,
          timestamp: new Date(),
          images: response.images,
        });
        scrollToBottom();
      }
      break;

    case 'error':
      sending.value = false;
      messages.value.push({
        id: `error-${Date.now()}`,
        role: 'assistant',
        content: response.content || 'An error occurred.',
        timestamp: new Date(),
      });
      scrollToBottom();
      break;

    case 'typing':
      sending.value = true;
      break;

    case 'pong':
      // Heartbeat response, ignore
      break;
  }
}

onMounted(() => {
  // Add welcome message
  messages.value.push({
    id: 'welcome',
    role: 'assistant',
    content: `Hello! I'm the CiRA Edge Agent. I can help you monitor and manage your edge AI devices.

Try asking me:
- "What's the status of all devices?"
- "How many defects on line 1 today?"
- "Show me the camera on line 2"
- "Deploy scratch_v4 to line 1"`,
    timestamp: new Date(),
  });

  // Connect to WebSocket
  connect();
});

onUnmounted(() => {
  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
  }
  if (ws) {
    ws.close();
    ws = null;
  }
});

async function sendMessage() {
  const text = inputText.value.trim();
  if (!text || sending.value) return;

  // Add user message
  const userMessage: Message = {
    id: `user-${Date.now()}`,
    role: 'user',
    content: text,
    timestamp: new Date(),
  };
  messages.value.push(userMessage);
  inputText.value = '';

  await scrollToBottom();

  // Send via WebSocket
  if (ws?.readyState === WebSocket.OPEN) {
    sending.value = true;
    ws.send(JSON.stringify({
      type: 'message',
      content: text,
    }));
  } else {
    // Not connected, show error
    messages.value.push({
      id: `error-${Date.now()}`,
      role: 'assistant',
      content: 'Not connected to server. Attempting to reconnect...',
      timestamp: new Date(),
    });
    connect();
    await scrollToBottom();
  }
}

async function scrollToBottom() {
  await nextTick();
  if (messagesContainer.value) {
    messagesContainer.value.scrollTop = messagesContainer.value.scrollHeight;
  }
}

function formatTime(date: Date): string {
  return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function handleKeydown(event: KeyboardEvent) {
  if (event.key === 'Enter' && !event.shiftKey) {
    event.preventDefault();
    sendMessage();
  }
}

/**
 * Render message content with Mermaid diagrams and code blocks.
 *
 * Security: We escape HTML to prevent XSS, then restore our own formatted tags.
 * Mermaid regex MUST run before generic code block regex.
 */
function renderContent(content: string): string {
  // Step 1: Extract code blocks and mermaid blocks before escaping
  const codeBlocks: string[] = [];
  const mermaidBlocks: string[] = [];

  // Store mermaid blocks (process first to handle them separately)
  let html = content.replace(/```mermaid\n([\s\S]*?)```/g, (_, code) => {
    mermaidBlocks.push(code);
    return `__MERMAID_${mermaidBlocks.length - 1}__`;
  });

  // Store code blocks
  html = html.replace(/```(\w*)\n([\s\S]*?)```/g, (_, lang, code) => {
    codeBlocks.push(code);
    return `__CODE_${codeBlocks.length - 1}__`;
  });

  // Step 2: Escape HTML to prevent XSS (on the non-code portions)
  html = html
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');

  // Step 3: Restore mermaid blocks with proper styling
  html = html.replace(/__MERMAID_(\d+)__/g, (_, idx) => {
    const code = mermaidBlocks[parseInt(idx)];
    return `<div class="mermaid" style="margin:12px 0;background:#1e293b;padding:16px;border-radius:8px;overflow-x:auto">${code}</div>`;
  });

  // Step 4: Restore code blocks with proper styling
  html = html.replace(/__CODE_(\d+)__/g, (_, idx) => {
    const code = codeBlocks[parseInt(idx)]
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;');
    return `<pre style="background:#1e1e1e;color:#d4d4d4;padding:12px;border-radius:6px;overflow-x:auto;font-size:13px;margin:8px 0;white-space:pre-wrap;font-family:'JetBrains Mono',Consolas,monospace"><code>${code}</code></pre>`;
  });

  return html;
}

/**
 * Trigger Mermaid rendering after DOM update.
 */
async function renderMermaid() {
  await nextTick();
  window.mermaid?.run({ querySelector: '.mermaid' });
}

// Watch for new assistant messages and render Mermaid diagrams
watch(
  () => messages.value.length,
  (newLen, oldLen) => {
    if (newLen > oldLen) {
      const lastMessage = messages.value[newLen - 1];
      if (lastMessage?.role === 'assistant') {
        renderMermaid();
      }
    }
  }
);
</script>

<template>
  <div class="chat-page">
    <header class="page-header">
      <h2>Chat with CiRA Agent</h2>
      <div class="connection-status" :class="{ connected }">
        <span class="status-dot"></span>
        {{ connected ? 'Connected' : 'Disconnected' }}
      </div>
    </header>

    <div class="chat-container">
      <div class="messages" ref="messagesContainer">
        <div
          v-for="message in messages"
          :key="message.id"
          class="message"
          :class="message.role"
        >
          <div class="message-avatar">
            {{ message.role === 'user' ? '👤' : '🤖' }}
          </div>
          <div class="message-content">
            <div
              v-if="message.role === 'assistant'"
              class="message-text"
              v-html="renderContent(message.content)"
            ></div>
            <div v-else class="message-text">{{ message.content }}</div>
            <div class="message-images" v-if="message.images?.length">
              <img
                v-for="(img, i) in message.images"
                :key="i"
                :src="img"
                alt="Attached image"
                class="message-image"
              />
            </div>
            <div class="message-time">{{ formatTime(message.timestamp) }}</div>
          </div>
        </div>

        <div class="typing-indicator" v-if="sending">
          <div class="message-avatar">🤖</div>
          <div class="dots">
            <span></span>
            <span></span>
            <span></span>
          </div>
        </div>
      </div>

      <div class="input-area">
        <textarea
          v-model="inputText"
          @keydown="handleKeydown"
          placeholder="Type a message... (Enter to send)"
          rows="1"
          :disabled="sending"
        ></textarea>
        <button @click="sendMessage" :disabled="!inputText.trim() || sending || !connected">
          {{ sending ? '...' : 'Send' }}
        </button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.chat-page {
  max-width: 900px;
  margin: 0 auto;
  height: calc(100vh - 48px);
  display: flex;
  flex-direction: column;
}

.page-header {
  margin-bottom: 16px;
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.page-header h2 {
  font-size: 1.5rem;
  font-weight: 600;
}

.connection-status {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 0.875rem;
  color: #94a3b8;
}

.connection-status.connected {
  color: #22c55e;
}

.status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #94a3b8;
}

.connection-status.connected .status-dot {
  background: #22c55e;
}

.chat-container {
  flex: 1;
  display: flex;
  flex-direction: column;
  background: #1E293B;
  border-radius: 12px;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1);
  overflow: hidden;
}

.messages {
  flex: 1;
  overflow-y: auto;
  padding: 20px;
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.message {
  display: flex;
  gap: 12px;
  max-width: 80%;
}

.message.user {
  align-self: flex-end;
  flex-direction: row-reverse;
}

.message-avatar {
  width: 36px;
  height: 36px;
  border-radius: 50%;
  background: #334155;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 1.25rem;
  flex-shrink: 0;
}

.message.user .message-avatar {
  background: #6366F1;
}

.message-content {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.message-text {
  padding: 12px 16px;
  border-radius: 12px;
  background: #334155;
  color: #E2E8F0;
  white-space: pre-wrap;
  line-height: 1.5;
}

.message.user .message-text {
  background: #6366F1;
  color: white;
}

.message-images {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 8px;
}

.message-image {
  max-width: 300px;
  border-radius: 8px;
  cursor: pointer;
}

.message-time {
  font-size: 0.75rem;
  color: #94a3b8;
  padding: 0 4px;
}

.message.user .message-time {
  text-align: right;
}

.typing-indicator {
  display: flex;
  gap: 12px;
  align-items: center;
}

.dots {
  display: flex;
  gap: 4px;
  padding: 12px 16px;
  background: #334155;
  border-radius: 12px;
}

.dots span {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #94a3b8;
  animation: bounce 1.4s infinite ease-in-out both;
}

.dots span:nth-child(1) { animation-delay: -0.32s; }
.dots span:nth-child(2) { animation-delay: -0.16s; }

@keyframes bounce {
  0%, 80%, 100% { transform: scale(0); }
  40% { transform: scale(1); }
}

.input-area {
  display: flex;
  gap: 12px;
  padding: 16px;
  border-top: 1px solid #334155;
}

.input-area textarea {
  flex: 1;
  padding: 12px 16px;
  border: 1px solid #334155;
  border-radius: 8px;
  resize: none;
  font-family: inherit;
  font-size: 0.875rem;
  line-height: 1.5;
  background: #0F172A;
  color: #E2E8F0;
}

.input-area textarea:focus {
  outline: none;
  border-color: #6366F1;
}

.input-area button {
  padding: 12px 24px;
  background: #6366F1;
  color: white;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  font-weight: 500;
}

.input-area button:hover {
  background: #1d4ed8;
}

.input-area button:disabled {
  background: #94a3b8;
  cursor: not-allowed;
}
</style>
