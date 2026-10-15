<script setup lang="ts">
import { ref } from 'vue';

interface Todo {
  id: number;
  text: string;
  done: boolean;
}

const props = defineProps<{
  title?: string;
  initialTodos?: Array<{ text: string; done?: boolean }>;
}>();

const todos = ref<Todo[]>(
  (props.initialTodos ?? []).map((t, i) => ({
    id: i + 1,
    text: t.text,
    done: t.done ?? false,
  }))
);

function toggle(id: number) {
  const todo = todos.value.find((t) => t.id === id);
  if (todo) todo.done = !todo.done;
}
</script>

<template>
  <div class="todo-list">
    <h3>{{ title ?? 'Todos' }}</h3>
    <ul>
      <li
        v-for="todo in todos"
        :key="todo.id"
        :class="{ done: todo.done }"
        @click="toggle(todo.id)"
      >
        {{ todo.text }}
      </li>
    </ul>
    <p v-if="todos.length === 0" class="empty">Nothing to do 🎉</p>
  </div>
</template>

<style scoped>
.todo-list {
  max-width: 320px;
  font-family: sans-serif;
}
.todo-list ul {
  list-style: none;
  padding: 0;
}
.todo-list li {
  padding: 0.4rem 0.5rem;
  border-radius: 4px;
  cursor: pointer;
}
.todo-list li:hover {
  background: #f0f0f0;
}
.todo-list li.done {
  text-decoration: line-through;
  color: #999;
}
.empty {
  color: #999;
}
</style>