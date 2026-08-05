#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ft26::log_format {

constexpr uint8_t PROTOCOL_VERSION = 0x02;
constexpr uint8_t LOG_MAGIC = 0xAA;

enum LogType : uint8_t {
  LOG_TYPE_HEADER = 0,
  LOG_TYPE_RECORD = 1,
  LOG_TYPE_EVENT = 2,
  LOG_TYPE_CNT = 3,
};

struct __attribute__((packed, aligned(sizeof(uint32_t)))) LogRecord {
  int16_t hv_voltage;   // 0.1 V
  int16_t hv_current;   // 0.1 A
  int16_t lv_voltage;   // 0.01 V
  int16_t temperature;  // 0.01 C
};

struct __attribute__((packed, aligned(sizeof(uint32_t)))) LogEvent {
  uint8_t type;
  uint8_t id;
  uint8_t data[6];
};

union __attribute__((packed, aligned(sizeof(uint32_t)))) LogPacket {
  LogRecord record;
  LogEvent event;
};

struct __attribute__((packed, aligned(sizeof(uint32_t)))) Log {
  uint8_t magic;
  uint8_t type;
  uint16_t checksum;
  uint32_t timestamp;
  LogPacket packet;
};

struct __attribute__((packed, aligned(sizeof(uint32_t)))) Header {
  uint8_t magic;
  uint8_t type;
  uint16_t checksum;
  uint32_t timestamp;
  uint32_t uid[3];
  uint16_t v_cal;
  uint16_t c_cal;
  uint8_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint16_t millisecond;
};

struct BootTime {
  uint8_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint16_t millisecond;
};

static_assert(sizeof(LogRecord) == 8, "LogRecord must match original payload size");
static_assert(sizeof(LogEvent) == 8, "LogEvent must match original payload size");
static_assert(sizeof(Log) == 16, "Log must be exactly 16 bytes");
static_assert(sizeof(Header) == 32, "Header must be exactly 32 bytes");
static_assert(offsetof(Header, uid) == 8, "Header UID offset must match original");
static_assert(offsetof(Header, year) == 24, "Header boot time offset must match original");
static_assert(offsetof(Log, packet) == 8, "Log payload offset must match original");

uint16_t checksum16(const void* data, size_t size);

void setUidFromMac(uint32_t uid[3], const uint8_t mac[6]);

Header makeHeader(uint32_t timestamp,
                  const uint32_t uid[3],
                  uint16_t v_cal,
                  uint16_t c_cal,
                  const BootTime& boot_time);

Log makeRecord(uint32_t timestamp,
               int16_t hv_voltage,
               int16_t hv_current,
               int16_t lv_voltage,
               int16_t temperature);

Log makeEvent(uint32_t timestamp, uint8_t event_type, uint8_t event_id,
              const uint8_t data[6]);

void finalize(Header& header);
void finalize(Log& log);

}  // namespace ft26::log_format
