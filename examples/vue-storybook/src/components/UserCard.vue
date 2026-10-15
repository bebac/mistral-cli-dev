<script setup lang="ts">
defineProps<{
  name: string;
  role?: string;
  online?: boolean;
}>();
</script>

<template>
  <div class="user-card">
    <div class="avatar">{{ name.charAt(0).toUpperCase() }}</div>
    <div class="info">
      <span class="name">{{ name }}</span>
      <span class="role">{{ role ?? 'Member' }}</span>
    </div>
    <span class="status" :class="{ online: online }">
      {{ online ? 'Online' : 'Offline' }}
    </span>
  </div>
</template>

<style scoped>
.user-card {
  display: flex;
  align-items: center;
  gap: 0.75rem;
  padding: 0.75rem 1rem;
  border: 1px solid #e2e2e2;
  border-radius: 8px;
  font-family: sans-serif;
  max-width: 280px;
}
.avatar {
  width: 40px;
  height: 40px;
  border-radius: 50%;
  background: #42b883;
  color: white;
  display: flex;
  align-items: center;
  justify-content: center;
  font-weight: bold;
}
.info {
  display: flex;
  flex-direction: column;
}
.name {
  font-weight: 600;
}
.role {
  font-size: 0.8rem;
  color: #777;
}
.status {
  margin-left: auto;
  font-size: 0.75rem;
  color: #999;
}
.status.online {
  color: #42b883;
}
</style>