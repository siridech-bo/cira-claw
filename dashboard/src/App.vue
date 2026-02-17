<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue';
import { RouterLink, RouterView } from 'vue-router';

const connected = ref(false);
const nodeCount = ref(0);

let ws: WebSocket | null = null;

onMounted(() => {
  connectWebSocket();
});

onUnmounted(() => {
  if (ws) {
    ws.close();
  }
});

function connectWebSocket() {
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  const host = window.location.host;
  ws = new WebSocket(`${protocol}//${host}/ws`);

  ws.onopen = () => {
    connected.value = true;
  };

  ws.onmessage = (event) => {
    try {
      const msg = JSON.parse(event.data);
      if (msg.type === 'connected' && msg.payload?.summary) {
        nodeCount.value = msg.payload.summary.total;
      }
    } catch (e) {
      console.error('Failed to parse WS message:', e);
    }
  };

  ws.onclose = () => {
    connected.value = false;
    setTimeout(connectWebSocket, 3000);
  };

  ws.onerror = () => {
    connected.value = false;
  };
}
</script>

<template>
  <div class="app">
    <nav class="sidebar">
      <div class="logo">
        <h1>CiRA Edge</h1>
        <span class="status" :class="{ online: connected }">
          {{ connected ? 'Connected' : 'Disconnected' }}
        </span>
      </div>

      <div class="nav-links">
        <RouterLink to="/" class="nav-link">
          <span class="icon">📊</span>
          Overview
        </RouterLink>
        <RouterLink to="/cameras" class="nav-link">
          <span class="icon">📷</span>
          Cameras
        </RouterLink>
        <RouterLink to="/chat" class="nav-link">
          <span class="icon">💬</span>
          Chat
        </RouterLink>
      </div>

      <div class="sidebar-footer">
        <div class="node-count">{{ nodeCount }} devices</div>
      </div>
    </nav>

    <main class="content">
      <RouterView />
    </main>
  </div>
</template>

<style>
.app {
  display: flex;
  min-height: 100vh;
}

.sidebar {
  width: 240px;
  background: #1e293b;
  color: white;
  display: flex;
  flex-direction: column;
  padding: 20px;
}

.logo h1 {
  font-size: 1.5rem;
  font-weight: 600;
  margin-bottom: 4px;
}

.logo .status {
  font-size: 0.75rem;
  color: #94a3b8;
}

.logo .status.online {
  color: #4ade80;
}

.logo .status::before {
  content: '●';
  margin-right: 6px;
}

.nav-links {
  margin-top: 32px;
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.nav-link {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 12px 16px;
  border-radius: 8px;
  color: #94a3b8;
  text-decoration: none;
  transition: all 0.2s;
}

.nav-link:hover {
  background: #334155;
  color: white;
}

.nav-link.router-link-active {
  background: #2563eb;
  color: white;
}

.icon {
  font-size: 1.25rem;
}

.sidebar-footer {
  margin-top: auto;
  padding-top: 20px;
  border-top: 1px solid #334155;
}

.node-count {
  font-size: 0.875rem;
  color: #94a3b8;
}

.content {
  flex: 1;
  padding: 24px;
  overflow-y: auto;
}
</style>
