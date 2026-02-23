<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, watch } from 'vue';

interface Props {
  host: string;
  port: number;
  annotated?: boolean;
  mode?: 'auto' | 'mjpeg' | 'polling';
  pollInterval?: number;
}

const props = withDefaults(defineProps<Props>(), {
  annotated: true,
  mode: 'auto',
  pollInterval: 100,
});

const emit = defineEmits<{
  (e: 'error', msg: string): void;
  (e: 'modeChange', mode: 'mjpeg' | 'polling'): void;
}>();

const activeMode = ref<'mjpeg' | 'polling'>(props.mode === 'polling' ? 'polling' : 'mjpeg');
const imgSrc = ref('');
const loading = ref(true);
const errorCount = ref(0);
const lastSequence = ref(0);
const streamError = ref(false);
const lastFrameTime = ref(0);

let pollTimer: number | null = null;
let connectionTimeout: number | null = null;
let mjpegWatchdog: number | null = null;

const MJPEG_STALL_TIMEOUT = 8000; // Consider stream stalled if no frame for 8 seconds

const baseUrl = computed(() => `http://${props.host}:${props.port}`);

const mjpegUrl = computed(() => {
  const endpoint = props.annotated ? '/stream/annotated' : '/stream/raw';
  return `${baseUrl.value}${endpoint}`;
});

const frameUrl = computed(() => {
  return `${baseUrl.value}/frame/latest`;
});

// Clear connection timeout
function clearConnectionTimeout() {
  if (connectionTimeout) {
    clearTimeout(connectionTimeout);
    connectionTimeout = null;
  }
}

// Clear MJPEG watchdog
function clearMjpegWatchdog() {
  if (mjpegWatchdog) {
    clearInterval(mjpegWatchdog);
    mjpegWatchdog = null;
  }
}

// Start MJPEG watchdog to detect stalled streams
function startMjpegWatchdog() {
  clearMjpegWatchdog();
  lastFrameTime.value = Date.now();

  mjpegWatchdog = window.setInterval(() => {
    if (activeMode.value !== 'mjpeg' || loading.value) return;

    const timeSinceLastFrame = Date.now() - lastFrameTime.value;
    if (timeSinceLastFrame > MJPEG_STALL_TIMEOUT) {
      console.log(`MJPEG stream stalled (${timeSinceLastFrame}ms since last frame), reconnecting...`);
      // Try to reconnect by refreshing the stream URL
      imgSrc.value = mjpegUrl.value + `?_t=${Date.now()}`;
      lastFrameTime.value = Date.now(); // Reset to avoid rapid retries
      errorCount.value++;

      // After multiple stalls, switch to polling if in auto mode
      if (props.mode === 'auto' && errorCount.value >= 3) {
        console.log('MJPEG keeps stalling, switching to polling mode');
        startPolling();
      }
    }
  }, 2000); // Check every 2 seconds
}

// Start with MJPEG, fallback to polling on errors
function startMjpeg() {
  activeMode.value = 'mjpeg';
  loading.value = true;
  errorCount.value = 0;
  streamError.value = false;
  clearConnectionTimeout();
  clearMjpegWatchdog();
  imgSrc.value = mjpegUrl.value + `?_t=${Date.now()}`;
  emit('modeChange', 'mjpeg');

  // Start watchdog to detect stalled streams
  startMjpegWatchdog();

  // Set connection timeout - if MJPEG doesn't load within 5 seconds, switch to polling
  if (props.mode === 'auto') {
    connectionTimeout = window.setTimeout(() => {
      if (loading.value && activeMode.value === 'mjpeg') {
        console.log('MJPEG connection timeout, switching to polling mode');
        startPolling();
      }
    }, 5000);
  }
}

// Switch to polling mode
function startPolling() {
  activeMode.value = 'polling';
  loading.value = true;
  streamError.value = false;
  clearConnectionTimeout();
  clearMjpegWatchdog();
  emit('modeChange', 'polling');
  pollFrame();
}

// Poll for new frame
async function pollFrame() {
  if (activeMode.value !== 'polling') return;

  try {
    // Fetch frame with cache-busting timestamp
    // Note: Don't send Cache-Control header as it triggers CORS preflight
    const response = await fetch(`${frameUrl.value}?_t=${Date.now()}`, {
      cache: 'no-store',
    });

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    // Check sequence number from header (optional optimization to skip duplicate frames)
    const seq = parseInt(response.headers.get('X-Frame-Sequence') || '0', 10);

    // Always process the frame if we get valid data
    // Only skip if sequence is same AND we already have an image displayed
    const shouldUpdate = seq !== lastSequence.value || !imgSrc.value || imgSrc.value === '';

    if (shouldUpdate || seq > lastSequence.value) {
      lastSequence.value = seq;

      // Create blob URL from response
      const blob = await response.blob();

      // Verify we got actual image data
      if (blob.size > 0) {
        const oldSrc = imgSrc.value;
        imgSrc.value = URL.createObjectURL(blob);

        // Revoke old blob URL to prevent memory leak
        if (oldSrc && oldSrc.startsWith('blob:')) {
          URL.revokeObjectURL(oldSrc);
        }
      }
    }

    loading.value = false;
    errorCount.value = 0;

    // Schedule next poll
    pollTimer = window.setTimeout(pollFrame, props.pollInterval);
  } catch (e) {
    errorCount.value++;
    if (errorCount.value < 10) {
      // Retry after a longer delay
      pollTimer = window.setTimeout(pollFrame, 1000);
    } else {
      // Max retries reached - show error state with reconnect option
      loading.value = false;
      streamError.value = true;
      emit('error', 'Failed to fetch frames');
    }
  }
}

