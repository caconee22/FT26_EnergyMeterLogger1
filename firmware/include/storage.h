#pragma once

#include <stddef.h>
#include <stdint.h>

#include "log_format.h"

namespace ft26::storage {

// microSD SPI 버스를 시작하고 카드를 마운트합니다. 파일은 만들지 않습니다.
bool beginCard();

// SD 카드가 마운트되어 있는지 반환합니다.
bool cardMounted();

// 마운트된 SD 카드의 전체 용량을 바이트로 반환합니다.
uint64_t cardSizeBytes();

// 원본 펌웨어와 같은 규칙으로 로그 파일을 열고 헤더를 씁니다.
bool openLogFile(const log_format::Header& header, char* filename, size_t filename_size);

// 원본 로그 파일과 같은 base 이름으로 오류 txt 파일명을 준비합니다.
void setErrorLogFileName(const log_format::BootTime& boot_time, const uint32_t uid[3]);

// 열린 로그 파일에 측정 레코드 묶음을 씁니다.
bool writeRecords(const log_format::Log* records, size_t count);

// 열린 로그 파일의 캐시를 SD 카드에 반영합니다.
bool syncLogFile();

// 열린 로그 파일을 닫습니다.
void closeLogFile();

// 로그 파일이 열린 상태인지 반환합니다.
bool logFileOpen();

// SD가 마운트된 뒤 발생한 오류를 텍스트 파일에 즉시 추가합니다.
bool appendErrorLog(uint32_t timestamp_ms, const char* source, const char* detail);

}  // namespace ft26::storage
