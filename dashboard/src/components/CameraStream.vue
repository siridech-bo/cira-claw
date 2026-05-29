<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, watch, nextTick } from 'vue';

interface Props {
  host: string;
  port: number;
  annotated?: boolean;
  mode?: 'auto' | 'webrtc' | 'mjpeg';
  webrtcUrl?: string;  // WebRTC signaling URL from go2rtc
}

const props = withDefaults(defineProps<Props>(), {
  annotated: true,
  mode: 'auto',
});

const emit = defineEmits<{
  (e: 'error', msg: string): void;
  (e: 'modeChange', mode: 'webrtc' | 'mjpeg'): void;
}>();

const mjpegImgRef = ref<HTMLImageElement | null>(null);
const videoRef = ref<HTMLVideoElement | null>(null);  // WebRTC video element

// State
const activeMode = ref<'webrtc' | 'mjpeg'>(
  props.mode === 'webrtc' ? 'webrtc' : 'mjpeg'
);
const mjpegSrc = ref('');
const loading = ref(true);
const errorCount = ref(0);
const streamError = ref(false);
const lastFrameTime = ref(0);
const hasFrame = ref(false);
const blankFrameCount = ref(0);

// WebRTC state
let peerConnection: RTCPeerConnection | null = null;
let webrtcRetryCount = 0;
const MAX_WEBRTC_RETRIES = 3;
const WEBRTC_CONNECTION_TIMEOUT = 10000;  // 10 seconds

let connectionTimeout: number | null = null;
let mjpegWatchdog: number | null = null;
let mjpegMemoryTimer: number | null = null;
let webrtcConnectionTimer: number | null = null;

// Stall timeout - if no valid frames for this long, reconnect
const MJPEG_STALL_TIMEOUT = 5000;
// Blank frame threshold - if img has no content for this many checks, reconnect
const BLANK_FRAME_THRESHOLD = 3;
// MJPEG memory cleanup - reconnect every N minutes to clear browser's accumulated buffer
const MJPEG_MEMORY_CLEANUP_INTERVAL = 3 * 60 * 1000; // 3 minutes (aggressive)

const baseUrl = computed(() => `http://${props.host}:${props.port}`);

const mjpegUrl = computed(() => {
  const endpoint = props.annotated ? '/stream/annotated' : '/stream/raw';
  return `${baseUrl.value}${endpoint}`;
});

function clearConnectionTimeout() {
  if (connectionTimeout) {
    clearTimeout(connectionTimeout);
    connectionTimeout = null;
  }
}

function clearMjpegWatchdog() {
  if (mjpegWatchdog) {
    clearInterval(mjpegWatchdog);
    mjpegWatchdog = null;
  }
}

function clearMjpegMemoryTimer() {
  if (mjpegMemoryTimer) {
    clearInterval(mjpegMemoryTimer);
    mjpegMemoryTimer = null;
  }
}

// Periodic MJPEG reconnect to clear browser's accumulated memory buffer
function startMjpegMemoryCleanup() {
  clearMjpegMemoryTimer();
  mjpegMemoryTimer = window.setInterval(() => {
    if (activeMode.value !== 'mjpeg') return;
    console.log('MJPEG memory cleanup - reconnecting to clear buffer');
    // Clear and reconnect to release accumulated memory
    mjpegSrc.value = '';
    setTimeout(() => {
      mjpegSrc.value = mjpegUrl.value + `?_t=${Date.now()}`;
      lastFrameTime.value = Date.now();
    }, 100);
  }, MJPEG_MEMORY_CLEANUP_INTERVAL);
}

// Clear WebRTC connection timer
function clearWebRTCConnectionTimer() {
  if (webrtcConnectionTimer) {
    clearTimeout(webrtcConnectionTimer);
    webrtcConnectionTimer = null;
  }
}

