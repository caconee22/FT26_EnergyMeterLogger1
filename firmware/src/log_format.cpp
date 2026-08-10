#include "log_format.h"

#include <string.h>

namespace ft26 {
namespace log_format {

uint16_t checksum16(const void* data, size_t size) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  uint16_t checksum = 0;

  for (size_t i = 0; i + 1 < size; i += 2) {
    const uint16_t word = static_cast<uint16_t>(bytes[i]) |
                          (static_cast<uint16_t>(bytes[i + 1]) << 8);
    checksum ^= word;
  }

  return checksum;
}

void setUidFromMac(uint32_t uid[3], const uint8_t mac[6]) {
  uid[0] = static_cast<uint32_t>(mac[0]) |
           (static_cast<uint32_t>(mac[1]) << 8) |
           (static_cast<uint32_t>(mac[2]) << 16) |
           (static_cast<uint32_t>(mac[3]) << 24);
  uid[1] = static_cast<uint32_t>(mac[4]) |
           (static_cast<uint32_t>(mac[5]) << 8);
  uid[2] = 0;
}

Header makeHeader(uint32_t timestamp,
                  const uint32_t uid[3],
                  uint16_t v_cal,
                  uint16_t c_cal,
                  const BootTime& boot_time) {
  Header header = {};
  header.magic = LOG_MAGIC;
  header.type = LOG_TYPE_HEADER;
  header.timestamp = timestamp;
  header.uid[0] = uid[0];
  header.uid[1] = uid[1];
  header.uid[2] = uid[2];
  header.v_cal = v_cal;
  header.c_cal = c_cal;
  header.year = boot_time.year;
  header.month = boot_time.month;
  header.day = boot_time.day;
  header.hour = boot_time.hour;
  header.minute = boot_time.minute;
  header.second = boot_time.second;
  header.millisecond = boot_time.millisecond;
  finalize(header);
  return header;
}

Log makeRecord(uint32_t timestamp,
               int16_t hv_voltage,
               int16_t hv_current,
               int16_t lv_voltage,
               int16_t temperature) {
  Log log = {};
  log.magic = LOG_MAGIC;
  log.type = LOG_TYPE_RECORD;
  log.timestamp = timestamp;
  log.packet.record.hv_voltage = hv_voltage;
  log.packet.record.hv_current = hv_current;
  log.packet.record.lv_voltage = lv_voltage;
  log.packet.record.temperature = temperature;
  finalize(log);
  return log;
}

Log makeEvent(uint32_t timestamp, uint8_t event_type, uint8_t event_id,
              const uint8_t data[6]) {
  Log log = {};
  log.magic = LOG_MAGIC;
  log.type = LOG_TYPE_EVENT;
  log.timestamp = timestamp;
  log.packet.event.type = event_type;
  log.packet.event.id = event_id;
  if (data != nullptr) {
    memcpy(log.packet.event.data, data, sizeof(log.packet.event.data));
  }
  finalize(log);
  return log;
}

void finalize(Header& header) {
  header.checksum = 0;
  header.checksum = checksum16(&header, sizeof(header));
}

void finalize(Log& log) {
  log.checksum = 0;
  log.checksum = checksum16(&log, sizeof(log));
}

}  // namespace log_format
}  // namespace ft26
