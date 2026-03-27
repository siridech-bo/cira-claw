<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, watch, nextTick } from 'vue';

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

// Canvas for polling mode (flicker-free)
const canvasRef = ref<HTMLCanvasElement | null>(null);
const containerRef = ref<HTMLDivElement | null>(null);
const mjpegImgRef = ref<HTMLImageElement | null>(null);
let ctx: CanvasRenderingContext2D | null = null;

// State
const activeMode = ref<'mjpeg' | 'polling'>(props.mode === 'polling' ? 'polling' : 'mjpeg');
const mjpegSrc = ref('');
const loading = ref(true);
const errorCount = ref(0);
const lastSequence = ref(0);
const streamError = ref(false);
const lastFrameTime = ref(0);
const hasFrame = ref(false);
const blankFrameCount = ref(0);

let pollTimer: number | null = null;
let connectionTimeout: number | null = null;
let mjpegWatchdog: number | null = null;
let mjpegMemoryTimer: number | null = null;
let resizeObserver: ResizeObserver | null = null;
let pendingBlobUrl: string | null = null;

// Reusable Image object for polling mode (prevents memory leak from creating new Images)
let reusableImage: HTMLImageElement | null = null;
let frameCount = 0;
const RESET_IMAGE_EVERY_N_FRAMES = 500; // Reset image object every 500 frames to prevent corruption

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

const frameUrl = computed(() => {
  return `${baseUrl.value}/frame/latest`;
});

// Initialize canvas for polling mode
function initCanvas() {
  if (!canvasRef.value) return;
  ctx = canvasRef.value.getContext('2d', {
    alpha: false,
    desynchronized: true,
    willReadFrequently: false,
  });
  if (ctx) {
    // Disable smoothing for sharper image rendering
    ctx.imageSmoothingEnabled = false;
  }
  updateCanvasSize();
}

function updateCanvasSize() {
  if (!canvasRef.value || !containerRef.value) return;

  const rect = containerRef.value.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;

  // Set canvas resolution to match display size * DPR
  canvasRef.value.width = Math.floor(rect.width * dpr);
  canvasRef.value.height = Math.floor(rect.height * dpr);

  // Reset context state after resize (canvas resize clears context)
  if (ctx) {
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.imageSmoothingEnabled = false;
  }
}

// Draw frame to canvas (for polling mode)
function drawFrame(img: HTMLImageElement) {
  if (!ctx || !canvasRef.value || !containerRef.value) return;
  if (!img.naturalWidth || !img.naturalHeight) return; // Skip invalid images

  const rect = containerRef.value.getBoundingClientRect();
  const canvasWidth = rect.width;
  const canvasHeight = rect.height;
  const dpr = window.devicePixelRatio || 1;

  const imgAspect = img.naturalWidth / img.naturalHeight;
  const canvasAspect = canvasWidth / canvasHeight;

  let drawWidth: number, drawHeight: number, drawX: number, drawY: number;

  if (imgAspect > canvasAspect) {
    drawWidth = canvasWidth;
    drawHeight = canvasWidth / imgAspect;
    drawX = 0;
    drawY = (canvasHeight - drawHeight) / 2;
  } else {
    drawHeight = canvasHeight;
    drawWidth = canvasHeight * imgAspect;
    drawX = (canvasWidth - drawWidth) / 2;
    drawY = 0;
  }

  // Reset context state before drawing to prevent accumulated corruption
  ctx.setTransform(1, 0, 0, 1, 0, 0); // Reset transform
  ctx.globalCompositeOperation = 'source-over';
  ctx.globalAlpha = 1;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0); // Apply DPR scaling
  ctx.imageSmoothingEnabled = false;

  // Clear background
  ctx.fillStyle = '#0F172A';
  ctx.fillRect(0, 0, canvasWidth, canvasHeight);

  // Draw image
  ctx.drawImage(img, Math.floor(drawX), Math.floor(drawY), Math.ceil(drawWidth), Math.ceil(drawHeight));

  hasFrame.value = true;
}

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

      if (props.mode === 'auto' && errorCount.value >= 5) {
        console.log('MJPEG keeps stalling, switching to polling mode');
        startPolling();
      }
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

  // Connection timeout (longer to allow for slow inference startup)
  if (props.mode === 'auto') {
    connectionTimeout = window.setTimeout(() => {
      if (loading.value && activeMode.value === 'mjpeg') {
        console.log('MJPEG connection timeout, switching to polling mode');
        startPolling();
      }
    }, 10000);
  }
}

// MJPEG load handler
function onMjpegLoad() {
  clearConnectionTimeout();
  loading.value = false;
  errorCount.value = 0;
  lastFrameTime.value = Date.now();
  hasFrame.value = true;
}

// MJPEG error handler
function onMjpegError() {
  errorCount.value++;

  if (props.mode === 'auto' && errorCount.value >= 3) {
    console.log('MJPEG failed, switching to polling mode');
    startPolling();
  } else if (props.mode !== 'polling') {
    setTimeout(() => {
      mjpegSrc.value = mjpegUrl.value + `?_t=${Date.now()}`;
    }, 2000);
  }
}