// Stop WebRTC connection
function stopWebRTC() {
  clearWebRTCConnectionTimer();

  if (peerConnection) {
    peerConnection.ontrack = null;
    peerConnection.onconnectionstatechange = null;
    peerConnection.close();
    peerConnection = null;
  }

  // Clear video source
  if (videoRef.value) {
    videoRef.value.srcObject = null;
  }
}

// Start WebRTC mode - uses go2rtc HTTP POST signaling
async function startWebRTC() {
  if (!props.webrtcUrl) {
    console.log('No WebRTC URL provided, falling back to MJPEG');
    startMjpeg();
    return;
  }

  activeMode.value = 'webrtc';
  loading.value = true;
  streamError.value = false;
  hasFrame.value = false;
  clearConnectionTimeout();
  clearMjpegWatchdog();
  clearMjpegMemoryTimer();
  stopWebRTC();

  emit('modeChange', 'webrtc');

  // Connection timeout - fall back to MJPEG if WebRTC fails
  webrtcConnectionTimer = window.setTimeout(() => {
    if (loading.value && activeMode.value === 'webrtc') {
      console.log('WebRTC connection timeout, falling back to MJPEG');
      webrtcRetryCount++;
      if (webrtcRetryCount >= MAX_WEBRTC_RETRIES) {
        console.log('Max WebRTC retries reached, using MJPEG');
        startMjpeg();
      } else {
        // Retry WebRTC
        startWebRTC();
      }
    }
  }, WEBRTC_CONNECTION_TIMEOUT);

  try {
    // Create RTCPeerConnection with STUN servers
    peerConnection = new RTCPeerConnection({
      iceServers: [
        { urls: 'stun:stun.l.google.com:19302' },
        { urls: 'stun:stun1.l.google.com:19302' },
      ],
    });

    // Handle incoming media track
    peerConnection.ontrack = (event) => {
      console.log('WebRTC track received:', event.track.kind);
      if (videoRef.value && event.streams[0]) {
        videoRef.value.srcObject = event.streams[0];
        loading.value = false;
        hasFrame.value = true;
        webrtcRetryCount = 0;
        clearWebRTCConnectionTimer();
      }
    };

    // Monitor connection state
    peerConnection.onconnectionstatechange = () => {
      console.log('WebRTC connection state:', peerConnection?.connectionState);
      if (peerConnection?.connectionState === 'failed' ||
          peerConnection?.connectionState === 'disconnected') {
        console.log('WebRTC connection failed/disconnected');
        if (props.mode === 'auto' || props.mode === 'webrtc') {
          // Try to reconnect or fall back
          webrtcRetryCount++;
          if (webrtcRetryCount >= MAX_WEBRTC_RETRIES) {
            console.log('Max WebRTC retries reached, falling back to MJPEG');
            startMjpeg();
          } else {
            setTimeout(() => startWebRTC(), 2000);
          }
        }
      }
    };

    // Add transceivers for receiving video
    peerConnection.addTransceiver('video', { direction: 'recvonly' });

    // Create SDP offer
    const offer = await peerConnection.createOffer();
    await peerConnection.setLocalDescription(offer);

    // Wait for ICE gathering to complete (or timeout after 2s)
    await new Promise<void>((resolve) => {
      if (peerConnection?.iceGatheringState === 'complete') {
        resolve();
        return;
      }
      const checkState = () => {
        if (peerConnection?.iceGatheringState === 'complete') {
          resolve();
        }
      };
      peerConnection?.addEventListener('icegatheringstatechange', checkState);
      // Timeout after 2 seconds - proceed with partial candidates
      setTimeout(resolve, 2000);
    });

    // Send offer to go2rtc via HTTP POST (this is the correct go2rtc API)
    const response = await fetch(props.webrtcUrl, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/sdp',
      },
      body: peerConnection.localDescription?.sdp,
    });

    if (!response.ok) {
      throw new Error(`go2rtc returned ${response.status}`);
    }

    // Get answer SDP from response
    const answerSdp = await response.text();

    // Set remote description
    await peerConnection.setRemoteDescription({
      type: 'answer',
      sdp: answerSdp,
    });

    console.log('WebRTC answer received and set');

  } catch (err) {
    console.error('Failed to start WebRTC:', err);
    webrtcRetryCount++;
    if (webrtcRetryCount >= MAX_WEBRTC_RETRIES || props.mode === 'auto') {
      startMjpeg();
    }
  }
}

