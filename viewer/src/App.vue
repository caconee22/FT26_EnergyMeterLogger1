<script setup>
import NavTabs from "./components/NavTabs.vue";
</script>

<template>
  <div class="app-container">
    <header class="header">
      <div class="header-content">
        <a href="/" class="logo">
          <span class="logo-icon">FT26</span>
          <h1>FOURTOR EnergyMeter Logger</h1>
        </a>
        <NavTabs />
      </div>
    </header>

    <main class="main-content">
      <router-view />
    </main>
  </div>
</template>

<style scoped>
.app-container {
  min-height: 100vh;
  background: var(--bg-primary);
}

.header {
  background: #1a1b21;
  padding: 1rem 2rem;
  border-bottom: 1px solid rgba(255, 255, 255, 0.08);
}

.header-content {
  max-width: 1400px;
  margin: 0 auto;
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 2rem;
}

.logo {
  display: flex;
  align-items: center;
  gap: 0.75rem;
  flex-shrink: 0;
  text-decoration: none;
}

.logo-icon {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  height: 32px;
  min-width: 56px;
  padding: 0 0.5rem;
  border: 1px solid rgba(255, 255, 255, 0.16);
  border-radius: 6px;
  color: #e2e4e9;
  font-size: 0.85rem;
  font-weight: 700;
}

.logo h1 {
  color: #e2e4e9;
  font-size: 1.25rem;
  font-weight: 600;
  margin: 0;
}

.main-content {
  max-width: 1400px;
  margin: 0 auto;
  padding: 1.5rem 2rem;
}

@media (max-width: 640px) {
  .header {
    padding: 0.75rem 1rem;
  }

  .logo h1 {
    font-size: 1.05rem;
  }

  .header-content {
    flex-wrap: wrap;
    gap: 0.75rem;
  }

  .main-content {
    padding: 1rem;
  }
}
</style>
