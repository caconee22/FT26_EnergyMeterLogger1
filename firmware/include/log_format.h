#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ft26 {
namespace log_format {

constexpr uint8_t PROTOCOL_VERSION = 0x02;
constexpr uint8_t LOG_MAGIC = 0xAA;

// 원본 STM32 로그와 같은 패킷 타입 값입니다.
enum LogType : uint8_t {
  LOG_TYPE_HEADER = 0,  // 파일 시작 헤더입니다.
  LOG_TYPE_RECORD = 1,  // 100Hz 측정 레코드입니다.
  LOG_TYPE_EVENT = 2,   // 즉시 이벤트 레코드입니다.
  LOG_TYPE_CNT = 3,     // 타입 개수입니다.
};

// 원본 뷰어가 읽는 8바이트 측정 payload입니다.
struct __attribute__((packed, aligned(sizeof(uint32_t)))) LogRecord {
  int16_t hv_voltage;   // 0.1 V
  int16_t hv_current;   // 0.1 A
  int16_t lv_voltage;   // 0.01 V
  int16_t temperature;  // 0.01 C
};

// 원본 포맷에 정의된 8바이트 이벤트 payload입니다.
struct __attribute__((packed, aligned(sizeof(uint32_t)))) LogEvent {
  uint8_t type;     // 이벤트 종류입니다.
  uint8_t id;       // 이벤트 ID입니다.
  uint8_t data[6];  // 이벤트 추가 데이터입니다.
};

// 16바이트 로그 패킷 안에 들어가는 payload 공용 영역입니다.
union __attribute__((packed, aligned(sizeof(uint32_t)))) LogPacket {
  LogRecord record;  // 측정 레코드 payload입니다.
  LogEvent event;    // 이벤트 payload입니다.
};

// 원본과 호환되는 16바이트 로그 패킷입니다.
struct __attribute__((packed, aligned(sizeof(uint32_t)))) Log {
  uint8_t magic;       // 패킷 시작 magic 값입니다.
  uint8_t type;        // LogType 값입니다.
  uint16_t checksum;   // 16비트 XOR checksum입니다.
  uint32_t timestamp;  // 부팅 후 경과 시간 ms입니다.
  LogPacket packet;    // 레코드 또는 이벤트 payload입니다.
};

// 원본과 호환되는 32바이트 파일 헤더입니다.
struct __attribute__((packed, aligned(sizeof(uint32_t)))) Header {
  uint8_t magic;        // 헤더 magic 값입니다.
  uint8_t type;         // LOG_TYPE_HEADER 값입니다.
  uint16_t checksum;    // 16비트 XOR checksum입니다.
  uint32_t timestamp;   // 헤더 작성 시점의 부팅 후 ms입니다.
  uint32_t uid[3];      // 원본 96비트 UID 호환 영역입니다.
  uint16_t v_cal;       // HV 전압 zero calibration 값입니다.
  uint16_t c_cal;       // HV 전류 zero calibration 값입니다.
  uint8_t year;         // 2000년 기준 두 자리 연도입니다.
  uint8_t month;        // 월입니다.
  uint8_t day;          // 일입니다.
  uint8_t hour;         // 시입니다.
  uint8_t minute;       // 분입니다.
  uint8_t second;       // 초입니다.
  uint16_t millisecond; // 밀리초입니다.
};

// 로그 헤더에 저장할 부팅 시각입니다.
struct BootTime {
  uint8_t year;          // 2000년 기준 두 자리 연도입니다.
  uint8_t month;         // 월입니다.
  uint8_t day;           // 일입니다.
  uint8_t hour;          // 시입니다.
  uint8_t minute;        // 분입니다.
  uint8_t second;        // 초입니다.
  uint16_t millisecond;  // 밀리초입니다.
};

static_assert(sizeof(LogRecord) == 8, "LogRecord must match original payload size");
static_assert(sizeof(LogEvent) == 8, "LogEvent must match original payload size");
static_assert(sizeof(Log) == 16, "Log must be exactly 16 bytes");
static_assert(sizeof(Header) == 32, "Header must be exactly 32 bytes");
static_assert(offsetof(Header, uid) == 8, "Header UID offset must match original");
static_assert(offsetof(Header, year) == 24, "Header boot time offset must match original");
static_assert(offsetof(Log, packet) == 8, "Log payload offset must match original");

// 원본과 같은 16비트 단위 XOR checksum을 계산합니다.
uint16_t checksum16(const void* data, size_t size);

// ESP32 MAC 주소를 원본 헤더의 uid[3] 영역에 넣습니다.
void setUidFromMac(uint32_t uid[3], const uint8_t mac[6]);

// 32바이트 파일 헤더를 생성하고 checksum까지 채웁니다.
Header makeHeader(uint32_t timestamp,
                  const uint32_t uid[3],
                  uint16_t v_cal,
                  uint16_t c_cal,
                  const BootTime& boot_time);

// 16바이트 측정 레코드를 생성하고 checksum까지 채웁니다.
Log makeRecord(uint32_t timestamp,
               int16_t hv_voltage,
               int16_t hv_current,
               int16_t lv_voltage,
               int16_t temperature);

// 16바이트 이벤트 레코드를 생성하고 checksum까지 채웁니다.
Log makeEvent(uint32_t timestamp, uint8_t event_type, uint8_t event_id,
              const uint8_t data[6]);

// 헤더 checksum을 다시 계산해 채웁니다.
void finalize(Header& header);

// 로그 패킷 checksum을 다시 계산해 채웁니다.
void finalize(Log& log);

}  // namespace log_format
}  // namespace ft26