// MJPEG watchdog - detect blank/stalled streams by checking actual image content
function startMjpegWatchdog() {
  clearMjpegWatchdog();
  lastFrameTime.value = Date.now();
  blankFrameCount.value = 0;

  mjpegWatchdog = window.setInterval(() => {
    if (activeMode.value !== 'mjpeg' || loading.value) return;

    // Check if the img element actually has content (naturalWidth > 0)
    const img = mjpegImgRef.value;
    const hasContent = img && img.naturalWidth > 0 && img.naturalHeight > 0;

    if (hasContent) {
      // Stream is working - reset counters
      blankFrameCount.value = 0;
      lastFrameTime.value = Date.now();
    } else {
      // No content - increment blank counter
      blankFrameCount.value++;
      console.log(`MJPEG blank frame detected (count: ${blankFrameCount.value})`);
    }

    // Check for stall (no valid content for too long)
    const timeSinceLastFrame = Date.now() - lastFrameTime.value;
    const isStalled = timeSinceLastFrame > MJPEG_STALL_TIMEOUT ||
                      blankFrameCount.value >= BLANK_FRAME_THRESHOLD;

    if (isStalled) {
      console.log(`MJPEG stream stalled (blank: ${blankFrameCount.value}, time: ${timeSinceLastFrame}ms), reconnecting...`);
      errorCount.value++;
      blankFrameCount.value = 0;

      // Reconnect by refreshing the stream URL
      mjpegSrc.value = '';
      setTimeout(() => {
        mjpegSrc.value = mjpegUrl.value + `?_t=${Date.now()}`;
        lastFrameTime.value = Date.now();
      }, 100);

    }
  }, 1500);  // Check every 1.5 seconds
}

// Start MJPEG mode - uses native img tag for continuous streaming
function startMjpeg() {
  activeMode.value = 'mjpeg';
  loading.value = true;
  errorCount.value = 0;
  streamError.value = false;
  hasFrame.value = false;
  clearConnectionTimeout();
  clearMjpegWatchdog();
  clearMjpegMemoryTimer();

  // Set MJPEG source - browser handles continuous stream natively
  mjpegSrc.value = mjpegUrl.value + `?_t=${Date.now()}`;

  emit('modeChange', 'mjpeg');
  startMjpegWatchdog();
  startMjpegMemoryCleanup(); // Periodic reconnect to prevent memory buildup

  // Connection timeout - try to reconnect MJPEG
  connectionTimeout = window.setTimeout(() => {
    if (loading.value && activeMode.value === 'mjpeg') {
      console.log('MJPEG connection timeout, reconnecting...');
      mjpegSrc.value = '';
      setTimeout(() => {
        mjpegSrc.value = mjpegUrl.value + `?_t=${Date.now()}`;
      }, 500);
    }
  }, 10000);
}

// MJPEG load handler
function onMjpegLoad() {
  clearConnectionTimeout();
  loading.value = false;
  errorCount.value = 0;
  lastFrameTime.value = Date.now();
  hasFrame.value = true;
}

// MJPEG error handler - retry with backoff
function onMjpegError() {
  errorCount.value++;
  const backoff = Math.min(errorCount.value * 1000, 5000); // Max 5 second backoff
  console.log(`MJPEG error (${errorCount.value}), retrying in ${backoff}ms...`);

  setTimeout(() => {
    mjpegSrc.value = mjpegUrl.value + `?_t=${Date.now()}`;
  }, backoff);
}

