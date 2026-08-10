#include "com_protocol.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace ft26 {
namespace com_protocol {
namespace {

int compareText(const char* left, size_t left_length, const char* right,
                size_t right_length) {
  const size_t common = left_length < right_length ? left_length : right_length;
  const int compared = strncmp(left, right, common);
  if (compared != 0) {
    return compared;
  }
  if (left_length == right_length) {
    return 0;
  }
  return left_length > right_length ? 1 : -1;
}

bool isLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

bool equalsIgnoreCase(const char* left, const char* right, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    const unsigned char left_char = static_cast<unsigned char>(left[i]);
    const unsigned char right_char = static_cast<unsigned char>(right[i]);
    if (tolower(left_char) != tolower(right_char)) {
      return false;
    }
  }
  return true;
}

bool parseDigits(const char* text, size_t offset, size_t count, int& value) {
  value = 0;
  for (size_t i = 0; i < count; ++i) {
    const unsigned char character =
        static_cast<unsigned char>(text[offset + i]);
    if (!isdigit(character)) {
      return false;
    }
    value = value * 10 + static_cast<int>(character - '0');
  }
  return true;
}

}  // namespace

bool parseLogFileName(const char* name, LogFileNameInfo& info) {
  info = {};
  if (name == nullptr || name[0] == '\0') {
    return false;
  }
  const char* dot = strrchr(name, '.');
  if (dot == nullptr || strlen(dot) < 4 || !equalsIgnoreCase(dot, ".log", 4)) {
    return false;
  }
  info.session_length = static_cast<size_t>(dot - name);
  if (info.session_length == 0) {
    return false;
  }
  if (dot[4] == '\0') {
    return true;
  }

  uint32_t recovery_index = 0;
  for (const char* digit = dot + 4; *digit != '\0'; ++digit) {
    if (!isdigit(static_cast<unsigned char>(*digit))) {
      return false;
    }
    recovery_index = recovery_index * 10U + static_cast<uint32_t>(*digit - '0');
    if (recovery_index > UINT16_MAX) {
      return false;
    }
  }
  if (recovery_index == 0) {
    return false;
  }
  info.recovery_index = static_cast<uint16_t>(recovery_index);
  return true;
}

int compareLogFileNames(const char* left, const char* right) {
  LogFileNameInfo left_info = {};
  LogFileNameInfo right_info = {};
  const bool left_valid = parseLogFileName(left, left_info);
  const bool right_valid = parseLogFileName(right, right_info);
  if (!left_valid || !right_valid) {
    return strcmp(left != nullptr ? left : "", right != nullptr ? right : "");
  }

  const int session_compare = compareText(left, left_info.session_length, right,
                                          right_info.session_length);
  if (session_compare != 0) {
    return session_compare;
  }
  if (left_info.recovery_index == right_info.recovery_index) {
    return 0;
  }
  return left_info.recovery_index > right_info.recovery_index ? 1 : -1;
}

bool parseIndexedCommand(const char* line, const char* command,
                         size_t item_count, size_t& index) {
  index = 0;
  if (line == nullptr || command == nullptr) {
    return false;
  }
  const size_t command_length = strlen(command);
  if (strlen(line) <= command_length ||
      !equalsIgnoreCase(line, command, command_length) ||
      line[command_length] != ' ') {
    return false;
  }

  const char* text = line + command_length + 1;
  while (*text == ' ') {
    ++text;
  }
  if (*text == '\0' || *text == '-') {
    return false;
  }
  char* end = nullptr;
  const unsigned long value = strtoul(text, &end, 10);
  if (end == text) {
    return false;
  }
  while (*end == ' ') {
    ++end;
  }
  if (*end != '\0' || value >= item_count) {
    return false;
  }
  index = static_cast<size_t>(value);
  return true;
}

bool parseRtcTimestamp(const char* text, RtcFields& fields) {
  fields = {};
  if (text == nullptr || strlen(text) != 23 || text[4] != '-' ||
      text[7] != '-' || text[10] != '-' || text[13] != '-' ||
      text[16] != '-' || text[19] != '-') {
    return false;
  }
  if (!parseDigits(text, 0, 4, fields.year) ||
      !parseDigits(text, 5, 2, fields.month) ||
      !parseDigits(text, 8, 2, fields.day) ||
      !parseDigits(text, 11, 2, fields.hour) ||
      !parseDigits(text, 14, 2, fields.minute) ||
      !parseDigits(text, 17, 2, fields.second) ||
      !parseDigits(text, 20, 3, fields.millisecond)) {
    return false;
  }
  if (fields.year < 2020 || fields.year > 2099 ||
      fields.month < 1 || fields.month > 12 || fields.hour < 0 ||
      fields.hour > 23 || fields.minute < 0 || fields.minute > 59 ||
      fields.second < 0 || fields.second > 59 || fields.millisecond < 0 ||
      fields.millisecond > 999) {
    return false;
  }

  static constexpr uint8_t days_per_month[] = {31, 28, 31, 30, 31, 30,
                                                31, 31, 30, 31, 30, 31};
  uint8_t max_day = days_per_month[fields.month - 1];
  if (fields.month == 2 && isLeapYear(fields.year)) {
    max_day = 29;
  }
  return fields.day >= 1 && fields.day <= max_day;
}

}  // namespace com_protocol
}  // namespace ft26
