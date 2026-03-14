<script setup lang="ts">
import { ref, watch, computed } from 'vue';

const props = defineProps<{
  name: string;
  rms: number;
  peak: number;
  mean: number;
  windowReady: boolean;
  sampleCount?: number;
}>();

// Internal ring buffer for sparkline (64 points)
const ringBuffer = ref<number[]>([]);
const BUFFER_SIZE = 64;

// Watch for rms changes and update ring buffer
watch(() => props.rms, (newRms) => {
  if (typeof newRms === 'number' && !isNaN(newRms)) {
    ringBuffer.value.push(newRms);
    if (ringBuffer.value.length > BUFFER_SIZE) {
      ringBuffer.value.shift();
    }
  }
});

// Compute SVG polyline points
const sparklinePoints = computed(() => {
  const buffer = ringBuffer.value;
  if (buffer.length < 2) return '';

  const width = 200;
  const height = 50;
  const padding = 4;
  const drawHeight = height - padding * 2;

  // Find min/max for normalization
  const minVal = Math.min(...buffer);
  const maxVal = Math.max(...buffer);
  const range = maxVal - minVal || 1;

  // Generate points
  const points: string[] = [];
  for (let i = 0; i < buffer.length; i++) {
    const x = (i / (buffer.length - 1)) * width;
    const normalizedY = (buffer[i] - minVal) / range;
    const y = height - padding - (normalizedY * drawHeight);
    points.push(`${x.toFixed(1)},${y.toFixed(1)}`);
  }

  return points.join(' ');
});

// Format number for display
function formatValue(val: number): string {
  if (Math.abs(val) < 0.001) return val.toExponential(2);
  return val.toFixed(3);
}
</script>

<template>
  <div class="signal-channel-card">
    <div class="card-header">
      <span class="channel-name">{{ name }}</span>
      <span class="readiness" :class="windowReady ? 'ready' : 'waiting'">
        <span class="dot"></span>
        <span v-if="sampleCount !== undefined">{{ sampleCount }}/128</span>
        <span v-else>{{ windowReady ? 'Ready' : 'Waiting' }}</span>
      </span>
    </div>

    <div class="sparkline-container">
      <svg width="200" height="50" class="sparkline">
        <polyline
          v-if="sparklinePoints"
          :points="sparklinePoints"
          fill="none"
          stroke="#6366F1"
          stroke-width="1.5"
          stroke-linejoin="round"
          stroke-linecap="round"
        />
        <text v-else x="100" y="30" text-anchor="middle" fill="#64748B" font-size="11">
          Collecting data...
        </text>
      </svg>
    </div>

    <div class="stats">
      <div class="stat">
        <span class="stat-label">RMS</span>
        <span class="stat-value">{{ formatValue(rms) }}</span>
      </div>
      <div class="stat">
        <span class="stat-label">Peak</span>
        <span class="stat-value">{{ formatValue(peak) }}</span>
      </div>
      <div class="stat">
        <span class="stat-label">Mean</span>
        <span class="stat-value">{{ formatValue(mean) }}</span>
      </div>
    </div>
  </div>
</template>

<style scoped>
.signal-channel-card {
  background: #1E293B;
  border: 1px solid #334155;
  border-radius: 10px;
  padding: 12px;
  min-width: 220px;
}

.card-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 8px;
}

.channel-name {
  font-size: 0.875rem;
  font-weight: 600;
  color: #E2E8F0;
}

.readiness {
  display: flex;
  align-items: center;
  gap: 4px;
  font-size: 0.75rem;
  color: #94A3B8;
}

.dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #F59E0B;
}

.readiness.ready .dot {
  background: #10B981;
}

.sparkline-container {
  background: #0F172A;
  border-radius: 6px;
  padding: 4px;
  margin-bottom: 8px;
}

.sparkline {
  display: block;
  width: 100%;
  height: 50px;
}

.stats {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
}

.stat {
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.stat-label {
  font-size: 0.625rem;
  font-weight: 600;
  color: #64748B;
  text-transform: uppercase;
}

.stat-value {
  font-size: 0.8125rem;
  font-weight: 500;
  color: #E2E8F0;
  font-family: 'SF Mono', 'Monaco', 'Consolas', monospace;
}
</style>
