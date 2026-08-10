#pragma once

#include <stddef.h>
#include <stdint.h>

#include "log_format.h"

namespace ft26::storage {

// microSD SPI 버스를 시작하고 카드를 마운트합니다. 파일은 만들지 않습니다.
bool beginCard();

// Serialize all filesystem access shared by recorder and COM tasks.
bool lockCard(uint32_t timeout_ms);
void unlockCard();

// SD card를 다시 마운트합니다. 기존 파일은 믿지 않고 닫은 뒤 시도합니다.
bool remountCard();

// SD 카드가 마운트되어 있는지 반환합니다.
bool cardMounted();

// 마운트된 SD 카드의 전체 용량을 바이트로 반환합니다.
uint64_t cardSizeBytes();

// 원본 펌웨어와 같은 규칙으로 로그 파일을 열고 헤더를 씁니다.
bool openLogFile(const log_format::Header& header, char* filename, size_t filename_size);

// 기존 파일은 건드리지 않고 LOG1, LOG2 같은 새 복구 파일을 엽니다.
bool openRecoveryLogFile(const log_format::Header& header, char* filename,
                         size_t filename_size);

// 열린 로그 파일에 측정 레코드 묶음을 씁니다.
bool writeRecords(const log_format::Log* records, size_t count);

// 열린 로그 파일의 캐시를 SD 카드에 반영합니다.
bool syncLogFile();

// 열린 로그 파일을 닫습니다.
void closeLogFile();

// 로그 파일이 열린 상태인지 반환합니다.
bool logFileOpen();

}  // namespace ft26::storage
