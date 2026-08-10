#include "com_mode.h"

#include <Arduino.h>
#include <RTClib.h>
#include <SD.h>
#include <Wire.h>
#include <algorithm>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "boot.h"
#include "com_protocol.h"
#include "config.h"
#include "recorder.h"
#include "storage.h"

namespace ft26::com_mode {
namespace {

struct ComFileEntry {
  String name;
  uint32_t size;
};

struct ComPortState {
  Stream* io;
  char line[96];
  uint8_t len;
  bool discard_until_newline;
};

std::vector<ComFileEntry> file_list;
RTC_DS3231 rtc;
bool rtc_present = false;
bool rtc_valid = false;
bool shared_with_recorder = false;
ComPortState usb_port = {&Serial, {}, 0, false};
ComPortState uart_port = {&Serial1, {}, 0, false};

void writeText(Stream& io, const char* text) {
  io.print(text);
}

void writeLinef(Stream& io, const char* format, ...) {
  char buffer[192] = {};
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  io.print(buffer);
}

bool newerFile(const ComFileEntry& left, const ComFileEntry& right) {
  return com_protocol::compareLogFileNames(left.name.c_str(), right.name.c_str()) >
         0;
}

bool scanFilesLocked() {
  file_list.clear();
  if (!storage::cardMounted()) {
    return false;
  }

  File root = SD.open("/");
  if (!root) {
    return false;
  }

  for (;;) {
    File entry = root.openNextFile();
    if (!entry) {
      break;
    }
    if (!entry.isDirectory()) {
      String name = entry.name();
      if (name.startsWith("/")) {
        name.remove(0, 1);
      }
      com_protocol::LogFileNameInfo info = {};
      if (com_protocol::parseLogFileName(name.c_str(), info)) {
        file_list.push_back({name, static_cast<uint32_t>(entry.size())});
      }
    }
    entry.close();
  }
  root.close();
  std::sort(file_list.begin(), file_list.end(), newerFile);
  return true;
}

bool refreshFiles(uint32_t timeout_ms = 100) {
  if (!storage::lockCard(timeout_ms)) {
    file_list.clear();
    return false;
  }
  bool ok = scanFilesLocked();
  if (!ok && !recorder::active()) {
    ok = storage::remountCard() && scanFilesLocked();
  }
  storage::unlockCard();
  return ok;
}

bool exactCommand(const char* line, const char* command) {
  return strcasecmp(line, command) == 0;
}

bool commandStartsWith(const char* line, const char* command) {
  const size_t len = strlen(command);
  return strncasecmp(line, command, len) == 0 && line[len] == ' ';
}

void sendList(Stream& io) {
  writeLinef(io, "OK LIST %u\r\n", static_cast<unsigned>(file_list.size()));
  for (size_t i = 0; i < file_list.size(); ++i) {
    writeLinef(io, "%u %s %lu\r\n", static_cast<unsigned>(i),
               file_list[i].name.c_str(),
               static_cast<unsigned long>(file_list[i].size));
  }
  writeText(io, "END\r\n");
}

void formatCurrentTime(char* output, size_t output_size) {
  if (!rtc_valid) {
    snprintf(output, output_size, "2000-01-01-00-00-00-000");
    return;
  }

  if (recorder::active()) {
    const log_format::BootTime& boot_time = boot::status().boot_time;
    const uint32_t elapsed_ms = millis() - boot::status().boot_millis;
    const uint32_t total_ms = boot_time.millisecond + elapsed_ms;
    const DateTime base(2000 + boot_time.year, boot_time.month, boot_time.day,
                        boot_time.hour, boot_time.minute, boot_time.second);
    const DateTime now = base + TimeSpan(total_ms / 1000);
    snprintf(output, output_size, "20%02u-%02u-%02u-%02u-%02u-%02u-%03u",
             now.year() - 2000, now.month(), now.day(), now.hour(), now.minute(),
             now.second(), static_cast<unsigned>(total_ms % 1000));
    return;
  }

  const DateTime now = rtc.now();
  snprintf(output, output_size, "20%02u-%02u-%02u-%02u-%02u-%02u-000",
           now.year() >= 2000 ? now.year() - 2000 : 0, now.month(), now.day(),
           now.hour(), now.minute(), now.second());
}

void sendHello(Stream& io) {
  const bool sd_ok = refreshFiles();
  char time_text[32] = {};
  formatCurrentTime(time_text, sizeof(time_text));
  const uint32_t* uid = boot::status().uid;
  char uid_text[32] = {};
  snprintf(uid_text, sizeof(uid_text), "%08lX-%08lX-%08lX",
           static_cast<unsigned long>(uid[0]),
           static_cast<unsigned long>(uid[1]),
           static_cast<unsigned long>(uid[2]));
  writeLinef(io, "OK HELLO %s %s sd=%u rtc=%u files=%u mode=%s\r\n",
             uid_text, time_text, sd_ok ? 1 : 0, rtc_valid ? 1 : 0,
             static_cast<unsigned>(file_list.size()),
             recorder::active() ? "record" : "com");
}

void sendFile(Stream& io, size_t index) {
  if (!storage::lockCard(1000)) {
    writeText(io, "ERR SD BUSY\r\n");
    return;
  }
  if (!scanFilesLocked() || index >= file_list.size()) {
    storage::unlockCard();
    writeText(io, "ERR READ INDEX\r\n");
    return;
  }

  const String path = "/" + file_list[index].name;
  File file = SD.open(path.c_str(), FILE_READ);
  if (!file) {
    storage::unlockCard();
    writeLinef(io, "ERR READ_OPEN %u\r\n", static_cast<unsigned>(index));
    return;
  }

  const uint32_t total_size = static_cast<uint32_t>(file.size());
  uint32_t sent = 0;
  writeLinef(io, "OK READ %u %lu\r\n", static_cast<unsigned>(index),
             static_cast<unsigned long>(total_size));

  uint8_t buffer[ft26::COM_READ_CHUNK_SIZE] = {};
  while (sent < total_size) {
    const size_t request_size =
        std::min<size_t>(sizeof(buffer), total_size - sent);
    const int count = file.read(buffer, request_size);
    if (count <= 0) {
      writeLinef(io, "ERR READ_FAULT SD_READ %lu %lu\r\n",
                 static_cast<unsigned long>(sent),
                 static_cast<unsigned long>(total_size));
      file.close();
      storage::unlockCard();
      return;
    }

    writeLinef(io, "CHUNK %lu %d\r\n", static_cast<unsigned long>(sent), count);
    const size_t written = io.write(buffer, static_cast<size_t>(count));
    if (written != static_cast<size_t>(count)) {
      writeLinef(io, "ERR READ_FAULT SERIAL_WRITE %lu %lu\r\n",
                 static_cast<unsigned long>(sent),
                 static_cast<unsigned long>(total_size));
      file.close();
      storage::unlockCard();
      return;
    }
    sent += static_cast<uint32_t>(count);
  }

  io.flush();
  file.close();
  storage::unlockCard();
  writeLinef(io, "OK DONE %lu\r\n", static_cast<unsigned long>(sent));
}

void deleteFile(Stream& io, size_t index) {
  if (!storage::lockCard(1000)) {
    writeText(io, "ERR SD BUSY\r\n");
    return;
  }
  if (!scanFilesLocked() || index >= file_list.size()) {
    storage::unlockCard();
    writeText(io, "ERR DEL INDEX\r\n");
    return;
  }

  const String deleted_name = file_list[index].name;
  const String path = "/" + deleted_name;
  const bool removed = SD.remove(path.c_str()) && !SD.exists(path.c_str());
  scanFilesLocked();
  storage::unlockCard();
  if (!removed) {
    writeLinef(io, "ERR DEL REMOVE %u\r\n", static_cast<unsigned>(index));
    return;
  }
  writeLinef(io, "OK DEL %u %s\r\n", static_cast<unsigned>(index),
             deleted_name.c_str());
}

void setTimeCommand(Stream& io, const char* line) {
  if (recorder::active()) {
    writeText(io, "ERR BUSY RECORDING\r\n");
    return;
  }
  const char* space = strchr(line, ' ');
  if (space == nullptr || !rtc_present) {
    writeText(io, space == nullptr ? "ERR RTC FORMAT\r\n"
                                   : "ERR RTC DEVICE\r\n");
    return;
  }

  com_protocol::RtcFields fields = {};
  if (!com_protocol::parseRtcTimestamp(space + 1, fields)) {
    writeText(io, "ERR RTC FORMAT\r\n");
    return;
  }
  DateTime time(fields.year, fields.month, fields.day, fields.hour,
                fields.minute, fields.second);
  if (fields.millisecond >= 500) {
    time = time + TimeSpan(1);
  }
  rtc.adjust(time);
  const DateTime verified = rtc.now();
  const uint32_t expected_epoch = time.unixtime();
  const uint32_t verified_epoch = verified.unixtime();
  rtc_valid = verified_epoch >= expected_epoch &&
              verified_epoch - expected_epoch <= 1;
  writeText(io, rtc_valid ? "OK RTC\r\n" : "ERR RTC VERIFY\r\n");
}

void handleCommand(Stream& io, char* line) {
  if (exactCommand(line, "HELLO")) {
    sendHello(io);
    return;
  }
  if (exactCommand(line, "LIST")) {
    if (!refreshFiles()) {
      writeText(io, "ERR SD LIST\r\n");
      return;
    }
    sendList(io);
    return;
  }
  if (commandStartsWith(line, "READ") || commandStartsWith(line, "REED")) {
    if (recorder::active()) {
      writeText(io, "ERR BUSY RECORDING\r\n");
      return;
    }
    if (!refreshFiles()) {
      writeText(io, "ERR SD LIST\r\n");
      return;
    }
    size_t index = 0;
    const char* command = strncasecmp(line, "REED", 4) == 0 ? "REED" : "READ";
    if (!com_protocol::parseIndexedCommand(line, command, file_list.size(),
                                           index)) {
      writeText(io, "ERR READ INDEX\r\n");
      return;
    }
    sendFile(io, index);
    return;
  }
  if (commandStartsWith(line, "DEL")) {
    if (recorder::active()) {
      writeText(io, "ERR BUSY RECORDING\r\n");
      return;
    }
    if (!refreshFiles()) {
      writeText(io, "ERR SD LIST\r\n");
      return;
    }
    size_t index = 0;
    if (!com_protocol::parseIndexedCommand(line, "DEL", file_list.size(),
                                           index)) {
      writeText(io, "ERR DEL INDEX\r\n");
      return;
    }
    deleteFile(io, index);
    return;
  }
  if (commandStartsWith(line, "RTC") || commandStartsWith(line, "TIME")) {
    setTimeCommand(io, line);
    return;
  }
  writeText(io, "ERR COMMAND\r\n");
}

}  // namespace

void begin(bool recording_mode) {
  shared_with_recorder = recording_mode;
  Serial1.begin(ft26::SERIAL_BAUD, SERIAL_8N1, ft26::PIN_UART_RX,
                ft26::PIN_UART_TX);

  if (!shared_with_recorder) {
    Wire.begin(ft26::PIN_I2C_SDA, ft26::PIN_I2C_SCL);
    Wire.setClock(ft26::I2C_CLOCK_HZ);
    Wire.setTimeOut(ft26::I2C_TIMEOUT_MS);
    storage::beginCard();
  }

  rtc_present = rtc.begin(&Wire);
  rtc_valid = rtc_present && !rtc.lostPower();
  const bool sd_ready = refreshFiles();
  writeLinef(Serial, "COM READY baud=%lu sd=%u rtc=%u files=%u mode=%s\r\n",
             static_cast<unsigned long>(ft26::SERIAL_BAUD), sd_ready ? 1 : 0,
             rtc_valid ? 1 : 0, static_cast<unsigned>(file_list.size()),
             recording_mode ? "record" : "com");
  writeLinef(Serial1, "COM READY baud=%lu sd=%u rtc=%u files=%u mode=%s\r\n",
             static_cast<unsigned long>(ft26::SERIAL_BAUD), sd_ready ? 1 : 0,
             rtc_valid ? 1 : 0, static_cast<unsigned>(file_list.size()),
             recording_mode ? "record" : "com");
}

void processPort(ComPortState& port) {
  while (port.io->available() > 0) {
    const char c = static_cast<char>(port.io->read());
    if (port.discard_until_newline) {
      if (c == '\n') {
        port.discard_until_newline = false;
        port.len = 0;
      }
      continue;
    }
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      port.line[port.len] = '\0';
      if (port.len > 0) {
        handleCommand(*port.io, port.line);
      }
      port.len = 0;
      return;
    }
    if (port.len < sizeof(port.line) - 1) {
      port.line[port.len++] = c;
    } else {
      port.len = 0;
      port.discard_until_newline = true;
      writeText(*port.io, "ERR LINE_TOO_LONG\r\n");
    }
  }
}

void tick() {
  processPort(usb_port);
  processPort(uart_port);
}

}  // namespace ft26::com_mode
