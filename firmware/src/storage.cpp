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

bool beginCard() {
  SPI.begin(ft26::PIN_SD_SCK, ft26::PIN_SD_MISO, ft26::PIN_SD_MOSI,
            ft26::PIN_SD_CS);
  mounted = SD.begin(ft26::PIN_SD_CS, SPI);
  return mounted;
}

bool cardMounted() {
  return mounted;
}

uint64_t cardSizeBytes() {
  if (!mounted) {
    return 0;
  }
  return SD.cardSize();
}

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

bool writeRecords(const log_format::Log* records, size_t count) {
  if (!log_file || records == nullptr || count == 0) {
    return false;
  }

  const size_t bytes = sizeof(log_format::Log) * count;
  return log_file.write(reinterpret_cast<const uint8_t*>(records), bytes) == bytes;
}

bool syncLogFile() {
  if (!log_file) {
    return false;
  }

  log_file.flush();
  return true;
}

void closeLogFile() {
  if (log_file) {
    log_file.flush();
    log_file.close();
  }
}

bool logFileOpen() {
  return static_cast<bool>(log_file);
}

}  // namespace ft26::storage
