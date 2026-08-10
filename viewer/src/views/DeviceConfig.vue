<script setup>
import { computed, nextTick, ref, watch } from "vue";
import { useRouter } from "vue-router";
import { useDeviceStore } from "../stores/device";
import { useNotification } from "../composables/useNotification";

const router = useRouter();
const deviceStore = useDeviceStore();
const notyf = useNotification();
const deleteUnlocked = ref(false);
const uiEvent = ref("");
const manualCommand = ref("");
const autoScroll = ref(true);
const serialLogRef = ref(null);
const selectedDeleteIndex = ref(null);

const statusText = computed(() => {
  if (deviceStore.busy) return deviceStore.activeOperation || "Working";
  if (deviceStore.connected) return "Ready";
  return "Disconnected";
});

const selectedDeleteFile = computed(() => {
  if (selectedDeleteIndex.value === null) return null;
  return deviceStore.files.find((file) => file.index === selectedDeleteIndex.value) || null;
});

function formatBytes(bytes) {
  if (!bytes) return "0 B";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1024 / 1024).toFixed(2)} MB`;
}

function formatDownloadedAt(timestamp) {
  if (!timestamp) return "N/A";
  const date = new Date(timestamp);
  return date.toLocaleString();
}

function setUiEvent(message) {
  uiEvent.value = message;
}

function clearUiEvent() {
  uiEvent.value = "";
}

function requireReady(action) {
  if (!deviceStore.canUseSerial) return "Web Serial is not available in this browser.";
  if (!deviceStore.connected) return `Connect a device before ${action}.`;
  if (deviceStore.busy) return `${deviceStore.activeOperation || "Another operation"} is running.`;
  return "";
}

function connectReason() {
  if (!deviceStore.canUseSerial) return "Web Serial is not available in this browser.";
  if (deviceStore.connected) return "A device is already connected.";
  if (deviceStore.busy) return `${deviceStore.activeOperation || "Another operation"} is running.`;
  return "";
}

function receiveLatestReason() {
  const ready = requireReady("receiving logs");
  if (ready) return ready;
  if (!deviceStore.files.length) return "No log files are available on the device.";
  return "";
}

function saveReason() {
  if (!deviceStore.downloadedLog) return "Receive a log before saving it.";
  return "";
}

function deleteReason() {
  const ready = requireReady("deleting source logs");
  if (ready) return ready;
  if (!selectedDeleteFile.value) return "Select one log file to delete.";
  if (!deleteUnlocked.value) return "Unlock delete before deleting source logs.";
  return "";
}

function showReason(reason) {
  if (reason) setUiEvent(reason);
}

function showHint(message, reason = "") {
  setUiEvent(reason || message);
}

function manualReason() {
  const ready = requireReady("sending a command");
  if (ready) return ready;
  if (!manualCommand.value.trim()) return "Type a serial command before sending.";
  return "";
}

async function run(action, successMessage) {
  try {
    const result = await action();
    if (successMessage) notyf.success(typeof successMessage === "function" ? successMessage(result) : successMessage);
    clearUiEvent();
    return result;
  } catch (e) {
    const message = deviceStore.lastError || e.message;
    setUiEvent(message);
    notyf.error(message);
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
  const log = await run(() => deviceStore.downloadLatest(), (result) => `${result.name} received`);
  if (log) router.push("/");
}

async function handleDownload(index) {
  const log = await run(() => deviceStore.downloadFile(index), (result) => `${result.name} received`);
  if (log) router.push("/");
}

function handleSaveDownloaded() {
  try {
    const filename = deviceStore.saveDownloadedLog();
    clearUiEvent();
    notyf.success(`${filename} saved`);
  } catch (e) {
    setUiEvent(e.message);
    notyf.error(e.message);
  }
}

async function handleDeleteLogs() {
  const deleted = await run(() => deviceStore.deleteLog(selectedDeleteIndex.value), (file) => `${file.name} deleted`);
  if (deleted !== null) {
    deleteUnlocked.value = false;
    selectedDeleteIndex.value = null;
  }
}

async function handleManualSend() {
  const command = manualCommand.value.trim();
  const response = await run(() => deviceStore.sendManualCommand(command), "Command completed");
  if (response !== null) manualCommand.value = "";
}

watch(
  () => deviceStore.serialEvents.length,
  async () => {
    if (!autoScroll.value) return;
    await nextTick();
    if (serialLogRef.value) {
      serialLogRef.value.scrollTop = serialLogRef.value.scrollHeight;
    }
  },
);
</script>

<template>
  <div class="device-config">
    <section class="device-summary">
      <div class="summary-main">
        <div class="summary-icon"><i class="fas fa-microchip"></i></div>
        <div>
          <h2>FT26 Device</h2>
          <p>{{ statusText }}</p>
        </div>
      </div>
      <div class="summary-actions">
        <span class="action-wrap" @mouseenter="showHint('Open a serial port and load the device file list.', connectReason())" @focusin="showHint('Open a serial port and load the device file list.', connectReason())">
          <button
            class="btn"
            :class="deviceStore.connected ? 'btn-success' : 'btn-warning'"
            :disabled="!!connectReason()"
            @click="handleConnect"
          >
            <i class="fab fa-usb"></i>{{ deviceStore.connected ? "Connected" : "Connect" }}
          </button>
        </span>
        <span class="action-wrap" @mouseenter="showHint('Close the current serial connection.', requireReady('disconnecting'))" @focusin="showHint('Close the current serial connection.', requireReady('disconnecting'))">
          <button class="btn btn-ghost" :disabled="!!requireReady('disconnecting')" @click="deviceStore.disconnect">
            <i class="fas fa-plug-circle-xmark"></i>Disconnect
          </button>
        </span>
      </div>
    </section>

    <div v-if="uiEvent" class="event-banner">
      <i class="fas fa-bell"></i>
      {{ uiEvent }}
    </div>

    <div v-if="!deviceStore.canUseSerial" class="alert alert-warning">
      Web Serial API requires Chrome or Edge.
    </div>
    <div v-if="deviceStore.lastError" class="alert alert-danger">
      {{ deviceStore.lastError }}
    </div>

    <div class="device-grid">
      <div class="main-column">
        <section class="card device-panel logs-panel" style="animation-delay: 0.1s">
          <div class="card-header">
            <h3><i class="fas fa-folder-open"></i> Logs</h3>
          </div>
          <div class="card-body">
            <div class="log-toolbar">
              <div class="file-info">
                <span class="label">FILES:</span>
                <span class="value">{{ deviceStore.files.length }}</span>
              </div>
              <div class="toolbar-actions">
                <span class="action-wrap" @mouseenter="showHint('Receive the newest log file from the device.', receiveLatestReason())" @focusin="showHint('Receive the newest log file from the device.', receiveLatestReason())">
                  <button class="btn btn-success" :disabled="!!receiveLatestReason()" @click="handleDownloadLatest">
                    <i class="fas fa-download"></i>Receive Latest
                  </button>
                </span>
              </div>
            </div>

            <div v-if="deviceStore.downloadProgress.total" class="progress-row">
              <div class="progress-label">
                {{ deviceStore.downloadProgress.name }} {{ deviceStore.downloadPercent }}%
              </div>
              <div class="progress-track">
                <div class="progress-fill" :style="{ width: `${deviceStore.downloadPercent}%` }"></div>
              </div>
            </div>

            <div class="loaded-file">
              <div class="loaded-copy">
                <span class="section-label">Loaded File</span>
                <template v-if="deviceStore.downloadedLog">
                  <strong>{{ deviceStore.downloadedLog.name }}</strong>
                  <span>{{ formatBytes(deviceStore.downloadedLog.data.length) }} received at {{ formatDownloadedAt(deviceStore.downloadedLog.downloadedAt) }}</span>
                </template>
                <template v-else>
                  <strong>No loaded file</strong>
                  <span>Receive a log from the device to load it here.</span>
                </template>
              </div>
              <span class="action-wrap" @mouseenter="showHint('Save the currently loaded log as a .log file on this PC.', saveReason())" @focusin="showHint('Save the currently loaded log as a .log file on this PC.', saveReason())">
                <button class="btn btn-ghost" :disabled="!!saveReason()" @click="handleSaveDownloaded">
                  <i class="fas fa-floppy-disk"></i>Save .log
                </button>
              </span>
            </div>

            <div class="delete-inline">
              <div class="danger-copy">
                <strong>Delete selected source log on device SD</strong>
                <span v-if="selectedDeleteFile">Selected: {{ selectedDeleteFile.name }}. This sends DEL {{ selectedDeleteFile.index }} to the device.</span>
                <span v-else>Select one log from the list below before deleting source files.</span>
              </div>
              <div class="delete-actions">
                <span class="action-wrap" @mouseenter="showHint('Unlock or lock deletion for the selected SD log file.', requireReady('unlocking delete'))" @focusin="showHint('Unlock or lock deletion for the selected SD log file.', requireReady('unlocking delete'))">
                  <button class="btn btn-ghost" :disabled="!!requireReady('unlocking delete')" @click="deleteUnlocked = !deleteUnlocked">
                    <i :class="deleteUnlocked ? 'fas fa-lock-open' : 'fas fa-lock'"></i>{{ deleteUnlocked ? "Lock" : "Unlock" }}
                  </button>
                </span>
                <span class="action-wrap" @mouseenter="showHint('Delete the selected log file from the device SD card.', deleteReason())" @focusin="showHint('Delete the selected log file from the device SD card.', deleteReason())">
                  <button class="btn btn-danger" :disabled="!!deleteReason()" @click="handleDeleteLogs">
                    <i class="fas fa-trash"></i>Delete Selected
                  </button>
                </span>
              </div>
            </div>

            <div class="file-list">
              <div v-for="file in deviceStore.files" :key="file.index" class="file-row">
                <label class="file-pick">
                  <input v-model="selectedDeleteIndex" type="radio" name="delete-file" :value="file.index" :disabled="deviceStore.busy" />
                  <span>
                    <div class="file-name">{{ file.name }}</div>
                    <div class="file-size">{{ formatBytes(file.size) }}</div>
                  </span>
                </label>
                <div class="file-actions">
                  <span class="action-wrap" @mouseenter="showHint(`Receive ${file.name} from the device.`, requireReady(`receiving ${file.name}`))" @focusin="showHint(`Receive ${file.name} from the device.`, requireReady(`receiving ${file.name}`))">
                    <button class="btn btn-sm btn-ghost" :disabled="!!requireReady(`receiving ${file.name}`)" @click="handleDownload(file.index)">
                      <i class="fas fa-file-arrow-down"></i>Receive
                    </button>
                  </span>
                </div>
              </div>
              <div v-if="deviceStore.connected && !deviceStore.files.length" class="empty-state">No log files found.</div>
            </div>
          </div>
        </section>
      </div>

      <div class="side-column">
        <section class="card device-panel">
          <div class="card-header">
            <h3><i class="fas fa-circle-info"></i> Status</h3>
          </div>
          <div class="card-body">
            <div class="status-grid">
              <div class="status-item">
                <span>Baud</span>
                <strong>{{ deviceStore.connected ? `${deviceStore.baudRate} bps` : "N/A" }}</strong>
              </div>
              <div class="status-item">
                <span>Device ID</span>
                <strong>{{ deviceStore.uid || "UNKNOWN" }}</strong>
              </div>
              <div class="status-item">
                <span>RTC</span>
                <strong>{{ deviceStore.deviceTime || "N/A" }}</strong>
              </div>
              <div class="status-item">
                <span>Media</span>
                <strong>SD {{ deviceStore.sdReady ? "OK" : "N/A" }} / RTC {{ deviceStore.rtcReady ? "OK" : "N/A" }}</strong>
              </div>
            </div>
            <div class="button-group">
              <span class="action-wrap" @mouseenter="showHint('Run HELLO and LIST again to refresh device status and files.', requireReady('refreshing'))" @focusin="showHint('Run HELLO and LIST again to refresh device status and files.', requireReady('refreshing'))">
                <button class="btn btn-primary" :disabled="!!requireReady('refreshing')" @click="handleRefresh">
                  <i class="fas fa-rotate"></i>Refresh
                </button>
              </span>
              <span class="action-wrap" @mouseenter="showHint('Set the device RTC to this PC time.', requireReady('syncing RTC'))" @focusin="showHint('Set the device RTC to this PC time.', requireReady('syncing RTC'))">
                <button class="btn btn-success" :disabled="!!requireReady('syncing RTC')" @click="handleSyncRtc">
                  <i class="fas fa-clock"></i>Sync RTC
                </button>
              </span>
            </div>
          </div>
        </section>

        <section class="card device-panel serial-console" style="animation-delay: 0.2s">
      <div class="card-header">
        <h3><i class="fas fa-terminal"></i> Serial Console</h3>
      </div>
      <div class="card-body">
        <div class="console-input">
          <input
            v-model="manualCommand"
            class="form-input"
            type="text"
            placeholder="HELLO, LIST, RTC yyyy-mm-dd-hh-mm-ss-ms, DEL 1"
            :disabled="!deviceStore.connected || deviceStore.busy"
            @keyup.enter="!manualReason() && handleManualSend()"
          />
          <span class="action-wrap" @mouseenter="showHint('Send the typed serial command and show the response below.', manualReason())" @focusin="showHint('Send the typed serial command and show the response below.', manualReason())">
            <button class="btn btn-primary" :disabled="!!manualReason()" @click="handleManualSend">
              <i class="fas fa-paper-plane"></i>Send
            </button>
          </span>
          <button
            class="btn btn-ghost"
            :disabled="!deviceStore.serialEvents.length"
            @mouseenter="showHint('Clear the serial traffic view. This does not affect the device.')"
            @focusin="showHint('Clear the serial traffic view. This does not affect the device.')"
            @click="deviceStore.clearSerialEvents"
          >
            <i class="fas fa-eraser"></i>Clear
          </button>
        </div>
        <label class="console-option" @mouseenter="showHint('Keep the serial traffic view pinned to the newest line.')" @focusin="showHint('Keep the serial traffic view pinned to the newest line.')">
          <input v-model="autoScroll" type="checkbox" />
          Auto scroll
        </label>

        <div ref="serialLogRef" class="serial-log" aria-label="Serial traffic log">
          <div v-for="event in deviceStore.serialEvents" :key="event.id" class="serial-row" :class="[`serial-${event.direction}`, `serial-${event.kind}`]">
            <span class="serial-time">{{ event.time }}</span>
            <span class="serial-dir">{{ event.direction.toUpperCase() }}</span>
            <code>{{ event.text }}</code>
          </div>
          <div v-if="!deviceStore.serialEvents.length" class="empty-state">No serial traffic yet.</div>
        </div>
      </div>
        </section>
      </div>
    </div>
  </div>
</template>

<style scoped>
.device-config {
  display: flex;
  flex-direction: column;
  gap: 1.5rem;
  max-width: 1360px;
}

.device-grid {
  display: grid;
  grid-template-columns: minmax(0, 1.35fr) minmax(360px, 0.85fr);
  gap: 1.5rem;
  align-items: start;
}

.main-column,
.side-column {
  display: flex;
  flex-direction: column;
  gap: 1.5rem;
  min-width: 0;
}

.device-panel {
  min-width: 0;
}
.device-summary {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1rem;
  padding: 1rem;
  border: 1px solid var(--border-color);
  border-radius: 8px;
  background: var(--bg-card);
  box-shadow: var(--shadow-card);
}

.summary-main,
.summary-actions,
.toolbar-actions,
.button-group {
  display: flex;
  align-items: center;
  gap: 0.75rem;
  flex-wrap: wrap;
}

.action-wrap {
  display: inline-flex;
}

.event-banner {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.75rem 1rem;
  border: 1px solid rgba(59, 130, 246, 0.22);
  border-radius: 6px;
  color: var(--accent-primary);
  background: rgba(59, 130, 246, 0.08);
  font-size: 0.875rem;
}

.summary-icon {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 44px;
  height: 44px;
  border-radius: 8px;
  background: rgba(59, 130, 246, 0.12);
  color: var(--accent-primary);
}

.summary-main h2 {
  margin: 0;
  font-size: 1.05rem;
}

.summary-main p {
  margin: 0.125rem 0 0;
  color: var(--text-secondary);
  font-size: 0.875rem;
}

.button-group {
  margin-top: 1rem;
}

.status-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 0.75rem;
}

.status-item {
  padding: 0.75rem;
  border: 1px solid var(--border-color);
  border-radius: 6px;
  background: var(--bg-secondary);
}

.status-item span {
  display: block;
  color: var(--text-secondary);
  font-size: 0.75rem;
  margin-bottom: 0.25rem;
}

.status-item strong {
  font-family: "JetBrains Mono", monospace;
  font-size: 0.875rem;
  overflow-wrap: anywhere;
}

.log-toolbar {
  display: flex;
  justify-content: space-between;
  gap: 0.75rem;
  flex-wrap: wrap;
  align-items: center;
  margin-bottom: 1rem;
}

.file-info {
  font-size: 0.875rem;
}

.file-info .label,
.file-size,
.loaded-file span,
.danger-copy span {
  color: var(--text-secondary);
}

.file-info .value,
.file-name {
  font-family: "JetBrains Mono", monospace;
  color: var(--text-primary);
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

.loaded-file {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 0.75rem;
  padding: 0.75rem;
  border-radius: 6px;
  border: 1px solid var(--border-color);
  background: var(--bg-secondary);
  margin-bottom: 1rem;
}

.loaded-copy {
  display: flex;
  flex-direction: column;
  gap: 0.125rem;
  min-width: 0;
}

.loaded-copy strong,
.loaded-copy span {
  display: block;
  overflow-wrap: anywhere;
}

.section-label {
  color: var(--text-secondary);
  font-size: 0.75rem;
  font-weight: 600;
  text-transform: uppercase;
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

.file-pick {
  display: inline-flex;
  align-items: center;
  gap: 0.75rem;
  min-width: 0;
  flex: 1;
  cursor: pointer;
}

.file-pick input {
  width: 16px;
  height: 16px;
  flex: 0 0 auto;
}

.file-pick span {
  min-width: 0;
}

.file-actions {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  flex: 0 0 auto;
}

.danger-copy {
  display: flex;
  flex-direction: column;
  gap: 0.25rem;
}

.delete-inline {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1rem;
  padding: 0.75rem;
  margin-bottom: 1rem;
  border: 1px solid rgba(239, 68, 68, 0.25);
  border-radius: 6px;
  background: rgba(239, 68, 68, 0.05);
}

.delete-actions {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  flex: 0 0 auto;
}

.empty-state {
  color: var(--text-secondary);
  font-size: 0.875rem;
  padding: 0.75rem 0;
}

.console-input {
  display: grid;
  grid-template-columns: minmax(0, 1fr);
  gap: 0.75rem;
  align-items: center;
  margin-bottom: 1rem;
}

.console-input .action-wrap,
.console-input .btn {
  width: 100%;
}

.console-option {
  display: inline-flex;
  align-items: center;
  gap: 0.5rem;
  color: var(--text-secondary);
  font-size: 0.875rem;
  margin-bottom: 0.75rem;
  user-select: none;
}

.console-option input {
  width: 16px;
  height: 16px;
}

.serial-log {
  min-height: 180px;
  max-height: 320px;
  overflow: auto;
  border: 1px solid var(--border-color);
  border-radius: 6px;
  background: var(--bg-secondary);
  padding: 0.5rem;
}

.serial-row {
  display: grid;
  grid-template-columns: 74px 42px minmax(0, 1fr);
  gap: 0.5rem;
  align-items: baseline;
  padding: 0.25rem 0.375rem;
  border-radius: 4px;
  font-family: "JetBrains Mono", monospace;
  font-size: 0.78rem;
}

.serial-row:hover {
  background: var(--bg-hover);
}

.serial-time {
  color: var(--text-tertiary);
}

.serial-dir {
  font-weight: 700;
}

.serial-tx .serial-dir {
  color: var(--accent-primary);
}

.serial-rx .serial-dir {
  color: var(--accent-success);
}

.serial-bytes code {
  color: var(--text-secondary);
}

.serial-row code {
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}

@media (max-width: 1080px) {
  .device-grid {
    display: flex;
    flex-direction: column;
    gap: 1.5rem;
  }
}

@media (max-width: 720px) {

  .device-summary,
  .log-toolbar {
    align-items: stretch;
    flex-direction: column;
  }

  .summary-actions,
  .toolbar-actions {
    width: 100%;
  }

  .summary-actions .btn,
  .toolbar-actions .btn {
    flex: 1;
  }

  .loaded-file {
    align-items: stretch;
    flex-direction: column;
  }

  .delete-inline {
    align-items: stretch;
    flex-direction: column;
  }

  .delete-actions .action-wrap,
  .delete-actions .btn {
    width: 100%;
  }

  .file-row {
    align-items: stretch;
    flex-direction: column;
  }

  .file-actions .action-wrap,
  .file-actions .btn {
    width: 100%;
  }

  .status-grid {
    grid-template-columns: 1fr;
  }

  .console-input {
    grid-template-columns: 1fr;
  }
}
</style>
