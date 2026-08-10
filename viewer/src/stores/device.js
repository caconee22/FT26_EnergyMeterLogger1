import { defineStore } from "pinia";
import { computed, ref } from "vue";

const BAUD_RATE = 921600;
const COMMAND_TIMEOUT_MS = 3000;
const DOWNLOAD_TIMEOUT_MS = 10000;
const MAX_SERIAL_EVENTS = 400;

function bytesToText(bytes) {
  return new TextDecoder().decode(bytes);
}

function parseKeyValues(tokens) {
  const values = {};
  for (const token of tokens) {
    const idx = token.indexOf("=");
    if (idx > 0) values[token.slice(0, idx)] = token.slice(idx + 1);
  }
  return values;
}

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function formatHostRtc(date = new Date()) {
  const pad = (value, size = 2) => String(value).padStart(size, "0");
  return [
    date.getFullYear(),
    pad(date.getMonth() + 1),
    pad(date.getDate()),
    pad(date.getHours()),
    pad(date.getMinutes()),
    pad(date.getSeconds()),
    pad(date.getMilliseconds(), 3),
  ].join("-");
}

function formatSerialError(error, fallback = "Serial operation failed") {
  const message = error?.message || fallback;
  if (message === "Serial timeout") return `${fallback}: no response from device`;
  if (message.startsWith("ERR ")) return `${fallback}: ${message}`;
  return message;
}

