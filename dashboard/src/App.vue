<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, watch } from 'vue';
import { RouterLink, RouterView, useRoute } from 'vue-router';

const route = useRoute();
const connected = ref(false);
const nodeCount = ref(0);
const utilityExpanded = ref(false);

// Check if current route is a utility page
const isUtilityRoute = computed(() => route.path.startsWith('/utility'));

// Auto-expand utility menu when on utility pages
watch(isUtilityRoute, (val) => {
  if (val) utilityExpanded.value = true;
}, { immediate: true });

function toggleUtility() {
  utilityExpanded.value = !utilityExpanded.value;
}

let pollTimer: number | null = null;

onMounted(() => {
  // Initial check
  checkConnection();
  // Start periodic connection check
  pollTimer = window.setInterval(checkConnection, 5000);
  // Expand if already on utility route
  if (route.path.startsWith('/utility')) {
    utilityExpanded.value = true;
  }
});

onUnmounted(() => {
  if (pollTimer) {
    clearInterval(pollTimer);
    pollTimer = null;
  }
});

async function checkConnection() {
  try {
    const response = await fetch('/api/nodes');
    if (response.ok) {
      const data = await response.json();
      connected.value = true;
      // Count nodes from the array
      if (Array.isArray(data)) {
        nodeCount.value = data.length;
      } else if (data.nodes && Array.isArray(data.nodes)) {
        nodeCount.value = data.nodes.length;
      }
    } else {
      connected.value = false;
    }
  } catch (e) {
    connected.value = false;
  }
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
        <RouterLink to="/rules" class="nav-link">
          <span class="icon">📜</span>
          Rules
        </RouterLink>

        <!-- Utility Menu with Submenu -->
        <div class="nav-group">
          <button
            class="nav-link nav-toggle"
            :class="{ expanded: utilityExpanded, active: isUtilityRoute }"
            @click="toggleUtility"
          >
            <span class="icon">🔧</span>
            Utility
            <span class="arrow" :class="{ expanded: utilityExpanded }">▸</span>
          </button>
          <div class="submenu" :class="{ expanded: utilityExpanded }">
            <RouterLink to="/utility/model-conversion" class="nav-link sub-link">
              <span class="icon">🔄</span>
              Model Conversion
            </RouterLink>
            <RouterLink to="/utility/camera-manager" class="nav-link sub-link">
              <span class="icon">📹</span>
              Camera Manager
            </RouterLink>
            <RouterLink to="/utility/image-tester" class="nav-link sub-link">
              <span class="icon">🖼️</span>
              Image Tester
            </RouterLink>
          </div>
        </div>
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
/* CiRA ME Theme Colors */
/* Primary: #6366F1 (Indigo) */
/* Secondary: #22D3EE (Cyan) */
/* Background: #0F172A (Dark Slate) */
/* Success: #10B981 */

.app {
  display: flex;
  min-height: 100vh;
}

.sidebar {
  width: 240px;
  background: #0F172A;
  color: white;
  display: flex;
  flex-direction: column;
  padding: 20px;
  border-right: 1px solid #1E293B;
}

.logo h1 {
  font-size: 1.5rem;
  font-weight: 600;
  margin-bottom: 4px;
  background: linear-gradient(135deg, #6366F1 0%, #22D3EE 100%);
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
  background-clip: text;
}

.logo .status {
  font-size: 0.75rem;
  color: #94a3b8;
}

.logo .status.online {
  color: #10B981;
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
  background: #1E293B;
  color: white;
}

.nav-link.router-link-active {
  background: #6366F1;
  color: white;
}

.icon {
  font-size: 1.25rem;
}

.sidebar-footer {
  margin-top: auto;
  padding-top: 20px;
  border-top: 1px solid #1E293B;
}

.node-count {
  font-size: 0.875rem;
  color: #22D3EE;
}

.content {
  flex: 1;
  padding: 24px;
  overflow-y: auto;
  background: #0F172A;
}

/* Utility Menu Styles */
.nav-group {
  display: flex;
  flex-direction: column;
}

.nav-toggle {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 12px 16px;
  border-radius: 8px;
  color: #94a3b8;
  background: none;
  border: none;
  cursor: pointer;
  font-size: 1rem;
  font-family: inherit;
  text-align: left;
  width: 100%;
  transition: all 0.2s;
}

.nav-toggle:hover {
  background: #1E293B;
  color: white;
}

.nav-toggle.active {
  color: #22D3EE;
}

.nav-toggle .arrow {
  margin-left: auto;
  font-size: 0.75rem;
  transition: transform 0.2s;
}

.nav-toggle .arrow.expanded {
  transform: rotate(90deg);
}

.submenu {
  display: flex;
  flex-direction: column;
  overflow: hidden;
  max-height: 0;
  transition: max-height 0.2s ease-out;
}

.submenu.expanded {
  max-height: 300px;
}

.sub-link {
  padding-left: 44px !important;
  font-size: 0.9rem;
}

.sub-link .icon {
  font-size: 1rem;
}
</style>
