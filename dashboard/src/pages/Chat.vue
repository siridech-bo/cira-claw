<script setup lang="ts">
import { ref, nextTick, onMounted } from 'vue';

interface Message {
  id: string;
  role: 'user' | 'assistant';
  content: string;
  timestamp: Date;
  images?: string[];
}

const messages = ref<Message[]>([]);
const inputText = ref('');
const sending = ref(false);
const messagesContainer = ref<HTMLElement | null>(null);

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

  // Send to backend (placeholder - will be implemented with agent)
  sending.value = true;

  try {
    // For now, simulate a response since the agent isn't implemented yet
    await new Promise(resolve => setTimeout(resolve, 1000));

    const assistantMessage: Message = {
      id: `assistant-${Date.now()}`,
      role: 'assistant',
      content: `I received your message: "${text}"

The AI agent integration is not yet implemented. Once configured, I'll be able to:
- Query device status
- Check inference results
- Take camera snapshots
- Deploy models
- And much more!

Please configure your Claude API key in ~/.cira/credentials/claude.json to enable the agent.`,
      timestamp: new Date(),
    };
    messages.value.push(assistantMessage);
  } catch (error) {
    messages.value.push({
      id: `error-${Date.now()}`,
      role: 'assistant',
      content: `Error: Failed to process your request. Please try again.`,
      timestamp: new Date(),
    });
  } finally {
    sending.value = false;
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
</script>

<template>
  <div class="chat-page">
    <header class="page-header">
      <h2>Chat with CiRA Agent</h2>
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
            <div class="message-text">{{ message.content }}</div>
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
        <button @click="sendMessage" :disabled="!inputText.trim() || sending">
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
}

.page-header h2 {
  font-size: 1.5rem;
  font-weight: 600;
}

.chat-container {
  flex: 1;
  display: flex;
  flex-direction: column;
  background: white;
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
  background: #f1f5f9;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 1.25rem;
  flex-shrink: 0;
}

.message.user .message-avatar {
  background: #2563eb;
}

.message-content {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.message-text {
  padding: 12px 16px;
  border-radius: 12px;
  background: #f1f5f9;
  white-space: pre-wrap;
  line-height: 1.5;
}

.message.user .message-text {
  background: #2563eb;
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
  background: #f1f5f9;
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
  border-top: 1px solid #e2e8f0;
}

.input-area textarea {
  flex: 1;
  padding: 12px 16px;
  border: 1px solid #e2e8f0;
  border-radius: 8px;
  resize: none;
  font-family: inherit;
  font-size: 0.875rem;
  line-height: 1.5;
}

.input-area textarea:focus {
  outline: none;
  border-color: #2563eb;
}

.input-area button {
  padding: 12px 24px;
  background: #2563eb;
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
