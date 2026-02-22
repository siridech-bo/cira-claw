<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, watch } from 'vue';

interface Node {
  id: string;
  name: string;
  host: string;
  status: string;
  runtime?: { port: number };
}

interface FileEntry {
  name: string;
  is_dir: boolean;
  is_image: boolean;
  size: number;
}

interface LocalImage {
  name: string;
  file: File;
  url: string;
}

interface Detection {
  label: string;
  confidence: number;
  x: number;
  y: number;
  w: number;
  h: number;
}

interface InferenceResult {
  success: boolean;
  detections: Detection[];
  inference_time_ms: number;
  error?: string;
}

// State
const nodes = ref<Node[]>([]);
const selectedNode = ref<Node | null>(null);
const sourceMode = ref<'local' | 'device'>('local');

// Local mode state
const localImages = ref<LocalImage[]>([]);
const folderInput = ref<HTMLInputElement | null>(null);

// Device mode state
const currentPath = ref('');
const fileEntries = ref<FileEntry[]>([]);
const pathHistory = ref<string[]>([]);

// Image viewing state
const currentIndex = ref(0);
const previewUrl = ref('');
const previewLoading = ref(false);

// Inference state
const inferenceResult = ref<InferenceResult | null>(null);
const inferenceLoading = ref(false);
const showAnnotations = ref(true);

// Auto-play state
const autoPlay = ref(false);
const autoPlayInterval = ref(2000);
const loopEnabled = ref(true);
let autoPlayTimer: number | null = null;

// Loading states
const loading = ref(true);
const filesLoading = ref(false);
const error = ref<string | null>(null);

// Computed
const onlineNodes = computed(() => nodes.value.filter(n => n.status === 'online'));

const images = computed(() => {
  if (sourceMode.value === 'local') {
    return localImages.value.map((img, i) => ({
      index: i,
      name: img.name,
      path: img.url,
      isLocal: true
    }));
  } else {
    return fileEntries.value
      .filter(f => f.is_image)
      .map((f, i) => ({
        index: i,
        name: f.name,
        path: `${currentPath.value}/${f.name}`,
        isLocal: false
      }));
  }
});

const currentImage = computed(() => images.value[currentIndex.value] || null);
const totalImages = computed(() => images.value.length);

// Lifecycle
onMounted(async () => {
  await fetchNodes();
});

onUnmounted(() => {
  stopAutoPlay();
  // Clean up blob URLs
  localImages.value.forEach(img => URL.revokeObjectURL(img.url));
});

// Watch for source mode changes
watch(sourceMode, () => {
  currentIndex.value = 0;
  previewUrl.value = '';
  inferenceResult.value = null;
  stopAutoPlay();
});

// Fetch nodes
async function fetchNodes() {
  try {
    loading.value = true;
    const response = await fetch('/api/nodes');
    if (!response.ok) throw new Error('Failed to fetch');
    const data = await response.json();
    nodes.value = data.nodes;

    if (onlineNodes.value.length > 0) {
      selectedNode.value = onlineNodes.value[0];
    }
  } catch (e) {
    error.value = 'Failed to load devices';
  } finally {
    loading.value = false;
  }
}

// Local folder selection
function selectLocalFolder() {
  folderInput.value?.click();
}

function handleFolderSelect(event: Event) {
  const input = event.target as HTMLInputElement;
  const files = input.files;
  if (!files) return;

  // Clean up old URLs
  localImages.value.forEach(img => URL.revokeObjectURL(img.url));
  localImages.value = [];

  // Filter image files and create blob URLs
  const imageFiles = Array.from(files).filter(f =>
    /\.(jpg|jpeg|png|bmp|gif|webp)$/i.test(f.name)
  );

  localImages.value = imageFiles.map(file => ({
    name: file.name,
    file: file,
    url: URL.createObjectURL(file)
  }));

  currentIndex.value = 0;
  if (localImages.value.length > 0) {
    loadPreview(0);
  }
}

// Device file browsing
async function browsePath(path: string) {
  if (!selectedNode.value) return;

  try {
    filesLoading.value = true;
    error.value = null;

    const baseUrl = `http://${selectedNode.value.host}:${selectedNode.value.runtime?.port || 8080}`;
    const response = await fetch(`${baseUrl}/api/files?path=${encodeURIComponent(path)}`);

    if (!response.ok) throw new Error('Failed to fetch');

    const data = await response.json();
    if (data.error) {
      error.value = data.error;
      return;
    }

    currentPath.value = data.path;
    fileEntries.value = data.entries || [];

    // Sort: directories first, then files
    fileEntries.value.sort((a, b) => {
      if (a.is_dir && !b.is_dir) return -1;
      if (!a.is_dir && b.is_dir) return 1;
      return a.name.localeCompare(b.name);
    });

    currentIndex.value = 0;
    const imageList = fileEntries.value.filter(f => f.is_image);
    if (imageList.length > 0) {
      loadPreview(0);
    } else {
      previewUrl.value = '';
    }
  } catch (e) {
    error.value = 'Failed to browse directory';
  } finally {
    filesLoading.value = false;
  }
}