function safeFilename(name) {
  return (name || "ft26-log.log").replace(/[<>:"/\\|?*\x00-\x1f]/g, "_");
}

function nowText() {
  const date = new Date();
  return date.toLocaleTimeString([], { hour12: false });
}

export const useDeviceStore = defineStore("device", () => {
  const port = ref(null);
  const reader = ref(null);
  const writer = ref(null);
  const reading = ref(false);
  const rxBuffer = ref(new Uint8Array());
  const connected = ref(false);
  const busy = ref(false);
  const activeOperation = ref("");
  const statusLine = ref("");
  const lastError = ref("");
  const uid = ref("");
  const deviceTime = ref("");
  const sdReady = ref(false);
  const rtcReady = ref(false);
  const files = ref([]);
  const downloadedLog = ref(null);
  const downloadProgress = ref({ received: 0, total: 0, name: "" });
  const serialEvents = ref([]);

  const canUseSerial = computed(() => typeof navigator !== "undefined" && "serial" in navigator);
  const downloadPercent = computed(() => {
    if (!downloadProgress.value.total) return 0;
    return Math.round((downloadProgress.value.received / downloadProgress.value.total) * 100);
  });

  function appendRx(bytes) {
    const merged = new Uint8Array(rxBuffer.value.length + bytes.length);
    merged.set(rxBuffer.value);
    merged.set(bytes, rxBuffer.value.length);
    rxBuffer.value = merged;
  }

  function addSerialEvent(direction, text, kind = "line") {
    serialEvents.value.push({
      id: `${Date.now()}-${serialEvents.value.length}`,
      time: nowText(),
      direction,
      kind,
      text,
    });
    if (serialEvents.value.length > MAX_SERIAL_EVENTS) {
      serialEvents.value.splice(0, serialEvents.value.length - MAX_SERIAL_EVENTS);
    }
  }

  function clearSerialEvents() {
    serialEvents.value = [];
  }

  function resetDeviceState() {
    uid.value = "";
    deviceTime.value = "";
    sdReady.value = false;
    rtcReady.value = false;
    files.value = [];
    downloadProgress.value = { received: 0, total: 0, name: "" };
  }

  async function startReadLoop() {
    reading.value = true;
    try {
      while (reading.value && reader.value) {
        const { value, done } = await reader.value.read();
        if (done) break;
        if (value?.length) appendRx(value);
      }
    } catch (error) {
      if (connected.value) lastError.value = formatSerialError(error, "Read failed");
    } finally {
      reading.value = false;
    }
  }

  async function readLine(timeoutMs = COMMAND_TIMEOUT_MS) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const lf = rxBuffer.value.indexOf(10);
      if (lf >= 0) {
        const lineBytes = rxBuffer.value.slice(0, lf);
        rxBuffer.value = rxBuffer.value.slice(lf + 1);
        const line = bytesToText(lineBytes).replace(/\r$/, "");
        addSerialEvent("rx", line || "(blank)");
        return line;
      }
      await delay(10);
    }
    throw new Error("Serial timeout");
  }

  async function readBytes(count, timeoutMs = DOWNLOAD_TIMEOUT_MS) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      if (rxBuffer.value.length >= count) {
        const bytes = rxBuffer.value.slice(0, count);
        rxBuffer.value = rxBuffer.value.slice(count);
        addSerialEvent("rx", `${count.toLocaleString()} bytes`, "bytes");
        return bytes;
      }
      await delay(10);
    }
    throw new Error("Serial timeout");
  }

  async function writeLine(line) {
    if (!writer.value) throw new Error("Serial writer is not ready");
    addSerialEvent("tx", line);
    await writer.value.write(new TextEncoder().encode(`${line}\n`));
  }

  async function withOperation(label, action) {
    busy.value = true;
    activeOperation.value = label;
    lastError.value = "";
    try {
      const result = await action();
      return result;
    } catch (error) {
      lastError.value = formatSerialError(error, label);
      throw error;
    } finally {
      busy.value = false;
      activeOperation.value = "";
    }
  }

  async function drainStartupLines() {
    const deadline = Date.now() + 500;
    while (Date.now() < deadline) {
      try {
        const line = await readLine(120);
        if (line) statusLine.value = line;
      } catch {
        return;
      }
    }
  }

  function parseHello(line) {
    const parts = line.trim().split(/\s+/);
    if (parts[0] !== "OK" || parts[1] !== "HELLO") {
      throw new Error(line || "Unexpected HELLO response");
    }
    const kv = parseKeyValues(parts.slice(4));
    uid.value = parts[2] || "";
    deviceTime.value = parts[3] || "";
    sdReady.value = kv.sd === "1";
    rtcReady.value = kv.rtc === "1";
    statusLine.value = line;
  }

  function parseList(lines) {
    const header = lines[0]?.trim().split(/\s+/) ?? [];
    if (header[0] !== "OK" || header[1] !== "LIST") {
      throw new Error(lines[0] || "Unexpected LIST response");
    }
    files.value = lines.slice(1, -1).map((line) => {
      const match = line.match(/^(\d+)\s+(.+)\s+(\d+)$/);
      if (!match) throw new Error(`Invalid list row: ${line}`);
      return {
        index: Number(match[1]),
        name: match[2],
        size: Number(match[3]),
      };
    });
  }

  async function commandLine(command, timeoutMs = COMMAND_TIMEOUT_MS) {
    await writeLine(command);
    const line = await readLine(timeoutMs);
    if (line.startsWith("ERR ")) throw new Error(line);
    return line;
  }

  async function connect() {
    if (!canUseSerial.value) throw new Error("Web Serial API is not supported in this browser");
    return withOperation("Connect", async () => {
      port.value = await navigator.serial.requestPort();
      await port.value.open({ baudRate: BAUD_RATE, bufferSize: 65536 });
      reader.value = port.value.readable.getReader();
      writer.value = port.value.writable.getWriter();
      port.value.addEventListener("disconnect", disconnect);
      connected.value = true;
      rxBuffer.value = new Uint8Array();
      clearSerialEvents();
      startReadLoop();
      await drainStartupLines();
      await refresh();
      statusLine.value = "Connected";
    }).catch(async (error) => {
      if (connected.value || port.value) await disconnect();
      throw new Error(formatSerialError(error, "Connection failed"));
    });
  }

  async function disconnect() {
    connected.value = false;
    reading.value = false;
    try {
      await reader.value?.cancel();
      reader.value?.releaseLock();
      writer.value?.releaseLock();
      await port.value?.close();
    } catch {
    } finally {
      reader.value = null;
      writer.value = null;
      port.value = null;
      rxBuffer.value = new Uint8Array();
      resetDeviceState();
      statusLine.value = "Disconnected";
    }
  }

  async function refresh() {
    return withOperation("Refresh device", async () => {
      parseHello(await commandLine("HELLO"));
      await listFiles();
    });
  }

  async function listFiles() {
    await writeLine("LIST");
    const lines = [];
    for (;;) {
      const line = await readLine(COMMAND_TIMEOUT_MS);
      lines.push(line);
      if (line === "END") break;
      if (line.startsWith("ERR ")) throw new Error(line);
    }
    parseList(lines);
  }

  async function syncRtc() {
    return withOperation("Sync RTC", async () => {
      const line = await commandLine(`RTC ${formatHostRtc()}`);
      if (line !== "OK RTC") throw new Error(line);
      await refresh();
    });
  }

  async function deleteLog(index) {
    const entry = files.value.find((file) => file.index === index);
    if (!entry) throw new Error("Select a valid file to delete");

    return withOperation(`Delete ${entry.name}`, async () => {
      const line = await commandLine(`DEL ${index}`);
      const parts = line.trim().split(/\s+/);
      if (parts[0] !== "OK" || parts[1] !== "DEL" || Number(parts[2]) !== index || !parts[3]) {
        throw new Error(`Unexpected DEL response: ${line}`);
      }
      await refresh();
      const stillExists = files.value.some((file) => file.name === entry.name);
      if (stillExists) {
        throw new Error(`Delete was acknowledged, but ${entry.name} is still listed on the device`);
      }
      return entry;
    });
  }

  async function downloadFile(index) {
    const entry = files.value.find((file) => file.index === index);
    if (!entry) throw new Error("Selected file is no longer available");

    return withOperation(`Download ${entry.name}`, async () => {
      downloadProgress.value = { received: 0, total: entry.size, name: entry.name };
      const header = await commandLine(`READ ${index}`);
      const parts = header.trim().split(/\s+/);
      if (parts[0] !== "OK" || parts[1] !== "READ") throw new Error(header);
      const total = Number(parts[3]);
      if (!Number.isFinite(total) || total < 0) throw new Error(`Invalid READ size: ${header}`);

      const chunks = [];
      let received = 0;
      while (received < total) {
        const line = await readLine(DOWNLOAD_TIMEOUT_MS);
        if (line.startsWith("ERR ")) throw new Error(line);
        const match = line.match(/^CHUNK\s+(\d+)\s+(\d+)$/);
        if (!match) throw new Error(`Unexpected download response: ${line}`);
        const offset = Number(match[1]);
        const count = Number(match[2]);
        if (offset !== received) throw new Error(`Unexpected chunk offset ${offset}, expected ${received}`);
        const bytes = await readBytes(count);
        chunks.push(bytes);
        received += count;
        downloadProgress.value = { received, total, name: entry.name };
      }

      const done = await readLine(DOWNLOAD_TIMEOUT_MS);
      if (done !== `OK DONE ${received}`) throw new Error(done);

      const data = new Uint8Array(received);
      let pos = 0;
      for (const chunk of chunks) {
        data.set(chunk, pos);
        pos += chunk.length;
      }

      downloadedLog.value = { name: entry.name, data, downloadedAt: Date.now() };
      statusLine.value = `Downloaded ${entry.name}`;
      return downloadedLog.value;
    });
  }

  async function downloadLatest() {
    if (!files.value.length) throw new Error("No log files are available");
    return downloadFile(files.value[0].index);
  }

  function saveDownloadedLog(log = downloadedLog.value) {
    if (!log?.data) throw new Error("No downloaded log is available");
    const base = safeFilename(log.name).replace(/\.[^/.]+$/, "");
    const blob = new Blob([log.data], { type: "application/octet-stream" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = `${base}.log`;
    anchor.click();
    URL.revokeObjectURL(url);
    return anchor.download;
  }

  async function sendManualCommand(command) {
    const trimmed = command.trim();
    if (!trimmed) throw new Error("Command is empty");
    if (!connected.value) throw new Error("Connect a device before sending commands");
    const [head] = trimmed.split(/\s+/);
    const upperHead = head.toUpperCase();
    if (upperHead === "READ") {
      throw new Error("Use the Receive buttons for READ so binary log data stays synchronized.");
    }

    return withOperation(`Send ${trimmed}`, async () => {
      await writeLine(trimmed);
      const lines = [];

      for (;;) {
        const line = await readLine(COMMAND_TIMEOUT_MS);
        lines.push(line);
        if (line.startsWith("ERR ")) throw new Error(line);
        if (upperHead === "LIST") {
          if (line === "END") break;
        } else {
          break;
        }
      }

      const response = lines.join("\n");
      statusLine.value = response;
      return response;
    });
  }

  return {
    baudRate: BAUD_RATE,
    connected,
    busy,
    activeOperation,
    statusLine,
    lastError,
    uid,
    deviceTime,
    sdReady,
    rtcReady,
    files,
    downloadedLog,
    downloadProgress,
    downloadPercent,
    serialEvents,
    canUseSerial,
    connect,
    disconnect,
    refresh,
    syncRtc,
    deleteLog,
    downloadFile,
    downloadLatest,
    saveDownloadedLog,
    sendManualCommand,
    clearSerialEvents,
  };
});
