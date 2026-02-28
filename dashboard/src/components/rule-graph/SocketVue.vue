<script setup lang="ts">
import { computed } from 'vue';
import { SOCKET_TYPE_COLORS } from '@gateway/socket-registry';

const props = defineProps<{ data: { name: string } }>();

const color = computed(() =>
  (SOCKET_TYPE_COLORS as Record<string, string>)[props.data.name] ?? '#6B7280'
);

// Shape map per spec
const shapeMap: Record<string, string> = {
  'vision.detection': 'circle',
  'vision.confidence': 'diamond',
  'signal.threshold': 'square',
  'signal.rate': 'triangle',
  'system.health': 'hexagon',
  'any.boolean': 'dashed-circle',
  'time.window': 'hourglass',
  'boolean.any': 'circle',
};
const shape = computed(() => shapeMap[props.data.name] ?? 'circle');

const borderRadius = computed(() => {
  if (shape.value === 'circle' || shape.value === 'dashed-circle') return '50%';
  if (shape.value === 'diamond') return '2px';
  return '2px';
});

const borderStyle = computed(() => {
  if (shape.value === 'dashed-circle') return `2px dashed ${color.value}`;
  return 'none';
});

const transform = computed(() => {
  if (shape.value === 'diamond') return 'rotate(45deg)';
  return 'none';
});
</script>

<template>
  <div
    class="socket-dot"
    :title="data.name"
    :style="{
      width: '12px',
      height: '12px',
      borderRadius: borderRadius,
      background: shape === 'dashed-circle' ? 'transparent' : color,
      border: borderStyle,
      display: 'inline-block',
      transform: transform,
    }"
  />
</template>

<style scoped>
.socket-dot {
  cursor: crosshair;
  transition: transform 0.15s ease;
}

.socket-dot:hover {
  transform: scale(1.2);
}
</style>
