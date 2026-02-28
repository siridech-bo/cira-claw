<script setup lang="ts">
import { computed } from 'vue';
import { SOCKET_TYPE_COLORS } from '@gateway/socket-registry';

const props = defineProps<{
  data: {
    label: string;
    config: {
      condition: 'count_window' | 'consecutive' | 'rate' | 'sustained' | 'cooldown';
      accepts_socket_type: string;
      count: number;
      window_minutes: number;
    };
  };
}>();

const CONDITION_LABELS: Record<string, string> = {
  count_window: 'Count in Window',
  consecutive: 'Consecutive',
  rate: 'Rate per Minute',
  sustained: 'Sustained',
  cooldown: 'Cooldown',
};

const badgeColor = computed(() =>
  (SOCKET_TYPE_COLORS as Record<string, string>)[props.data.config.accepts_socket_type] ?? '#6B7280'
);

const conditionLabel = computed(() =>
  CONDITION_LABELS[props.data.config.condition] ?? props.data.config.condition
);

const params = computed(() => {
  const { count, window_minutes } = props.data.config;
  return `${count}x in ${window_minutes}min`;
});
</script>

<template>
  <div class="rn-stateful">
    <div class="rn-header">
      <span class="rn-label">STATEFUL</span>
      <span class="rn-badge" :style="{ background: badgeColor }">
        {{ data.config.accepts_socket_type }}
      </span>
    </div>
    <div class="rn-condition">{{ conditionLabel }}</div>
    <div class="rn-params">{{ params }}</div>
  </div>
</template>

<style scoped>
.rn-stateful {
  background: #1e293b;
  border: 1px solid #f59e0b;
  border-radius: 8px;
  padding: 10px 14px;
  min-width: 170px;
}

.rn-header {
  display: flex;
  gap: 8px;
  align-items: center;
  margin-bottom: 6px;
}

.rn-label {
  font-size: 9px;
  font-weight: 700;
  letter-spacing: 0.08em;
  color: #94a3b8;
  text-transform: uppercase;
}

.rn-badge {
  font-size: 10px;
  color: #fff;
  padding: 1px 7px;
  border-radius: 8px;
  font-weight: 600;
}

.rn-condition {
  font-size: 13px;
  font-weight: 600;
  color: #f1f5f9;
  margin-bottom: 4px;
}

.rn-params {
  font-size: 11px;
  color: #94a3b8;
}
</style>
