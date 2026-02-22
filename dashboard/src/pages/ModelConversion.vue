<script setup lang="ts">
import { ref, onMounted } from 'vue';

interface DarknetModel {
  name: string;
  path: string;
  cfgFile: string;
  weightsFile: string;
}

interface ConversionJob {
  model: string;
  status: 'pending' | 'running' | 'success' | 'error';
  message: string;
  progress: number;
}

const darknetModels = ref<DarknetModel[]>([]);
const selectedModel = ref<string>('');
const converting = ref(false);
const conversionJob = ref<ConversionJob | null>(null);
const error = ref<string | null>(null);
const darknet2ncnnPath = ref('darknet2ncnn'); // Default assumes it's in PATH

onMounted(async () => {
  await scanForDarknetModels();
});

async function scanForDarknetModels() {
  try {
    // Fetch models list from runtime
    const response = await fetch('/api/nodes/local-dev/models');
    if (!response.ok) return;

    const data = await response.json();

    // Filter for Darknet models (those with .cfg and .weights files)
    // For now, show placeholder since we need backend support
    darknetModels.value = [];

    // TODO: Backend needs to return Darknet models separately
  } catch (e) {
    console.error('Failed to scan models:', e);
  }
}

async function startConversion() {
  if (!selectedModel.value) {
    error.value = 'Please select a model to convert';
    return;
  }

  converting.value = true;
  error.value = null;
  conversionJob.value = {
    model: selectedModel.value,
    status: 'running',
    message: 'Starting conversion...',
    progress: 0,
  };

  try {
    const response = await fetch('/api/utility/convert-model', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        sourcePath: selectedModel.value,
        targetFormat: 'ncnn',
        darknet2ncnnPath: darknet2ncnnPath.value,
      }),
    });

    const data = await response.json();

    if (!response.ok || !data.success) {
      throw new Error(data.error || data.message || 'Conversion failed');
    }

    conversionJob.value = {
      model: selectedModel.value,
      status: 'success',
      message: `Converted successfully! Output: ${data.outputPath}`,
      progress: 100,
    };
  } catch (e) {
    conversionJob.value = {
      model: selectedModel.value,
      status: 'error',
      message: e instanceof Error ? e.message : 'Unknown error',
      progress: 0,
    };
    error.value = e instanceof Error ? e.message : 'Conversion failed';
  } finally {
    converting.value = false;
  }
}
</script>

<template>
  <div class="model-conversion">
    <h1>Model Conversion</h1>
    <p class="description">
      Convert Darknet models (.cfg + .weights) to NCNN format (.param + .bin) for optimized edge inference.
    </p>

    <div class="conversion-panel">
      <div class="panel-section">
        <h2>Darknet to NCNN Converter</h2>

        <div class="form-group">
          <label>darknet2ncnn Path</label>
          <input
            v-model="darknet2ncnnPath"
            type="text"
            placeholder="Path to darknet2ncnn executable"
            :disabled="converting"
          />
          <span class="hint">Leave as 'darknet2ncnn' if it's in your PATH</span>
        </div>

        <div class="form-group">
          <label>Source Model</label>
          <div class="input-group">
            <input
              v-model="selectedModel"
              type="text"
              placeholder="Path to Darknet model directory (with .cfg and .weights)"
              :disabled="converting"
            />
          </div>
          <span class="hint">e.g., D:/models/yolov4-tiny-darknet</span>
        </div>

        <div class="form-group" v-if="darknetModels.length > 0">
          <label>Or Select from Available Models</label>
          <select v-model="selectedModel" :disabled="converting">
            <option value="">-- Select a model --</option>
            <option v-for="model in darknetModels" :key="model.path" :value="model.path">
              {{ model.name }}
            </option>
          </select>
        </div>

        <button
          class="convert-btn"
          @click="startConversion"
          :disabled="converting || !selectedModel"
        >
          {{ converting ? 'Converting...' : 'Convert to NCNN' }}
        </button>

        <div v-if="conversionJob" class="job-status" :class="conversionJob.status">
          <div class="status-header">
            <span class="status-icon">
              {{ conversionJob.status === 'running' ? '⏳' :
                 conversionJob.status === 'success' ? '✓' : '✗' }}
            </span>
            <span class="status-text">{{ conversionJob.message }}</span>
          </div>
          <div v-if="conversionJob.status === 'running'" class="progress-bar">
            <div class="progress-fill" :style="{ width: conversionJob.progress + '%' }"></div>
          </div>
        </div>

        <div v-if="error" class="error-message">
          {{ error }}
        </div>
      </div>

      <div class="panel-section info-section">
        <h3>Requirements</h3>
        <ul>
          <li>
            <strong>darknet2ncnn</strong> - Build from
            <a href="https://github.com/xiangweizeng/darknet2ncnn" target="_blank">darknet2ncnn repo</a>
          </li>
          <li>Model directory must contain:
            <ul>
              <li><code>.cfg</code> file (network architecture)</li>
              <li><code>.weights</code> file (trained weights)</li>
              <li><code>obj.names</code> or <code>labels.txt</code> (class names)</li>
            </ul>
          </li>
        </ul>

        <h3>Output</h3>
        <p>Creates a new NCNN model directory with:</p>
        <ul>
          <li><code>.param</code> - Network architecture</li>
          <li><code>.bin</code> - Weights in NCNN format</li>
          <li><code>cira_model.json</code> - Model manifest</li>
          <li>Class labels file (copied)</li>
        </ul>
      </div>
    </div>
  </div>