function stopStream() {
  clearConnectionTimeout();
  clearMjpegWatchdog();
  clearMjpegMemoryTimer();
  stopWebRTC();
  mjpegSrc.value = '';
}

function reconnect() {
  stopStream();
  errorCount.value = 0;
  streamError.value = false;
  hasFrame.value = false;
  webrtcRetryCount = 0;

  if (props.mode === 'webrtc' || (props.mode === 'auto' && props.webrtcUrl)) {
    startWebRTC();
  } else {
    startMjpeg();
  }
}

function handleVisibilityChange() {
  if (document.visibilityState === 'visible') {
    if (streamError.value || (loading.value && !connectionTimeout)) {
      console.log('Tab visible, attempting reconnection');
      reconnect();
    }
  }
}

onMounted(async () => {
  await nextTick();

  if (props.mode === 'webrtc' || (props.mode === 'auto' && props.webrtcUrl)) {
    // Prefer WebRTC when available (better memory management)
    startWebRTC();
  } else {
    startMjpeg();
  }

  document.addEventListener('visibilitychange', handleVisibilityChange);
});

watch(() => props.mode, (newMode) => {
  stopStream();
  webrtcRetryCount = 0;
  if (newMode === 'webrtc' || (newMode === 'auto' && props.webrtcUrl)) {
    startWebRTC();
  } else {
    startMjpeg();
  }
});

// Watch for webrtcUrl changes - restart WebRTC if URL becomes available
watch(() => props.webrtcUrl, (newUrl, oldUrl) => {
  if (newUrl && !oldUrl && (props.mode === 'auto' || props.mode === 'webrtc')) {
    // WebRTC URL became available, switch to WebRTC
    stopStream();
    webrtcRetryCount = 0;
    startWebRTC();
  }
});

watch([() => props.host, () => props.port], () => {
  reconnect();
});

onUnmounted(() => {
  stopStream();
  document.removeEventListener('visibilitychange', handleVisibilityChange);
});

defineExpose({
  refresh() {
    reconnect();
  },
  reconnect,
  switchMode(mode: 'webrtc' | 'mjpeg') {
    stopStream();
    webrtcRetryCount = 0;
    if (mode === 'webrtc') {
      startWebRTC();
    } else {
      startMjpeg();
    }
  },
});
</script>

<template>
  <div class="camera-stream">
    <!-- Loading overlay -->
    <div class="loading-overlay" v-if="loading && !streamError">
      <span class="spinner"></span>
      <span>Connecting...</span>
    </div>

    <!-- Error overlay -->
    <div class="error-overlay" v-if="streamError">
      <span class="error-icon">⚠️</span>
      <span>Stream disconnected</span>
      <button class="reconnect-btn" @click="reconnect">Reconnect</button>
    </div>

    <!-- WebRTC mode: Native video element (best memory management) -->
    <video
      ref="videoRef"
      v-show="activeMode === 'webrtc' && !streamError"
      autoplay
      playsinline
      muted
      class="stream-video"
      :class="{ hidden: !hasFrame }"
    ></video>

    <!-- MJPEG mode: Native img tag (browser handles continuous stream) -->
    <img
      ref="mjpegImgRef"
      v-if="activeMode === 'mjpeg' && mjpegSrc && !streamError"
      :src="mjpegSrc"
      alt="Camera feed"
      class="stream-img"
      @load="onMjpegLoad"
      @error="onMjpegError"
    />

    <!-- Mode indicator -->
    <div class="mode-indicator" :class="activeMode" v-if="!streamError && hasFrame">
      {{ activeMode === 'webrtc' ? 'WebRTC' : 'MJPEG' }}
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

.stream-video {
  width: 100%;
  height: 100%;
  object-fit: contain;
  background: #0F172A;
}

.stream-video.hidden {
  visibility: hidden;
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
  z-index: 5;
}

.mode-indicator.webrtc {
  color: #6366F1;
}

.mode-indicator.mjpeg {
  color: #10B981;
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
