<script setup>
import { useRouter } from "vue-router";
import { useDeviceStore } from "../stores/device";
import { useNotification } from "../composables/useNotification";

const router = useRouter();
const deviceStore = useDeviceStore();
const notyf = useNotification();

function formatBytes(bytes) {
  if (!bytes) return "0 B";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1024 / 1024).toFixed(2)} MB`;
}

async function run(action, successMessage) {
  try {
    const result = await action();
    if (successMessage) notyf.success(typeof successMessage === "function" ? successMessage(result) : successMessage);
    return result;
  } catch (e) {
    notyf.error(e.message);
    return null;
  }
}

async function handleConnect() {
  await run(() => deviceStore.connect(), "Device connected");
}

async function handleRefresh() {
  await run(() => deviceStore.refresh(), "Device refreshed");
}

async function handleSyncRtc() {
  await run(() => deviceStore.syncRtc(), "RTC synchronized");
}

async function handleDownloadLatest() {
  const log = await run(() => deviceStore.downloadLatest(), (result) => `${result.name} downloaded`);
  if (log) router.push("/");
}

async function handleDownload(index) {
  const log = await run(() => deviceStore.downloadFile(index), (result) => `${result.name} downloaded`);
  if (log) router.push("/");
}
</script>

<template>
  <div class="device-config">
    <section class="card">
      <div class="card-header">
        <h3><i class="fas fa-microchip"></i> Device</h3>
      </div>
      <div class="card-body">
        <div v-if="!deviceStore.canUseSerial" class="alert alert-warning">
          Web Serial API requires Chrome or Edge.
        </div>
        <table class="stats-table">
          <tbody>
            <tr>
              <td>Connection</td>
              <td>{{ deviceStore.connected ? `${deviceStore.baudRate} bps` : "Disconnected" }}</td>
            </tr>
            <tr>
              <td>Device ID</td>
              <td>{{ deviceStore.uid || "UNKNOWN" }}</td>
            </tr>
            <tr>
              <td>RTC</td>
              <td>{{ deviceStore.deviceTime || "N/A" }}</td>
            </tr>
            <tr>
              <td>Status</td>
              <td>SD {{ deviceStore.sdReady ? "OK" : "N/A" }} / RTC {{ deviceStore.rtcReady ? "OK" : "N/A" }}</td>
            </tr>
          </tbody>
        </table>
        <div class="button-group">
          <button
            class="btn"
            :class="deviceStore.connected ? 'btn-success' : 'btn-warning'"
            :disabled="deviceStore.connected || deviceStore.busy || !deviceStore.canUseSerial"
            @click="handleConnect"
          >
            <i class="fab fa-usb"></i>{{ deviceStore.connected ? "Connected" : "Connect" }}
          </button>
          <button class="btn btn-primary" :disabled="!deviceStore.connected || deviceStore.busy" @click="handleRefresh">
            <i class="fas fa-rotate"></i>Refresh
          </button>
          <button class="btn btn-success" :disabled="!deviceStore.connected || deviceStore.busy" @click="handleSyncRtc">
            <i class="fas fa-clock"></i>Sync RTC
          </button>
          <button class="btn btn-ghost" :disabled="!deviceStore.connected || deviceStore.busy" @click="deviceStore.disconnect">
            <i class="fas fa-plug-circle-xmark"></i>Disconnect
          </button>
        </div>
      </div>
    </section>

    <section class="card" style="animation-delay: 0.1s">
      <div class="card-header">
        <h3><i class="fas fa-folder-open"></i> Logs</h3>
      </div>
      <div class="card-body">
        <div class="log-toolbar">
          <div class="file-info">
            <span class="label">FILES:</span>
            <span class="value">{{ deviceStore.files.length }}</span>
          </div>
          <button
            class="btn btn-success"
            :disabled="!deviceStore.connected || deviceStore.busy || !deviceStore.files.length"
            @click="handleDownloadLatest"
          >
            <i class="fas fa-download"></i>Download Latest
          </button>
        </div>

        <div v-if="deviceStore.downloadProgress.total" class="progress-row">
          <div class="progress-label">{{ deviceStore.downloadProgress.name }} {{ deviceStore.downloadPercent }}%</div>
          <div class="progress-track">
            <div class="progress-fill" :style="{ width: `${deviceStore.downloadPercent}%` }"></div>
          </div>
        </div>

        <div class="file-list">
          <div v-for="file in deviceStore.files" :key="file.index" class="file-row">
            <div>
              <div class="file-name">{{ file.name }}</div>
              <div class="file-size">{{ formatBytes(file.size) }}</div>
            </div>
            <button class="btn btn-sm btn-ghost" :disabled="deviceStore.busy" @click="handleDownload(file.index)">
              <i class="fas fa-file-arrow-down"></i>Get
            </button>
          </div>
          <div v-if="deviceStore.connected && !deviceStore.files.length" class="empty-state">No log files found.</div>
        </div>
      </div>
    </section>
  </div>
</template>

<style scoped>
.device-config {
  display: flex;
  flex-direction: column;
  gap: 1.5rem;
  max-width: 860px;
}

.button-group,
.log-toolbar {
  display: flex;
  gap: 0.75rem;
  flex-wrap: wrap;
  align-items: center;
  margin-top: 1rem;
}

.log-toolbar {
  justify-content: space-between;
  margin-top: 0;
  margin-bottom: 1rem;
}

.file-info {
  font-size: 0.875rem;
}

.file-info .label,
.file-size {
  color: var(--text-secondary);
}

.file-info .value,
.file-name {
  font-family: "JetBrains Mono", monospace;
  color: var(--text-primary);
}

.file-list {
  display: flex;
  flex-direction: column;
  gap: 0.5rem;
}

.file-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1rem;
  padding: 0.75rem;
  border: 1px solid var(--border-color);
  border-radius: 6px;
  background: var(--bg-secondary);
}

.progress-row {
  margin-bottom: 1rem;
}

.progress-label {
  font-size: 0.8125rem;
  color: var(--text-secondary);
  margin-bottom: 0.375rem;
}

.progress-track {
  height: 8px;
  overflow: hidden;
  border-radius: 999px;
  background: var(--bg-hover);
}

.progress-fill {
  height: 100%;
  background: var(--accent-success);
  transition: width 0.15s ease;
}

.empty-state {
  color: var(--text-secondary);
  font-size: 0.875rem;
  padding: 0.75rem 0;
}
</style>