</template>

<style scoped>
.model-conversion {
  max-width: 1000px;
}

h1 {
  font-size: 1.75rem;
  font-weight: 600;
  margin-bottom: 8px;
}

.description {
  color: #64748b;
  margin-bottom: 24px;
}

.conversion-panel {
  display: grid;
  grid-template-columns: 1fr 350px;
  gap: 24px;
}

.panel-section {
  background: #1E293B;
  border-radius: 12px;
  padding: 24px;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1);
}

.panel-section h2 {
  font-size: 1.25rem;
  font-weight: 600;
  margin-bottom: 20px;
  color: #E2E8F0;
}

.panel-section h3 {
  font-size: 1rem;
  font-weight: 600;
  margin-top: 16px;
  margin-bottom: 8px;
  color: #334155;
}

.form-group {
  margin-bottom: 20px;
}

.form-group label {
  display: block;
  font-weight: 500;
  margin-bottom: 6px;
  color: #334155;
}

.form-group input,
.form-group select {
  width: 100%;
  padding: 10px 12px;
  border: 1px solid #334155;
  border-radius: 6px;
  font-size: 0.9rem;
}

.form-group input:focus,
.form-group select:focus {
  outline: none;
  border-color: #6366F1;
  box-shadow: 0 0 0 3px rgba(37, 99, 235, 0.1);
}

.form-group input:disabled,
.form-group select:disabled {
  background: #334155;
  cursor: not-allowed;
}

.hint {
  display: block;
  font-size: 0.75rem;
  color: #94a3b8;
  margin-top: 4px;
}

.input-group {
  display: flex;
  gap: 8px;
}

.input-group input {
  flex: 1;
}

.convert-btn {
  width: 100%;
  padding: 12px 20px;
  background: #6366F1;
  color: white;
  border: none;
  border-radius: 6px;
  font-size: 1rem;
  font-weight: 500;
  cursor: pointer;
  transition: background 0.2s;
}

.convert-btn:hover:not(:disabled) {
  background: #1d4ed8;
}

.convert-btn:disabled {
  background: #94a3b8;
  cursor: not-allowed;
}

.job-status {
  margin-top: 16px;
  padding: 12px;
  border-radius: 6px;
}

.job-status.running {
  background: #eff6ff;
  border: 1px solid #bfdbfe;
}

.job-status.success {
  background: #f0fdf4;
  border: 1px solid #86efac;
}

.job-status.error {
  background: #fef2f2;
  border: 1px solid #fecaca;
}

.status-header {
  display: flex;
  align-items: center;
  gap: 8px;
}

.status-icon {
  font-size: 1.25rem;
}

.job-status.success .status-icon {
  color: #16a34a;
}

.job-status.error .status-icon {
  color: #dc2626;
}

.progress-bar {
  margin-top: 8px;
  height: 4px;
  background: #dbeafe;
  border-radius: 2px;
  overflow: hidden;
}

.progress-fill {
  height: 100%;
  background: #6366F1;
  transition: width 0.3s;
}

.error-message {
  margin-top: 12px;
  padding: 10px 12px;
  background: #fef2f2;
  color: #dc2626;
  border-radius: 6px;
  font-size: 0.875rem;
}

.info-section {
  background: #0F172A;
}

.info-section ul {
  margin: 0;
  padding-left: 20px;
}

.info-section li {
  margin-bottom: 8px;
  color: #475569;
  font-size: 0.875rem;
}

.info-section code {
  background: #334155;
  padding: 2px 6px;
  border-radius: 4px;
  font-size: 0.8rem;
}

.info-section a {
  color: #6366F1;
}

@media (max-width: 900px) {
  .conversion-panel {
    grid-template-columns: 1fr;
  }
}
</style>