function navigateToFolder(entry: FileEntry) {
  if (!entry.is_dir) return;
  pathHistory.value.push(currentPath.value);
  browsePath(`${currentPath.value}/${entry.name}`);
}

function navigateUp() {
  if (pathHistory.value.length > 0) {
    const prevPath = pathHistory.value.pop()!;
    browsePath(prevPath);
  } else {
    // Go to parent
    const parts = currentPath.value.split('/').filter(p => p);
    if (parts.length > 1) {
      parts.pop();
      browsePath('/' + parts.join('/'));
    }
  }
}

// Preview loading
function loadPreview(index: number) {
  if (index < 0 || index >= images.value.length) return;

  currentIndex.value = index;
  const img = images.value[index];

  if (img.isLocal) {
    previewUrl.value = img.path;
    // Auto-run inference for local images
    runInferenceLocal(index);
  } else {
    // For device images, we'd need to fetch through the runtime
    // For now, show a placeholder
    previewUrl.value = '';
    inferenceResult.value = null;
  }
}

// Navigation
function prevImage() {
  if (currentIndex.value > 0) {
    loadPreview(currentIndex.value - 1);
  } else if (loopEnabled.value && totalImages.value > 0) {
    loadPreview(totalImages.value - 1);
  }
}

function nextImage() {
  if (currentIndex.value < totalImages.value - 1) {
    loadPreview(currentIndex.value + 1);
  } else if (loopEnabled.value && totalImages.value > 0) {
    loadPreview(0);
  }
}

// Auto-play
function toggleAutoPlay() {
  if (autoPlay.value) {
    stopAutoPlay();
  } else {
    startAutoPlay();
  }
}

function startAutoPlay() {
  autoPlay.value = true;
  autoPlayTimer = window.setInterval(() => {
    nextImage();
  }, autoPlayInterval.value);
}

function stopAutoPlay() {
  autoPlay.value = false;
  if (autoPlayTimer) {
    clearInterval(autoPlayTimer);
    autoPlayTimer = null;
  }
}

// Inference
async function runInferenceLocal(index: number) {
  if (!selectedNode.value) return;

  const img = localImages.value[index];
  if (!img) return;

  try {
    inferenceLoading.value = true;
    inferenceResult.value = null;

    // For local images, we need to upload to the runtime
    // For now, simulate the result or show placeholder
    // TODO: Implement actual inference endpoint with image upload

    // Simulated delay
    await new Promise(resolve => setTimeout(resolve, 500));

    // Placeholder result
    inferenceResult.value = {
      success: true,
      detections: [],
      inference_time_ms: 0,
      error: 'Image upload inference not yet implemented. Use camera stream for real-time detection.'
    };
  } catch (e) {
    inferenceResult.value = {
      success: false,
      detections: [],
      inference_time_ms: 0,
      error: 'Failed to run inference'
    };
  } finally {
    inferenceLoading.value = false;
  }
}

function selectNode(node: Node) {
  selectedNode.value = node;
  if (sourceMode.value === 'device') {
    browsePath('/home');
  }
}
</script>

