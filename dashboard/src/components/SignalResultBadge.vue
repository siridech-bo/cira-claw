<script setup lang="ts">
import { computed } from 'vue';

const props = defineProps<{
  resultJson: string | null;
  timestamp: string | null;
}>();

interface LabelProbResult {
  class: string;
  probability: number;
}

interface SoftmaxClass {
  label: string;
  prob: number;
}

interface SoftmaxResult {
  classes: SoftmaxClass[];
}

interface ReconstructionResult {
  anomaly_score: number;
  is_anomaly: boolean;
  threshold: number;
}

type ResultType = 'label_prob' | 'softmax' | 'reconstruction' | 'anomaly_score' | 'none';

const parsedResult = computed(() => {
  if (!props.resultJson) return null;

  try {
    const data = JSON.parse(props.resultJson);

    // Check for no prediction yet
    if (data.status === 'no_prediction_yet') return null;

    // Detect format
    if ('class' in data && 'probability' in data) {
      return { type: 'label_prob' as ResultType, data: data as LabelProbResult };
    }
    if ('classes' in data && Array.isArray(data.classes)) {
      return { type: 'softmax' as ResultType, data: data as SoftmaxResult };
    }
    if ('anomaly_score' in data && 'is_anomaly' in data) {
      return { type: 'reconstruction' as ResultType, data: data as ReconstructionResult };
    }
    if ('anomaly_score' in data && 'threshold' in data) {
      return { type: 'anomaly_score' as ResultType, data: data as ReconstructionResult };
    }

    return null;
  } catch {
    return null;
  }
});

// Sorted classes for softmax (descending by prob)
const sortedClasses = computed(() => {
  if (parsedResult.value?.type !== 'softmax') return [];
  const classes = [...(parsedResult.value.data as SoftmaxResult).classes];
  return classes.sort((a, b) => b.prob - a.prob);
});

// Bar colors for softmax
const barColors = ['#6366F1', '#22D3EE', '#F59E0B', '#94A3B8'];

function getBarColor(index: number): string {
  return barColors[Math.min(index, barColors.length - 1)];
}

// Format timestamp
const formattedTime = computed(() => {
  if (!props.timestamp) return null;
  try {
    const date = new Date(props.timestamp);
    return date.toLocaleTimeString('en-US', { hour12: false });
  } catch {
    return null;
  }
});

function formatPercent(val: number): string {
  return (val * 100).toFixed(1) + '%';
}
</script>

<template>
  <div class="signal-result-badge">
    <!-- No prediction yet -->
    <div v-if="!parsedResult" class="no-prediction">
      No prediction yet
    </div>

    <!-- Label + Probability format -->
    <div v-else-if="parsedResult.type === 'label_prob'" class="label-prob-result">
      <div class="result-row">
        <span class="class-label">{{ (parsedResult.data as LabelProbResult).class }}</span>
        <div class="bar-container">
          <div
            class="bar"
            :style="{
              width: ((parsedResult.data as LabelProbResult).probability * 100) + '%',
              background: '#6366F1'
            }"
          ></div>
        </div>
        <span class="probability">{{ formatPercent((parsedResult.data as LabelProbResult).probability) }}</span>
      </div>
    </div>

    <!-- Softmax format (multiple classes) -->
    <div v-else-if="parsedResult.type === 'softmax'" class="softmax-result">
      <div
        v-for="(cls, idx) in sortedClasses"
        :key="cls.label"
        class="result-row"
      >
        <span class="class-label">{{ cls.label }}</span>
        <div class="bar-container">
          <div
            class="bar"
            :style="{
              width: (cls.prob * 100) + '%',
              background: getBarColor(idx)
            }"
          ></div>
        </div>
        <span class="probability">{{ formatPercent(cls.prob) }}</span>
      </div>
    </div>

    <!-- Reconstruction / Anomaly Score format -->
    <div v-else-if="parsedResult.type === 'reconstruction' || parsedResult.type === 'anomaly_score'" class="anomaly-result">
      <div class="anomaly-gauge">
        <span class="gauge-label">Anomaly Score</span>
        <div class="gauge-bar-container">
          <div
            class="gauge-bar"
            :style="{
              width: Math.min((parsedResult.data as ReconstructionResult).anomaly_score / (parsedResult.data as ReconstructionResult).threshold * 100, 100) + '%',
              background: (parsedResult.data as ReconstructionResult).is_anomaly ? '#EF4444' : '#10B981'
            }"
          ></div>
          <div
            class="threshold-marker"
            :style="{ left: '100%' }"
            title="Threshold"
          ></div>
        </div>
        <span class="gauge-value">{{ (parsedResult.data as ReconstructionResult).anomaly_score.toFixed(3) }} / {{ (parsedResult.data as ReconstructionResult).threshold }}</span>
      </div>
      <div class="anomaly-badge-container">
        <span
          class="anomaly-badge"
          :class="(parsedResult.data as ReconstructionResult).is_anomaly ? 'anomaly' : 'normal'"
        >
          {{ (parsedResult.data as ReconstructionResult).is_anomaly ? 'ANOMALY' : 'NORMAL' }}
        </span>
      </div>
    </div>

    <!-- Timestamp -->
    <div v-if="formattedTime" class="timestamp">
      Last updated: {{ formattedTime }}
    </div>
  </div>
</template>

<style scoped>
.signal-result-badge {
  padding: 8px 0;
}

.no-prediction {
  color: #64748B;
  font-size: 0.875rem;
  padding: 12px 0;
}

.result-row {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 8px;
}

.class-label {
  min-width: 80px;
  font-size: 0.875rem;
  font-weight: 500;
  color: #E2E8F0;
}

.bar-container {
  flex: 1;
  height: 16px;
  background: #334155;
  border-radius: 4px;
  overflow: hidden;
}

.bar {
  height: 100%;
  border-radius: 4px;
  transition: width 0.3s ease;
}

.probability {
  min-width: 50px;
  text-align: right;
  font-size: 0.875rem;
  font-weight: 600;
  color: #94A3B8;
  font-family: 'SF Mono', 'Monaco', 'Consolas', monospace;
}

.anomaly-result {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.anomaly-gauge {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.gauge-label {
  font-size: 0.75rem;
  font-weight: 600;
  color: #64748B;
  text-transform: uppercase;
}

.gauge-bar-container {
  position: relative;
  height: 20px;
  background: #334155;
  border-radius: 4px;
  overflow: hidden;
}

.gauge-bar {
  height: 100%;
  border-radius: 4px;
  transition: width 0.3s ease;
}

.threshold-marker {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 2px;
  background: #EF4444;
  transform: translateX(-1px);
}

.gauge-value {
  font-size: 0.8125rem;
  color: #94A3B8;
  font-family: 'SF Mono', 'Monaco', 'Consolas', monospace;
}

.anomaly-badge-container {
  display: flex;
  justify-content: flex-start;
}

.anomaly-badge {
  padding: 4px 12px;
  border-radius: 4px;
  font-size: 0.75rem;
  font-weight: 700;
  text-transform: uppercase;
}

.anomaly-badge.normal {
  background: rgba(16, 185, 129, 0.2);
  color: #10B981;
}

.anomaly-badge.anomaly {
  background: rgba(239, 68, 68, 0.2);
  color: #EF4444;
}

.timestamp {
  margin-top: 8px;
  font-size: 0.75rem;
  color: #64748B;
}
</style>
