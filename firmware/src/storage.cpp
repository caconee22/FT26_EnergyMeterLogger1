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
SemaphoreHandle_t card_mutex = nullptr;
uint64_t expected_file_size = 0;
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
bool findAvailableSessionFileName(const log_format::BootTime& boot_time,
                                  const uint32_t uid[3], const char* extension,
                                  bool recovery_only, char* filename,
                                  size_t filename_size) {
  if (!mounted) {
    return false;
  }

  char base_name[80] = {};
  const char* safe_extension = extension != nullptr ? extension : "log";

  formatSessionBaseName(base_name, sizeof(base_name), boot_time, uid);

  if (!recovery_only) {
    snprintf(filename, filename_size, "%s.%s", base_name, safe_extension);
    if (!SD.exists(filename)) {
      return true;
    }
  }

  for (uint16_t index = 1; index <= kMaxRotatedFileIndex; index++) {
    formatRotatedFileName(filename, filename_size, base_name, safe_extension, index);
    if (!SD.exists(filename)) {
      return true;
    }
  }

  return false;
}

}  // namespace

// SD SPI 버스를 시작하고 카드를 마운트합니다. 파일은 만들지 않습니다.
bool beginCard() {
  if (card_mutex == nullptr) {
    card_mutex = xSemaphoreCreateMutex();
    if (card_mutex == nullptr) {
      mounted = false;
      return false;
    }
  }
  SPI.begin(ft26::PIN_SD_SCK, ft26::PIN_SD_MISO, ft26::PIN_SD_MOSI,
            ft26::PIN_SD_CS);
  mounted = SD.begin(ft26::PIN_SD_CS, SPI);
  return mounted;
}

bool lockCard(uint32_t timeout_ms) {
  if (card_mutex == nullptr) {
    card_mutex = xSemaphoreCreateMutex();
  }
  return card_mutex != nullptr &&
         xSemaphoreTake(card_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void unlockCard() {
  if (card_mutex != nullptr) {
    xSemaphoreGive(card_mutex);
  }
}

// SD card를 다시 마운트합니다. 기존 파일은 믿지 않고 닫은 뒤 시도합니다.
bool remountCard() {
  if (log_file) {
    log_file.close();
  }
  expected_file_size = 0;

  SD.end();
  mounted = false;
  return beginCard();
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
  if (!findAvailableSessionFileName(boot_time, header.uid, "log", false, filename,
                                    filename_size)) {
    return false;
  }

  log_file = SD.open(filename, FILE_WRITE);
  if (!log_file) {
    return false;
  }

  log_file.clearWriteError();
  const bool written =
      log_file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) ==
          sizeof(header) &&
      log_file.getWriteError() == 0;
  if (!written) {
    log_file.close();
    expected_file_size = 0;
    mounted = SD.cardType() != CARD_NONE;
    return false;
  }
  expected_file_size = sizeof(header);
  return true;
}

// 기존 파일은 건드리지 않고 LOG1, LOG2 같은 새 복구 파일을 엽니다.
bool openRecoveryLogFile(const log_format::Header& header, char* filename,
                         size_t filename_size) {
  if (!mounted || filename == nullptr || filename_size == 0) {
    return false;
  }

  if (log_file) {
    log_file.close();
  }

  const log_format::BootTime boot_time = {
      header.year, header.month, header.day, header.hour,
      header.minute, header.second, header.millisecond};
  if (!findAvailableSessionFileName(boot_time, header.uid, "log", true, filename,
                                    filename_size)) {
    return false;
  }

  log_file = SD.open(filename, FILE_WRITE);
  if (!log_file) {
    return false;
  }

  log_file.clearWriteError();
  const bool written =
      log_file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) ==
          sizeof(header) &&
      log_file.getWriteError() == 0;
  if (!written) {
    log_file.close();
    expected_file_size = 0;
    mounted = SD.cardType() != CARD_NONE;
    return false;
  }
  expected_file_size = sizeof(header);
  return true;
}

// 원본 호환 16바이트 record 묶음을 열린 파일에 씁니다.
bool writeRecords(const log_format::Log* records, size_t count) {
  if (!log_file || records == nullptr || count == 0) {
    return false;
  }

  const size_t bytes = sizeof(log_format::Log) * count;
  log_file.clearWriteError();
  const bool written =
      log_file.write(reinterpret_cast<const uint8_t*>(records), bytes) == bytes &&
      log_file.getWriteError() == 0;
  if (written) {
    expected_file_size += bytes;
  }
  return written;
}

// 열린 로그 파일의 버퍼를 SD 카드에 반영합니다.
bool syncLogFile() {
  if (!log_file) {
    return false;
  }

  log_file.flush();
  if (log_file.getWriteError() != 0 || SD.cardType() == CARD_NONE ||
      log_file.size() < expected_file_size) {
    mounted = false;
    return false;
  }
  return true;
}

// 열린 로그 파일을 flush하고 닫습니다.
void closeLogFile() {
  if (log_file) {
    log_file.flush();
    log_file.close();
  }
  expected_file_size = 0;
}

// 로그 파일이 현재 열려 있는지 반환합니다.
bool logFileOpen() {
  return static_cast<bool>(log_file);
}

}  // namespace ft26::storage
