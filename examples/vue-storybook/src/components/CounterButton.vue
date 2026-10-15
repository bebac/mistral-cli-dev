<script setup lang="ts">
import { ref } from 'vue';

const props = withDefaults(
  defineProps<{
    label?: string;
    step?: number;
  }>(),
  {
    label: 'Count',
    step: 1,
  }
);

const count = ref(0);

function increment() {
  count.value += props.step;
}
</script>

<template>
  <button class="counter-button" @click="increment">
    {{ label }}: {{ count }}
  </button>
</template>

<style scoped>
.counter-button {
  padding: 0.5rem 1rem;
  font-size: 1rem;
  border-radius: 6px;
  border: 1px solid #42b883;
  background: #42b883;
  color: white;
  cursor: pointer;
}
.counter-button:hover {
  background: #369870;
}
</style>