<template>
  <div class="image-tester">
    <header class="page-header">
      <h2>Image Tester</h2>
      <p class="subtitle">Test model inference on static images</p>
    </header>

    <div class="loading" v-if="loading">Loading...</div>

    <div class="content" v-else>
      <!-- Controls Row -->
      <div class="controls-row">
        <!-- Device Selector -->
        <div class="control-group">
          <label>Device:</label>
          <select v-model="selectedNode" @change="selectNode(selectedNode!)">
            <option v-for="node in onlineNodes" :key="node.id" :value="node">
              {{ node.name }}
            </option>
          </select>
        </div>

        <!-- Source Mode Toggle -->
        <div class="control-group source-toggle">
          <button
            :class="{ active: sourceMode === 'local' }"
            @click="sourceMode = 'local'"
          >
            Local
          </button>
          <button
            :class="{ active: sourceMode === 'device' }"
            @click="sourceMode = 'device'; browsePath('/home')"
          >
            Device
          </button>
        </div>

        <!-- Show Annotations Toggle -->
        <div class="control-group">
          <label>
            <input type="checkbox" v-model="showAnnotations" />
            Show Annotations
          </label>
        </div>
      </div>

      <!-- Main Content Grid -->
      <div class="main-grid">
        <!-- File Browser Panel -->
        <div class="panel browser-panel">
          <div class="panel-header">
            <h3>{{ sourceMode === 'local' ? 'Local Images' : 'Device Files' }}</h3>
          </div>

          <!-- Local Mode -->
          <div v-if="sourceMode === 'local'" class="local-browser">
            <input
              ref="folderInput"
              type="file"
              webkitdirectory
              multiple
              accept="image/*"
              style="display: none"
              @change="handleFolderSelect"
            />
            <button class="browse-btn" @click="selectLocalFolder">
              Select Folder
            </button>

            <div class="file-list" v-if="localImages.length > 0">
              <div
                v-for="(img, i) in localImages"
                :key="img.name"
                class="file-item"
                :class="{ active: currentIndex === i }"
                @click="loadPreview(i)"
              >
                <span class="file-icon">🖼️</span>
                <span class="file-name">{{ img.name }}</span>
              </div>
            </div>

            <div class="empty-state" v-else>
              <p>Select a folder to browse images</p>
            </div>
          </div>

          <!-- Device Mode -->
          <div v-else class="device-browser">
            <div class="path-bar">
              <button class="up-btn" @click="navigateUp">⬆️</button>
              <input
                type="text"
                v-model="currentPath"
                @keyup.enter="browsePath(currentPath)"
                placeholder="/path/to/images"
              />
              <button class="go-btn" @click="browsePath(currentPath)">Go</button>
            </div>

            <div class="file-list" v-if="!filesLoading">
              <div
                v-for="entry in fileEntries"
                :key="entry.name"
                class="file-item"
                :class="{
                  folder: entry.is_dir,
                  image: entry.is_image,
                  active: entry.is_image && images.findIndex(img => img.name === entry.name) === currentIndex
                }"
                @click="entry.is_dir ? navigateToFolder(entry) : (entry.is_image && loadPreview(images.findIndex(img => img.name === entry.name)))"
              >
                <span class="file-icon">{{ entry.is_dir ? '📁' : (entry.is_image ? '🖼️' : '📄') }}</span>
                <span class="file-name">{{ entry.name }}</span>
              </div>

              <div class="empty-state" v-if="fileEntries.length === 0">
                <p>No files in this directory</p>
              </div>
            </div>

            <div class="loading-files" v-else>Loading...</div>
          </div>
        </div>

        <!-- Preview Panel -->
        <div class="panel preview-panel">
          <div class="panel-header">
            <h3>Preview</h3>
            <span class="image-counter" v-if="totalImages > 0">
              {{ currentIndex + 1 }} / {{ totalImages }}
            </span>
          </div>

          <div class="preview-container">
            <img
              v-if="previewUrl"
              :src="previewUrl"
              class="preview-image"
              :class="{ annotated: showAnnotations }"
            />
            <div class="no-preview" v-else>
              <p v-if="sourceMode === 'device'">
                Device image preview not yet supported.<br/>
                Select Local mode to test images.
              </p>
              <p v-else>No image selected</p>
            </div>

            <!-- Navigation Controls -->
            <div class="nav-controls" v-if="totalImages > 1">
              <button class="nav-btn prev" @click="prevImage">&lt;</button>
              <button class="nav-btn next" @click="nextImage">&gt;</button>
            </div>
          </div>

          <!-- Playback Controls -->
          <div class="playback-controls" v-if="totalImages > 1">
            <button
              class="play-btn"
              :class="{ playing: autoPlay }"
              @click="toggleAutoPlay"
            >
              {{ autoPlay ? '⏸️ Pause' : '▶️ Play' }}
            </button>
            <div class="interval-control">
              <label>Interval:</label>
              <select v-model.number="autoPlayInterval" @change="autoPlay && (stopAutoPlay(), startAutoPlay())">
                <option :value="500">0.5s</option>
                <option :value="1000">1s</option>
                <option :value="2000">2s</option>
                <option :value="3000">3s</option>
                <option :value="5000">5s</option>
              </select>
            </div>
            <label class="loop-control">
              <input type="checkbox" v-model="loopEnabled" />
              Loop
            </label>
          </div>
        </div>

        <!-- Results Panel -->
        <div class="panel results-panel">
          <div class="panel-header">
            <h3>Inference Results</h3>
          </div>

          <div class="results-content">
            <div class="loading-inference" v-if="inferenceLoading">
              <span class="spinner"></span>
              Running inference...
            </div>

            <div class="inference-result" v-else-if="inferenceResult">
              <div class="result-status" :class="{ success: inferenceResult.success, error: !inferenceResult.success }">
                {{ inferenceResult.success ? 'Success' : 'Error' }}
              </div>

              <div class="inference-time" v-if="inferenceResult.inference_time_ms > 0">
                Time: {{ inferenceResult.inference_time_ms.toFixed(1) }}ms
              </div>

              <div class="detections-list" v-if="inferenceResult.detections.length > 0">
                <div
                  v-for="(det, i) in inferenceResult.detections"
                  :key="i"
                  class="detection-item"
                >
                  <span class="det-label">{{ det.label }}</span>
                  <div class="confidence-bar">
                    <div class="confidence-fill" :style="{ width: (det.confidence * 100) + '%' }"></div>
                  </div>
                  <span class="det-conf">{{ (det.confidence * 100).toFixed(1) }}%</span>
                </div>
              </div>

              <div class="no-detections" v-else-if="!inferenceResult.error">
                No detections
              </div>

              <div class="error-message" v-if="inferenceResult.error">
                {{ inferenceResult.error }}
              </div>
            </div>

            <div class="no-result" v-else>
              <p>Select an image to run inference</p>
            </div>
          </div>
        </div>
      </div>

      <!-- Error Message -->
      <div class="message error" v-if="error">{{ error }}</div>
    </div>
  </div>
