#include "com_mode.h"

#include <Arduino.h>
#include <RTClib.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "storage.h"

namespace ft26::com_mode {
namespace {

struct ComFileEntry {
  char name[ft26::COM_FILE_NAME_MAX];  // COM 모드에서 전송할 SD 파일명입니다.
  uint32_t size;                       // 파일 크기입니다.
};

ComFileEntry file_list[ft26::COM_FILE_LIST_MAX] = {};
uint8_t file_count = 0;
RTC_DS3231 rtc;
bool rtc_ready = false;
bool sd_ready = false;

struct ComPortState {
  Stream* io;
  char line[96];
  uint8_t len;
};

ComPortState usb_port = {&Serial, {}, 0};
ComPortState uart_port = {&Serial1, {}, 0};

void writeText(Stream& io, const char* text) {
  io.print(text);
}

void writeLinef(Stream& io, const char* format, ...) {
  char buffer[160] = {};
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  io.print(buffer);
}

// 파일명이 로그 파일 확장자를 가지는지 확인합니다.
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

// 파일명 문자열을 기준으로 최신 파일이 먼저 오도록 비교합니다.
bool isNewerFileName(const char* left, const char* right) {
  if (right == nullptr || right[0] == '\0') {
    return true;
  }
  if (left == nullptr) {
    return false;
  }
  return strcmp(left, right) > 0;
}

// 앞쪽 '/'를 제거해 UART 응답에 쓰기 쉬운 파일명으로 만듭니다.
const char* trimRootSlash(const char* name) {
  if (name != nullptr && name[0] == '/') {
    return name + 1;
  }
  return name;
}

// 최신 10개 목록에 파일 하나를 삽입합니다.
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
  strncpy(file_list[pos].name, name, sizeof(file_list[pos].name) - 1);
  file_list[pos].size = size;
}

// SD 루트에서 로그 파일을 스캔하고 최신 10개만 RAM 목록에 저장합니다.
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
      const char* name = trimRootSlash(entry.name());
      insertFileEntry(name, static_cast<uint32_t>(entry.size()));
    }
    entry.close();
  }

  root.close();
}

// COM 명령 비교를 대소문자 무시로 처리합니다.
bool commandStartsWith(const char* line, const char* command) {
  const size_t len = strlen(command);
  return strncasecmp(line, command, len) == 0 &&
         (line[len] == '\0' || line[len] == ' ');
}

// 파일 목록을 ASCII로 전송합니다.
void sendList(Stream& io) {
  writeLinef(io, "OK LIST %u\r\n", file_count);
  for (uint8_t i = 0; i < file_count; ++i) {
    writeLinef(io, "%u %s %lu\r\n", i, file_list[i].name,
               static_cast<unsigned long>(file_list[i].size));
  }
  writeText(io, "END\r\n");
}

// READ 명령의 파일 번호를 파싱합니다.
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

// 선택된 파일을 chunk 단위 binary stream으로 전송합니다.
void sendFile(Stream& io, uint8_t index) {
  char path[ft26::COM_FILE_NAME_MAX + 2] = "/";
  strncat(path, file_list[index].name, sizeof(path) - strlen(path) - 1);

  File file = SD.open(path, FILE_READ);
  if (!file) {
    writeLinef(io, "ERR READ_OPEN %u\r\n", index);
    scanFiles();
    return;
  }

  const uint32_t total_size = static_cast<uint32_t>(file.size());
  uint32_t sent = 0;
  writeLinef(io, "OK READ %u %lu\r\n", index, static_cast<unsigned long>(total_size));

  uint8_t buffer[ft26::COM_READ_CHUNK_SIZE] = {};
  while (sent < total_size) {
    uint32_t request_size = total_size - sent;
    if (request_size > sizeof(buffer)) {
      request_size = sizeof(buffer);
    }

    const int count = file.read(buffer, request_size);
    if (count <= 0) {
      writeLinef(io, "ERR READ_FAULT SD_READ %lu %lu\r\n",
                 static_cast<unsigned long>(sent),
                 static_cast<unsigned long>(total_size));
      file.close();
      scanFiles();
      return;
    }

    writeLinef(io, "CHUNK %lu %d\r\n", static_cast<unsigned long>(sent), count);
    const size_t written = io.write(buffer, static_cast<size_t>(count));
    io.flush();
    if (written != static_cast<size_t>(count)) {
      writeLinef(io, "ERR READ_FAULT SERIAL_WRITE %lu %lu\r\n",
                 static_cast<unsigned long>(sent),
                 static_cast<unsigned long>(total_size));
      file.close();
      return;
    }

    sent += static_cast<uint32_t>(count);
  }

  if (sent != total_size) {
    writeLinef(io, "ERR READ_FAULT SIZE_MISMATCH %lu %lu\r\n",
               static_cast<unsigned long>(sent),
               static_cast<unsigned long>(total_size));
  } else {
    writeLinef(io, "OK DONE %lu\r\n", static_cast<unsigned long>(sent));
  }

  file.close();
}

