<script setup>
import { useRoute } from "vue-router";

const route = useRoute();

const navItems = [
  { label: "Log Analyzer", path: "/", icon: "fa-chart-line" },
  { label: "Device", path: "/device", icon: "fa-microchip" },
];
</script>

<template>
  <nav class="nav-tabs" aria-label="Main navigation">
    <router-link
      v-for="item in navItems"
      :key="item.path"
      :to="item.path"
      class="nav-tab"
      :class="{ active: route.path === item.path }"
    >
      <i :class="['fas', item.icon]"></i>
      {{ item.label }}
    </router-link>
  </nav>
</template>

<style scoped>
.nav-tabs {
  display: flex;
  gap: 0.5rem;
  flex: 1;
  justify-content: center;
}

.nav-tab {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  min-height: 40px;
  gap: 0.5rem;
  padding: 0.625rem 1.1rem;
  border-radius: 6px;
  color: #b0b4be;
  text-decoration: none;
  font-size: 0.9rem;
  font-weight: 500;
  background: rgba(255, 255, 255, 0.05);
  transition:
    background-color 0.15s ease,
    color 0.15s ease;
}

.nav-tab:hover,
.nav-tab.active {
  background: rgba(255, 255, 255, 0.12);
  color: #e2e4e9;
}

.nav-tab i {
  font-size: 0.875rem;
}

@media (max-width: 640px) {
  .nav-tabs {
    order: 2;
    flex: 0 0 100%;
  }

  .nav-tab {
    flex: 1;
    padding: 0.5rem 0.75rem;
    font-size: 0.8125rem;
  }
}
</style>