// Handle MJPEG load success
function onMjpegLoad() {
  clearConnectionTimeout();
  loading.value = false;
  errorCount.value = 0;
  lastFrameTime.value = Date.now(); // Reset watchdog timer on each frame
}

// Handle MJPEG error - switch to polling mode
function onMjpegError() {
  errorCount.value++;

  if (props.mode === 'auto' && errorCount.value >= 3) {
    // Switch to polling mode
    console.log('MJPEG failed, switching to polling mode');
    startPolling();
  } else if (props.mode !== 'polling') {
    // Retry MJPEG after delay
    setTimeout(() => {
      imgSrc.value = mjpegUrl.value + `?_t=${Date.now()}`;
    }, 2000);
  }
}

// Cleanup
function stopPolling() {
  clearConnectionTimeout();
  clearMjpegWatchdog();
  if (pollTimer) {
    clearTimeout(pollTimer);
    pollTimer = null;
  }
  // Clean up blob URL
  if (imgSrc.value && imgSrc.value.startsWith('blob:')) {
    URL.revokeObjectURL(imgSrc.value);
  }
}

// Reconnect function for manual retry
function reconnect() {
  stopPolling();
  errorCount.value = 0;
  streamError.value = false;
  if (props.mode === 'polling') {
    startPolling();
  } else {
    startMjpeg();
  }
}

// Handle visibility change - auto-reconnect when tab becomes visible
function handleVisibilityChange() {
  if (document.visibilityState === 'visible') {
    // If we had an error or stream stopped, try to reconnect
    if (streamError.value || (loading.value && !pollTimer && !connectionTimeout)) {
      console.log('Tab visible, attempting reconnection');
      reconnect();
    }
  }
}

// Initialize based on mode
onMounted(() => {
  if (props.mode === 'polling') {
    startPolling();
  } else {
    startMjpeg();
  }
  // Listen for visibility changes
  document.addEventListener('visibilitychange', handleVisibilityChange);
});

// Watch for mode prop changes
watch(() => props.mode, (newMode) => {
  stopPolling();
  if (newMode === 'polling') {
    startPolling();
  } else {
    startMjpeg();
  }
});

// Cleanup on unmount
onUnmounted(() => {
  stopPolling();
  document.removeEventListener('visibilitychange', handleVisibilityChange);
});

// Expose method to force refresh
defineExpose({
  refresh() {
    reconnect();
  },
  reconnect,
  switchMode(mode: 'mjpeg' | 'polling') {
    stopPolling();
    if (mode === 'polling') {
      startPolling();
    } else {
      startMjpeg();
    }
  },
});
</script>

<template>
  <div class="camera-stream">
    <div class="loading-overlay" v-if="loading && !streamError">
      <span class="spinner"></span>
      <span>Connecting...</span>
    </div>
    <div class="error-overlay" v-if="streamError">
      <span class="error-icon">⚠️</span>
      <span>Stream disconnected</span>
      <button class="reconnect-btn" @click="reconnect">Reconnect</button>
    </div>
    <img
      v-if="imgSrc && !streamError"
      :src="imgSrc"
      alt="Camera feed"
      class="stream-img"
      @load="onMjpegLoad"
      @error="onMjpegError"
    />
    <div class="mode-indicator" :class="activeMode" v-if="!streamError">
      {{ activeMode === 'mjpeg' ? 'MJPEG' : 'Polling' }}
    </div>
  </div>
</template>

<style scoped>
.camera-stream {
  position: relative;
  width: 100%;
  height: 100%;
  background: #0F172A;
  overflow: hidden;
}

.stream-img {
  width: 100%;
  height: 100%;
  object-fit: contain;
}

.loading-overlay {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  background: rgba(30, 41, 59, 0.9);
  color: #94a3b8;
  gap: 12px;
  z-index: 10;
}

.spinner {
  width: 24px;
  height: 24px;
  border: 3px solid #334155;
  border-top-color: #3b82f6;
  border-radius: 50%;
  animation: spin 1s linear infinite;
}

@keyframes spin {
  to { transform: rotate(360deg); }
}

.mode-indicator {
  position: absolute;
  top: 8px;
  right: 8px;
  padding: 4px 8px;
  font-size: 10px;
  font-weight: 600;
  text-transform: uppercase;
  border-radius: 4px;
  background: rgba(0, 0, 0, 0.5);
  color: #94a3b8;
}

.mode-indicator.mjpeg {
  color: #10B981;
}

.mode-indicator.polling {
  color: #fbbf24;
}

.error-overlay {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  background: rgba(30, 41, 59, 0.95);
  color: #f87171;
  gap: 12px;
  z-index: 10;
}

.error-icon {
  font-size: 32px;
}

.reconnect-btn {
  margin-top: 8px;
  padding: 8px 20px;
  background: #3b82f6;
  color: white;
  border: none;
  border-radius: 6px;
  font-size: 14px;
  cursor: pointer;
  transition: background 0.2s;
}

.reconnect-btn:hover {
  background: #6366F1;
}
</style>
