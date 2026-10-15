<script setup lang="ts">
import { computed, onBeforeUnmount, ref } from 'vue';

const props = withDefaults(
  defineProps<{
    modelValue?: number;
    min?: number;
    max?: number;
    step?: number;
    label?: string;
    size?: number;
    disabled?: boolean;
  }>(),
  {
    modelValue: 50,
    min: 0,
    max: 100,
    step: 1,
    label: 'Volume',
    size: 64,
    disabled: false,
  }
);

const emit = defineEmits<{
  (e: 'update:modelValue', value: number): void;
  (e: 'change', value: number): void;
}>();

// The knob indicator sweeps from -135deg to +135deg (270deg in total).
const SWEEP_DEGREES = 135;

// Dragging the pointer this many pixels upwards moves the value from min to max.
const DRAG_RANGE_PX = 150;

const dragging = ref(false);
const dragStartY = ref(0);
const dragStartValue = ref(0);
// Last value emitted during the current drag; null until the pointer moved.
const dragValue = ref<number | null>(null);

const displayValue = computed(() => Math.round(props.modelValue));

const angle = computed(() => {
  const range = props.max - props.min;
  if (range === 0) return 0;
  const t = Math.min(1, Math.max(0, (props.modelValue - props.min) / range));
  return -SWEEP_DEGREES + t * SWEEP_DEGREES * 2;
});

function quantize(value: number): number {
  const step = props.step > 0 ? props.step : 1;
  const steps = Math.round((value - props.min) / step);
  const snapped = props.min + steps * step;
  // toFixed guards against floating point drift (e.g. 0.1 steps)
  return Math.min(props.max, Math.max(props.min, Number(snapped.toFixed(6))));
}

function commit(value: number): void {
  const next = quantize(value);
  if (next !== props.modelValue) {
    emit('update:modelValue', next);
  }
}

function onPointerDown(event: PointerEvent): void {
  if (props.disabled) return;
  event.preventDefault();
  dragging.value = true;
  dragStartY.value = event.clientY;
  dragStartValue.value = props.modelValue;
  dragValue.value = null;
  window.addEventListener('pointermove', onPointerMove);
  window.addEventListener('pointerup', onPointerUp);
  window.addEventListener('pointercancel', onPointerUp);
}

function onPointerMove(event: PointerEvent): void {
  if (!dragging.value) return;
  // Dragging upwards increases the value, downwards decreases it.
  const deltaPx = dragStartY.value - event.clientY;
  const t = deltaPx / DRAG_RANGE_PX;
  const next = quantize(dragStartValue.value + t * (props.max - props.min));
  // Compare against the last emitted drag value, not against props: the
  // parent may not have re-rendered yet if several pointer events arrive
  // within the same frame.
  if (next !== (dragValue.value ?? dragStartValue.value)) {
    dragValue.value = next;
    emit('update:modelValue', next);
  }
}

function onPointerUp(): void {
  const next = dragValue.value;
  dragging.value = false;
  dragValue.value = null;
  window.removeEventListener('pointermove', onPointerMove);
  window.removeEventListener('pointerup', onPointerUp);
  window.removeEventListener('pointercancel', onPointerUp);
  if (next !== null && next !== dragStartValue.value) {
    emit('change', next);
  }
}

function onKeydown(event: KeyboardEvent): void {
  if (props.disabled) return;
  const bigStep = Math.max(props.step, (props.max - props.min) / 10);
  let delta = 0;
  switch (event.key) {
    case 'ArrowUp':
    case 'ArrowRight':
      delta = props.step;
      break;
    case 'ArrowDown':
    case 'ArrowLeft':
      delta = -props.step;
      break;
    case 'PageUp':
      delta = bigStep;
      break;
    case 'PageDown':
      delta = -bigStep;
      break;
    case 'Home':
      event.preventDefault();
      commit(props.min);
      return;
    case 'End':
      event.preventDefault();
      commit(props.max);
      return;
    default:
      return;
  }
  event.preventDefault();
  commit(props.modelValue + delta);
}

onBeforeUnmount(() => {
  window.removeEventListener('pointermove', onPointerMove);
  window.removeEventListener('pointerup', onPointerUp);
  window.removeEventListener('pointercancel', onPointerUp);
});
</script>

<template>
  <div class="knob">
    <div
      class="knob-dial"
      :class="{ 'is-dragging': dragging, 'is-disabled': disabled }"
      :style="{ width: size + 'px', height: size + 'px' }"
      role="slider"
      :tabindex="disabled ? -1 : 0"
      :aria-label="label"
      :aria-valuemin="min"
      :aria-valuemax="max"
      :aria-valuenow="displayValue"
      :aria-valuetext="label + ': ' + displayValue"
      @pointerdown="onPointerDown"
      @keydown="onKeydown"
    >
      <div class="knob-indicator" :style="{ transform: 'rotate(' + angle + 'deg)' }">
        <span class="knob-tip"></span>
      </div>
    </div>
    <div class="knob-readout" :style="{ width: size + 'px' }">{{ label }}: {{ displayValue }}</div>
  </div>
</template>

<style scoped>
.knob {
  display: inline-flex;
  flex-direction: column;
  align-items: center;
  gap: 0.5rem;
  font-family: sans-serif;
  user-select: none;
}
.knob-dial {
  position: relative;
  border-radius: 50%;
  background: radial-gradient(circle at 35% 30%, #5a5f6b, #2b2f38 70%);
  border: 2px solid #1a1d24;
  box-shadow: 0 3px 8px rgba(0, 0, 0, 0.35);
  cursor: grab;
  touch-action: none;
}
.knob-dial.is-dragging {
  cursor: grabbing;
  box-shadow: 0 0 0 4px rgba(66, 184, 131, 0.35), 0 3px 8px rgba(0, 0, 0, 0.35);
}
.knob-dial:focus-visible {
  outline: 2px solid #42b883;
  outline-offset: 3px;
}
.knob-dial.is-disabled {
  cursor: not-allowed;
  opacity: 0.45;
}
.knob-indicator {
  position: absolute;
  inset: 0;
  pointer-events: none;
}
.knob-tip {
  position: absolute;
  top: 6%;
  left: 50%;
  width: 4px;
  height: 26%;
  margin-left: -2px;
  border-radius: 2px;
  background: #42b883;
}
.knob-readout {
  font-size: 0.9rem;
  color: #333;
  text-align: center;
  white-space: nowrap;
  font-variant-numeric: tabular-nums;
}
</style>