// Start polling mode - uses canvas for flicker-free rendering
function startPolling() {
  activeMode.value = 'polling';
  loading.value = true;
  streamError.value = false;
  hasFrame.value = false;
  clearConnectionTimeout();
  clearMjpegWatchdog();

  // Clear MJPEG
  mjpegSrc.value = '';

  // Initialize canvas
  nextTick(() => {
    initCanvas();
    emit('modeChange', 'polling');
    pollFrame();
  });
}

// Poll for new frame and draw to canvas
async function pollFrame() {
  if (activeMode.value !== 'polling') return;

  try {
    const response = await fetch(`${frameUrl.value}?_t=${Date.now()}`, {
      cache: 'no-store',
    });

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    const seq = parseInt(response.headers.get('X-Frame-Sequence') || '0', 10);
    const shouldUpdate = seq !== lastSequence.value || !hasFrame.value;

    if (shouldUpdate || seq > lastSequence.value) {
      lastSequence.value = seq;

      const blob = await response.blob();

      if (blob.size > 0) {
        if (pendingBlobUrl) {
          URL.revokeObjectURL(pendingBlobUrl);
        }

        pendingBlobUrl = URL.createObjectURL(blob);

        // Periodically reset Image object to prevent color corruption over time
        frameCount++;
        if (!reusableImage || frameCount >= RESET_IMAGE_EVERY_N_FRAMES) {
          // Clean up old image
          if (reusableImage) {
            reusableImage.onload = null;
            reusableImage.onerror = null;
            reusableImage.src = '';
          }
          reusableImage = new Image();
          frameCount = 0;
        }

        reusableImage.onload = () => {
          if (reusableImage) {
            drawFrame(reusableImage);
          }
          if (pendingBlobUrl) {
            URL.revokeObjectURL(pendingBlobUrl);
            pendingBlobUrl = null;
          }
        };
        reusableImage.onerror = () => {
          if (pendingBlobUrl) {
            URL.revokeObjectURL(pendingBlobUrl);
            pendingBlobUrl = null;
          }
        };
        reusableImage.src = pendingBlobUrl;
      }
    }

    loading.value = false;
    errorCount.value = 0;

    pollTimer = window.setTimeout(pollFrame, props.pollInterval);
  } catch (e) {
    errorCount.value++;
    if (errorCount.value < 10) {
      pollTimer = window.setTimeout(pollFrame, 1000);
    } else {
      loading.value = false;
      streamError.value = true;
      emit('error', 'Failed to fetch frames');
    }
  }
}

function stopStream() {
  clearConnectionTimeout();
  clearMjpegWatchdog();
  clearMjpegMemoryTimer();

  if (pollTimer) {
    clearTimeout(pollTimer);
    pollTimer = null;
  }

  mjpegSrc.value = '';

  if (pendingBlobUrl) {
    URL.revokeObjectURL(pendingBlobUrl);
    pendingBlobUrl = null;
  }

  // Clean up reusable image
  if (reusableImage) {
    reusableImage.onload = null;
    reusableImage.onerror = null;
    reusableImage.src = '';
    reusableImage = null;
  }
}

function reconnect() {
  stopStream();
  errorCount.value = 0;
  streamError.value = false;
  hasFrame.value = false;

  if (props.mode === 'polling') {
    startPolling();
  } else {
    startMjpeg();
  }
}

function handleVisibilityChange() {
  if (document.visibilityState === 'visible') {
    if (streamError.value || (loading.value && !pollTimer && !connectionTimeout)) {
      console.log('Tab visible, attempting reconnection');
      reconnect();
    }
  }
}

onMounted(async () => {
  await nextTick();

  if (containerRef.value) {
    resizeObserver = new ResizeObserver(() => {
      if (activeMode.value === 'polling') {
        updateCanvasSize();
      }
    });
    resizeObserver.observe(containerRef.value);
  }

  if (props.mode === 'polling') {
    startPolling();
  } else {
    startMjpeg();
  }

  document.addEventListener('visibilitychange', handleVisibilityChange);
});

watch(() => props.mode, (newMode) => {
  stopStream();
  if (newMode === 'polling') {
    startPolling();
  } else {
    startMjpeg();
  }
});

watch([() => props.host, () => props.port], () => {
  reconnect();
});

onUnmounted(() => {
  stopStream();

  if (resizeObserver) {
    resizeObserver.disconnect();
    resizeObserver = null;
  }

  document.removeEventListener('visibilitychange', handleVisibilityChange);
});

defineExpose({
  refresh() {
    reconnect();
  },
  reconnect,
  switchMode(mode: 'mjpeg' | 'polling') {
    stopStream();
    if (mode === 'polling') {
      startPolling();
    } else {
      startMjpeg();
    }
  },
});
</script>

<template>
  <div class="camera-stream" ref="containerRef">
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

    <!-- Polling mode: Canvas (flicker-free rendering) -->
    <canvas
      v-show="activeMode === 'polling' && !streamError"
      ref="canvasRef"
      class="stream-canvas"
      :class="{ hidden: !hasFrame }"
    ></canvas>

    <!-- Mode indicator -->
    <div class="mode-indicator" :class="activeMode" v-if="!streamError && hasFrame">
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

.stream-canvas {
  position: absolute;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  will-change: contents;
  transform: translateZ(0);
}

.stream-canvas.hidden {
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
