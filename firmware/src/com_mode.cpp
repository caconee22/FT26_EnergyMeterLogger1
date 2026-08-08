#include "com_mode.h"

#include <Arduino.h>
#include <RTClib.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "boot.h"
#include "config.h"
#include "storage.h"

namespace ft26::com_mode {
namespace {

struct ComFileEntry {
  char name[ft26::COM_FILE_NAME_MAX];
  uint32_t size;
};

ComFileEntry file_list[ft26::COM_FILE_LIST_MAX] = {};
uint8_t file_count = 0;
char command_line[96] = {};
uint8_t command_len = 0;
RTC_DS3231 rtc;
bool rtc_ready = false;

bool isLogFileName(const char* name) {
  if (name == nullptr) {
    return false;
  }

  const char* dot = strrchr(name, '.');
  if (dot == nullptr) {
    return false;
  }

  if (strcasecmp(dot, ".log") == 0) {
    return true;
  }

  return strncasecmp(dot, ".LOG", 4) == 0 && isdigit(dot[4]);
}

bool isNewerFileName(const char* left, const char* right) {
  if (right == nullptr || right[0] == '\0') {
    return true;
  }
  if (left == nullptr) {
    return false;
  }
  return strcmp(left, right) > 0;
}

const char* trimRootSlash(const char* name) {
  if (name != nullptr && name[0] == '/') {
    return name + 1;
  }
  return name;
}

void makeRootPath(const char* name, char* path, size_t path_size) {
  if (path == nullptr || path_size == 0) {
    return;
  }

  path[0] = '\0';
  strncat(path, "/", path_size - 1);
  strncat(path, trimRootSlash(name), path_size - strlen(path) - 1);
}

void insertFileEntry(const char* name, uint32_t size) {
  if (name == nullptr || name[0] == '\0') {
    return;
  }

  uint8_t pos = 0;
  while (pos < file_count && !isNewerFileName(name, file_list[pos].name)) {
    ++pos;
  }

  if (pos >= ft26::COM_FILE_LIST_MAX) {
    return;
  }

  if (file_count < ft26::COM_FILE_LIST_MAX) {
    ++file_count;
  }

  for (int i = static_cast<int>(file_count) - 1; i > pos; --i) {
    file_list[i] = file_list[i - 1];
  }

  memset(&file_list[pos], 0, sizeof(file_list[pos]));
  strncpy(file_list[pos].name, trimRootSlash(name),
          sizeof(file_list[pos].name) - 1);
  file_list[pos].size = size;
}

void scanFiles() {
  file_count = 0;
  memset(file_list, 0, sizeof(file_list));

  if (!storage::cardMounted()) {
    return;
  }

  File root = SD.open("/");
  if (!root) {
    return;
  }

  for (;;) {
    File entry = root.openNextFile();
    if (!entry) {
      break;
    }

    if (!entry.isDirectory() && isLogFileName(entry.name())) {
      insertFileEntry(entry.name(), static_cast<uint32_t>(entry.size()));
    }
    entry.close();
  }

  root.close();
}

bool commandStartsWith(const char* line, const char* command) {
  const size_t len = strlen(command);
  return strncasecmp(line, command, len) == 0 &&
         (line[len] == '\0' || line[len] == ' ');
}

void sendHello() {
  scanFiles();

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;

  if (rtc_ready) {
    const DateTime now = rtc.now();
    year = now.year();
    month = now.month();
    day = now.day();
    hour = now.hour();
    minute = now.minute();
    second = now.second();
  }

  const ft26::boot::HardwareStatus& status = ft26::boot::status();
  Serial.printf(
      "OK HELLO %08lX-%08lX-%08lX %04d-%02d-%02d-%02d-%02d-%02d-000 sd=%u rtc=%u files=%u\r\n",
      static_cast<unsigned long>(status.uid[0]),
      static_cast<unsigned long>(status.uid[1]),
      static_cast<unsigned long>(status.uid[2]), year, month, day, hour, minute,
      second, storage::cardMounted(), rtc_ready, file_count);
}

void sendList() {
  Serial.printf("OK LIST %u\r\n", file_count);
  for (uint8_t i = 0; i < file_count; ++i) {
    Serial.printf("%u %s %lu\r\n", i, file_list[i].name,
                  static_cast<unsigned long>(file_list[i].size));
  }
  Serial.print("END\r\n");
}

bool parseFileIndex(const char* line, uint8_t& index) {
  const char* space = strchr(line, ' ');
  if (space == nullptr) {
    return false;
  }

  char* end = nullptr;
  const long value = strtol(space + 1, &end, 10);
  if (end == space + 1 || value < 0 || value >= file_count) {
    return false;
  }

  index = static_cast<uint8_t>(value);
  return true;
}

void sendFile(uint8_t index) {
  char path[ft26::COM_FILE_NAME_MAX + 2] = {};
  makeRootPath(file_list[index].name, path, sizeof(path));

  File file = SD.open(path, FILE_READ);
  if (!file) {
    Serial.printf("ERR READ_OPEN %u\r\n", index);
    scanFiles();
    return;
  }

  const uint32_t total_size = static_cast<uint32_t>(file.size());
  uint32_t sent = 0;
  Serial.printf("OK READ %u %lu\r\n", index,
                static_cast<unsigned long>(total_size));

  uint8_t buffer[ft26::COM_READ_CHUNK_SIZE] = {};
  while (sent < total_size) {
    uint32_t request_size = total_size - sent;
    if (request_size > sizeof(buffer)) {
      request_size = sizeof(buffer);
    }

    const int count = file.read(buffer, request_size);
    if (count <= 0) {
      Serial.printf("ERR READ_FAULT SD_READ %lu %lu\r\n",
                    static_cast<unsigned long>(sent),
                    static_cast<unsigned long>(total_size));
      file.close();
      scanFiles();
      return;
    }

    Serial.printf("CHUNK %lu %d\r\n", static_cast<unsigned long>(sent), count);
    const size_t written = Serial.write(buffer, static_cast<size_t>(count));
    Serial.flush();
    if (written != static_cast<size_t>(count)) {
      Serial.printf("ERR READ_FAULT SERIAL_WRITE %lu %lu\r\n",
                    static_cast<unsigned long>(sent),
                    static_cast<unsigned long>(total_size));
      file.close();
      return;
    }

    sent += static_cast<uint32_t>(count);
  }

  if (sent != total_size) {
    Serial.printf("ERR READ_FAULT SIZE_MISMATCH %lu %lu\r\n",
                  static_cast<unsigned long>(sent),
                  static_cast<unsigned long>(total_size));
  } else {
    Serial.printf("OK DONE %lu\r\n", static_cast<unsigned long>(sent));
  }

  file.close();
}

bool parseRtcTime(const char* text, DateTime& out_time) {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int millisecond = 0;
  const int parsed = sscanf(text, "%d-%d-%d-%d-%d-%d-%d", &year, &month, &day,
                            &hour, &minute, &second, &millisecond);
  if (parsed != 7) {
    return false;
  }

  if (year < 2020 || year > 2099 || month < 1 || month > 12 || day < 1 ||
      day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 59 || millisecond < 0 || millisecond > 999) {
    return false;
  }

  out_time = DateTime(year, month, day, hour, minute, second);
  return true;
}

void setRtcCommand(const char* line) {
  const char* space = strchr(line, ' ');
  if (space == nullptr) {
    Serial.print("ERR RTC FORMAT\r\n");
    return;
  }

  if (!rtc_ready) {
    Serial.print("ERR RTC DEVICE\r\n");
    return;
  }

  DateTime time;
  if (!parseRtcTime(space + 1, time)) {
    Serial.print("ERR RTC FORMAT\r\n");
    return;
  }

  rtc.adjust(time);
  Serial.print("OK RTC\r\n");
}

void deleteLogFiles() {
  if (!storage::cardMounted()) {
    Serial.print("ERR DEL SD\r\n");
    return;
  }

  File root = SD.open("/");
  if (!root) {
    Serial.print("ERR DEL OPEN\r\n");
    return;
  }

  uint16_t deleted_count = 0;
  char failed_name[ft26::COM_FILE_NAME_MAX] = {};

  for (;;) {
    File entry = root.openNextFile();
    if (!entry) {
      break;
    }

    char entry_name[ft26::COM_FILE_NAME_MAX] = {};
    strncpy(entry_name, trimRootSlash(entry.name()), sizeof(entry_name) - 1);
    const bool delete_entry = !entry.isDirectory() && isLogFileName(entry_name);
    entry.close();

    if (!delete_entry) {
      continue;
    }

    char path[ft26::COM_FILE_NAME_MAX + 2] = {};
    makeRootPath(entry_name, path, sizeof(path));
    if (SD.remove(path)) {
      ++deleted_count;
    } else if (failed_name[0] == '\0') {
      strncpy(failed_name, entry_name, sizeof(failed_name) - 1);
    }
  }

  root.close();
  scanFiles();

  if (failed_name[0] != '\0') {
    Serial.printf("ERR DEL REMOVE %s\r\n", failed_name);
    return;
  }

  Serial.printf("OK DEL %u\r\n", deleted_count);
}

void formatCommand() {
  Serial.print("ERR FORMAT_UNSUPPORTED\r\n");
}

void handleCommand(char* line) {
  if (commandStartsWith(line, "HELLO")) {
    sendHello();
    return;
  }

  if (commandStartsWith(line, "LIST")) {
    scanFiles();
    sendList();
    return;
  }

  if (commandStartsWith(line, "READ") || commandStartsWith(line, "REED")) {
    uint8_t index = 0;
    if (!parseFileIndex(line, index)) {
      Serial.print("ERR READ INDEX\r\n");
      return;
    }
    sendFile(index);
    return;
  }

  if (commandStartsWith(line, "RTC")) {
    setRtcCommand(line);
    return;
  }

  if (commandStartsWith(line, "DEL")) {
    deleteLogFiles();
    return;
  }

  if (commandStartsWith(line, "FORMAT")) {
    formatCommand();
    return;
  }

  Serial.print("ERR COMMAND\r\n");
}

}  // namespace

void begin() {
  Wire.begin(ft26::PIN_I2C_SDA, ft26::PIN_I2C_SCL);
  Wire.setClock(ft26::I2C_CLOCK_HZ);
  Wire.setTimeOut(ft26::I2C_TIMEOUT_MS);
  rtc_ready = rtc.begin(&Wire);

  const bool sd_ok = storage::beginCard();
  scanFiles();
  Serial.printf("COM READY baud=%lu sd=%u rtc=%u files=%u\r\n",
                static_cast<unsigned long>(ft26::SERIAL_BAUD), sd_ok, rtc_ready,
                file_count);
}

void tick() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      command_line[command_len] = '\0';
      if (command_len > 0) {
        handleCommand(command_line);
      }
      command_len = 0;
      return;
    }

    if (command_len < sizeof(command_line) - 1) {
      command_line[command_len++] = c;
    } else {
      command_len = 0;
      Serial.print("ERR LINE_TOO_LONG\r\n");
    }
  }
}

}  // namespace ft26::com_mode
