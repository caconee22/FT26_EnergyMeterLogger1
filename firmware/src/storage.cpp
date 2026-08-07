#include "storage.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <stdio.h>

#include "config.h"

namespace ft26::storage {
namespace {

bool mounted = false;
File log_file;
constexpr uint16_t kMaxRotatedFileIndex = 9999;

// 원본 로그 파일명 규칙에서 확장자를 제외한 공통 이름을 만듭니다.
void formatSessionBaseName(char* filename, size_t filename_size,
                           const log_format::BootTime& boot_time,
                           const uint32_t uid[3]) {
  if (filename == nullptr || filename_size == 0) {
    return;
  }

  snprintf(filename, filename_size,
           "/20%02u-%02u-%02u-%02u-%02u-%02u-%03u %08lX-%08lX-%08lX",
           boot_time.year, boot_time.month, boot_time.day, boot_time.hour,
           boot_time.minute, boot_time.second, boot_time.millisecond,
           static_cast<unsigned long>(uid[0]),
           static_cast<unsigned long>(uid[1]),
           static_cast<unsigned long>(uid[2]));
}

// 원본 로그 파일명 규칙으로 확장자까지 포함한 파일명을 만듭니다.
void formatSessionFileName(char* filename, size_t filename_size,
                           const log_format::BootTime& boot_time,
                           const uint32_t uid[3], const char* extension) {
  char base_name[80] = {};
  formatSessionBaseName(base_name, sizeof(base_name), boot_time, uid);
  snprintf(filename, filename_size, "%s.%s", base_name,
           extension != nullptr ? extension : "log");
}

// 번호가 붙은 보관 파일용 확장자를 대문자로 만듭니다. 예: log -> LOG1
void formatRotatedFileName(char* filename, size_t filename_size, const char* base_name,
                           const char* extension, uint16_t index) {
  char upper_extension[8] = {};
  size_t pos = 0;
  while (extension != nullptr && extension[pos] != '\0' &&
         pos < sizeof(upper_extension) - 1) {
    const char c = extension[pos];
    upper_extension[pos] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    pos++;
  }

  snprintf(filename, filename_size, "%s.%s%u", base_name, upper_extension,
           static_cast<unsigned>(index));
}

// 같은 세션 파일명이 이미 있으면 기존 파일들을 LOG1, LOG2처럼 뒤로 밀어냅니다.
bool rotateExistingSessionFile(const log_format::BootTime& boot_time,
                               const uint32_t uid[3], const char* extension) {
  if (!mounted) {
    return false;
  }

  char base_name[80] = {};
  char current_name[96] = {};
  char next_name[96] = {};
  const char* safe_extension = extension != nullptr ? extension : "log";

  formatSessionBaseName(base_name, sizeof(base_name), boot_time, uid);
  snprintf(current_name, sizeof(current_name), "%s.%s", base_name, safe_extension);

  if (!SD.exists(current_name)) {
    return true;
  }

  uint16_t highest_index = 0;
  for (uint16_t index = 1; index <= kMaxRotatedFileIndex; index++) {
    formatRotatedFileName(next_name, sizeof(next_name), base_name, safe_extension, index);
    if (!SD.exists(next_name)) {
      break;
    }
    highest_index = index;
  }

  if (highest_index >= kMaxRotatedFileIndex) {
    return false;
  }

  for (int index = highest_index; index >= 1; index--) {
    formatRotatedFileName(current_name, sizeof(current_name), base_name, safe_extension,
                          static_cast<uint16_t>(index));
    formatRotatedFileName(next_name, sizeof(next_name), base_name, safe_extension,
                          static_cast<uint16_t>(index + 1));
    if (!SD.rename(current_name, next_name)) {
      return false;
    }
  }

  snprintf(current_name, sizeof(current_name), "%s.%s", base_name, safe_extension);
  formatRotatedFileName(next_name, sizeof(next_name), base_name, safe_extension, 1);
  return SD.rename(current_name, next_name);
}

}  // namespace

// SD SPI 버스를 시작하고 카드를 마운트합니다. 파일은 만들지 않습니다.
bool beginCard() {
  SPI.begin(ft26::PIN_SD_SCK, ft26::PIN_SD_MISO, ft26::PIN_SD_MOSI,
            ft26::PIN_SD_CS);
  mounted = SD.begin(ft26::PIN_SD_CS, SPI);
  return mounted;
}

// SD 카드가 마운트되어 있는지 반환합니다.
bool cardMounted() {
  return mounted;
}

// 마운트된 SD 카드의 전체 용량을 바이트로 반환합니다.
uint64_t cardSizeBytes() {
  if (!mounted) {
    return 0;
  }
  return SD.cardSize();
}

// 원본 파일명 규칙으로 로그 파일을 열고 header를 먼저 씁니다.
bool openLogFile(const log_format::Header& header, char* filename, size_t filename_size) {
  if (!mounted || filename == nullptr || filename_size == 0) {
    return false;
  }

  if (log_file) {
    log_file.close();
  }

  const log_format::BootTime boot_time = {
      header.year, header.month, header.day, header.hour,
      header.minute, header.second, header.millisecond};
  if (!rotateExistingSessionFile(boot_time, header.uid, "log")) {
    return false;
  }
  formatSessionFileName(filename, filename_size, boot_time, header.uid, "log");

  log_file = SD.open(filename, FILE_APPEND);
  if (!log_file) {
    return false;
  }

  return log_file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) ==
         sizeof(header);
}

// 원본 호환 16바이트 record 묶음을 열린 파일에 씁니다.
bool writeRecords(const log_format::Log* records, size_t count) {
  if (!log_file || records == nullptr || count == 0) {
    return false;
  }

  const size_t bytes = sizeof(log_format::Log) * count;
  return log_file.write(reinterpret_cast<const uint8_t*>(records), bytes) == bytes;
}

// 열린 로그 파일의 버퍼를 SD 카드에 반영합니다.
bool syncLogFile() {
  if (!log_file) {
    return false;
  }

  log_file.flush();
  return true;
}

// 열린 로그 파일을 flush하고 닫습니다.
void closeLogFile() {
  if (log_file) {
    log_file.flush();
    log_file.close();
  }
}

// 로그 파일이 현재 열려 있는지 반환합니다.
bool logFileOpen() {
  return static_cast<bool>(log_file);
}

}  // namespace ft26::storage