</template>

<style scoped>
.image-tester {
  max-width: 1400px;
  margin: 0 auto;
}

.page-header {
  margin-bottom: 24px;
}

.page-header h2 {
  font-size: 1.5rem;
  font-weight: 600;
  margin-bottom: 4px;
}

.subtitle {
  color: #64748b;
  font-size: 0.875rem;
}

.loading {
  text-align: center;
  padding: 48px;
  color: #64748b;
}

.content {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

/* Controls Row */
.controls-row {
  display: flex;
  align-items: center;
  gap: 24px;
  padding: 16px 20px;
  background: white;
  border-radius: 12px;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1);
}

.control-group {
  display: flex;
  align-items: center;
  gap: 8px;
}

.control-group label {
  font-size: 0.875rem;
  color: #64748b;
}

.control-group select {
  padding: 6px 12px;
  border: 1px solid #e2e8f0;
  border-radius: 6px;
  font-size: 0.875rem;
}

.source-toggle {
  display: flex;
  background: #f1f5f9;
  border-radius: 6px;
  padding: 2px;
}

.source-toggle button {
  padding: 6px 16px;
  border: none;
  background: transparent;
  border-radius: 4px;
  cursor: pointer;
  font-size: 0.875rem;
  transition: all 0.2s;
}

.source-toggle button.active {
  background: white;
  box-shadow: 0 1px 2px rgba(0, 0, 0, 0.1);
}

/* Main Grid */
.main-grid {
  display: grid;
  grid-template-columns: 280px 1fr 300px;
  gap: 16px;
  min-height: 600px;
}

/* Panels */
.panel {
  background: white;
  border-radius: 12px;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1);
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.panel-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 16px;
  border-bottom: 1px solid #e2e8f0;
}

.panel-header h3 {
  font-size: 0.875rem;
  font-weight: 600;
}

.image-counter {
  font-size: 0.75rem;
  color: #64748b;
  background: #f1f5f9;
  padding: 4px 8px;
  border-radius: 4px;
}

/* Browser Panel */
.browser-panel {
  overflow: hidden;
}

.local-browser,
.device-browser {
  flex: 1;
  display: flex;
  flex-direction: column;
  padding: 12px;
  overflow: hidden;
}

.browse-btn {
  width: 100%;
  padding: 12px;
  background: #2563eb;
  color: white;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  font-weight: 500;
  margin-bottom: 12px;
}

.browse-btn:hover {
  background: #1d4ed8;
}

.path-bar {
  display: flex;
  gap: 8px;
  margin-bottom: 12px;
}

.path-bar input {
  flex: 1;
  padding: 8px;
  border: 1px solid #e2e8f0;
  border-radius: 6px;
  font-size: 0.75rem;
  font-family: monospace;
}

.up-btn,
.go-btn {
  padding: 8px 12px;
  background: #f1f5f9;
  border: 1px solid #e2e8f0;
  border-radius: 6px;
  cursor: pointer;
}

