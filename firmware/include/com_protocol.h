#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ft26 {
namespace com_protocol {

struct LogFileNameInfo {
  size_t session_length;
  uint16_t recovery_index;
};

struct RtcFields {
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;
  int millisecond;
};

bool parseLogFileName(const char* name, LogFileNameInfo& info);
int compareLogFileNames(const char* left, const char* right);
bool parseIndexedCommand(const char* line, const char* command,
                         size_t item_count, size_t& index);
bool parseRtcTimestamp(const char* text, RtcFields& fields);

}  // namespace com_protocol
}  // namespace ft26
