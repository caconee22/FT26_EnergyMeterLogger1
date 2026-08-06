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

}  // namespace

// SD SPI 버스를 시작하고 카드를 마운트합니다.
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

// SD 카드 전체 용량을 바이트 단위로 반환합니다.
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

  snprintf(filename, filename_size,
           "/20%02u-%02u-%02u-%02u-%02u-%02u-%03u %08lX-%08lX-%08lX.log",
           header.year, header.month, header.day, header.hour, header.minute,
           header.second, header.millisecond,
           static_cast<unsigned long>(header.uid[0]),
           static_cast<unsigned long>(header.uid[1]),
           static_cast<unsigned long>(header.uid[2]));

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

// 열린 로그 파일을 flush한 뒤 닫습니다.
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