.file-list {
  flex: 1;
  overflow-y: auto;
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.file-item {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 12px;
  border-radius: 6px;
  cursor: pointer;
  transition: background 0.15s;
}

.file-item:hover {
  background: #f1f5f9;
}

.file-item.active {
  background: #eff6ff;
  border: 1px solid #2563eb;
}

.file-item.folder {
  color: #2563eb;
}

.file-icon {
  font-size: 1rem;
}

.file-name {
  font-size: 0.75rem;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

/* Preview Panel */
.preview-panel {
  display: flex;
  flex-direction: column;
}

.preview-container {
  flex: 1;
  position: relative;
  background: #1e293b;
  display: flex;
  align-items: center;
  justify-content: center;
  min-height: 400px;
}

.preview-image {
  max-width: 100%;
  max-height: 100%;
  object-fit: contain;
}

.no-preview {
  text-align: center;
  color: #64748b;
  padding: 24px;
}

.nav-controls {
  position: absolute;
  left: 0;
  right: 0;
  top: 50%;
  transform: translateY(-50%);
  display: flex;
  justify-content: space-between;
  padding: 0 8px;
  pointer-events: none;
}

.nav-btn {
  width: 40px;
  height: 40px;
  background: rgba(0, 0, 0, 0.5);
  color: white;
  border: none;
  border-radius: 50%;
  cursor: pointer;
  font-size: 1.25rem;
  pointer-events: auto;
  transition: background 0.2s;
}

.nav-btn:hover {
  background: rgba(0, 0, 0, 0.7);
}

.playback-controls {
  display: flex;
  align-items: center;
  gap: 16px;
  padding: 12px 16px;
  border-top: 1px solid #e2e8f0;
}

.play-btn {
  padding: 8px 16px;
  background: #f1f5f9;
  border: 1px solid #e2e8f0;
  border-radius: 6px;
  cursor: pointer;
  font-size: 0.875rem;
}

.play-btn.playing {
  background: #fef3c7;
  border-color: #fbbf24;
}

.interval-control {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 0.75rem;
  color: #64748b;
}

.interval-control select {
  padding: 4px 8px;
  border: 1px solid #e2e8f0;
  border-radius: 4px;
  font-size: 0.75rem;
}

.loop-control {
  display: flex;
  align-items: center;
  gap: 4px;
  font-size: 0.75rem;
  color: #64748b;
}

/* Results Panel */
.results-panel {
  display: flex;
  flex-direction: column;
}

.results-content {
  flex: 1;
  padding: 16px;
  overflow-y: auto;
}

.loading-inference {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 24px;
  color: #64748b;
}

.spinner {
  width: 20px;
  height: 20px;
  border: 2px solid #e2e8f0;
  border-top-color: #2563eb;
  border-radius: 50%;
  animation: spin 1s linear infinite;
}

@keyframes spin {
  to { transform: rotate(360deg); }
}

.result-status {
  padding: 8px 12px;
  border-radius: 6px;
  font-size: 0.875rem;
  font-weight: 500;
  margin-bottom: 12px;
}

.result-status.success {
  background: #f0fdf4;
  color: #166534;
}

.result-status.error {
  background: #fef2f2;
  color: #991b1b;
}

.inference-time {
  font-size: 0.75rem;
  color: #64748b;
  margin-bottom: 16px;
}

.detections-list {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.detection-item {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 12px;
  background: #f8fafc;
  border-radius: 6px;
}

.det-label {
  font-size: 0.75rem;
  font-weight: 500;
  min-width: 80px;
}

.confidence-bar {
  flex: 1;
  height: 8px;
  background: #e2e8f0;
  border-radius: 4px;
  overflow: hidden;
}

.confidence-fill {
  height: 100%;
  background: #22c55e;
  transition: width 0.3s;
}

.det-conf {
  font-size: 0.75rem;
  color: #64748b;
  min-width: 48px;
  text-align: right;
}

.no-detections,
.no-result {
  text-align: center;
  padding: 24px;
  color: #64748b;
  font-size: 0.875rem;
}

.error-message {
  padding: 12px;
  background: #fef2f2;
  color: #991b1b;
  border-radius: 6px;
  font-size: 0.75rem;
  margin-top: 12px;
}

.empty-state {
  text-align: center;
  padding: 32px;
  color: #64748b;
  font-size: 0.875rem;
}

.loading-files {
  text-align: center;
  padding: 32px;
  color: #64748b;
}

.message.error {
  padding: 12px 16px;
  background: #fef2f2;
  color: #991b1b;
  border: 1px solid #fecaca;
  border-radius: 8px;
  font-size: 0.875rem;
}

@media (max-width: 1024px) {
  .main-grid {
    grid-template-columns: 1fr;
  }

  .browser-panel {
    order: 1;
  }

  .preview-panel {
    order: 0;
  }

  .results-panel {
    order: 2;
  }
}
</style>