void deleteFile(Stream& io, uint8_t index) {
  char path[ft26::COM_FILE_NAME_MAX + 2] = "/";
  strncat(path, file_list[index].name, sizeof(path) - strlen(path) - 1);

  char deleted_name[ft26::COM_FILE_NAME_MAX] = {};
  strncpy(deleted_name, file_list[index].name, sizeof(deleted_name) - 1);

  if (!SD.remove(path)) {
    writeLinef(io, "ERR DEL REMOVE %u\r\n", index);
    scanFiles();
    return;
  }

  writeLinef(io, "OK DEL %u %s\r\n", index, deleted_name);
  scanFiles();
}

void sendHello(Stream& io) {
  scanFiles();

  if (!rtc_ready) {
    writeLinef(io, "OK HELLO FT26-UNKNOWN 1970-01-01-00-00-00-000 sd=%u rtc=0 files=%u\r\n",
               sd_ready ? 1 : 0, file_count);
    return;
  }

  const DateTime now = rtc.now();
  writeLinef(io,
             "OK HELLO FT26-COM 20%02u-%02u-%02u-%02u-%02u-%02u-%03u sd=%u rtc=1 files=%u\r\n",
             static_cast<unsigned>(now.year() >= 2000 ? now.year() - 2000 : 0),
             static_cast<unsigned>(now.month()),
             static_cast<unsigned>(now.day()),
             static_cast<unsigned>(now.hour()),
             static_cast<unsigned>(now.minute()),
             static_cast<unsigned>(now.second()),
             static_cast<unsigned>(millis() % 1000),
             sd_ready ? 1 : 0,
             file_count);
}
// 파일명과 같은 날짜 형식에서 RTC 시간을 파싱합니다.
bool parseTime(const char* text, DateTime& out_time) {
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

// TIME 명령으로 RTC를 PC 시간에 맞춥니다.
void setTimeCommand(Stream& io, const char* line) {
  const char* space = strchr(line, ' ');
  if (space == nullptr) {
    writeText(io, "ERR RTC FORMAT\r\n");
    return;
  }

  if (!rtc_ready) {
    writeText(io, "ERR RTC DEVICE\r\n");
    return;
  }

  DateTime time;
  if (!parseTime(space + 1, time)) {
    writeText(io, "ERR RTC FORMAT\r\n");
    return;
  }

  rtc.adjust(time);
  writeText(io, "OK RTC\r\n");
}

// SD 포맷 명령의 현재 응답입니다. 실제 FAT 포맷 구현은 별도 API 확정 후 추가합니다.
// 한 줄 ASCII 명령을 처리합니다.
void handleCommand(Stream& io, char* line) {
  if (commandStartsWith(line, "HELLO")) {
    sendHello(io);
    return;
  }

  if (commandStartsWith(line, "LIST")) {
    scanFiles();
    sendList(io);
    return;
  }

  if (commandStartsWith(line, "READ") || commandStartsWith(line, "REED")) {
    uint8_t index = 0;
    if (!parseFileIndex(line, index)) {
      writeText(io, "ERR READ INDEX\r\n");
      return;
    }
    sendFile(io, index);
    return;
  }

  if (commandStartsWith(line, "DEL")) {
    scanFiles();
    uint8_t index = 0;
    if (!parseFileIndex(line, index)) {
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

void begin() {
  Serial1.begin(ft26::SERIAL_BAUD, SERIAL_8N1, ft26::PIN_UART_RX,
                ft26::PIN_UART_TX);

  Wire.begin(ft26::PIN_I2C_SDA, ft26::PIN_I2C_SCL);
  Wire.setClock(ft26::I2C_CLOCK_HZ);
  Wire.setTimeOut(ft26::I2C_TIMEOUT_MS);
  rtc_ready = rtc.begin(&Wire);

  sd_ready = storage::beginCard();
  scanFiles();
  writeLinef(Serial, "COM READY baud=%lu sd=%u rtc=%u files=%u\r\n",
             static_cast<unsigned long>(ft26::SERIAL_BAUD), sd_ready ? 1 : 0,
             rtc_ready ? 1 : 0, file_count);
  writeLinef(Serial1, "COM READY baud=%lu sd=%u rtc=%u files=%u\r\n",
             static_cast<unsigned long>(ft26::SERIAL_BAUD), sd_ready ? 1 : 0,
             rtc_ready ? 1 : 0, file_count);
}

void processPort(ComPortState& port) {
  while (port.io->available() > 0) {
    const char c = static_cast<char>(port.io->read());
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
      writeText(*port.io, "ERR LINE_TOO_LONG\r\n");
    }
  }
}

void tick() {
  processPort(usb_port);
  processPort(uart_port);
}

}  // namespace ft26::com_mode
