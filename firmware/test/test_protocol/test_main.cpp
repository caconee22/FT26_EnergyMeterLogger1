#include <unity.h>

#include "com_protocol.h"
#include "log_format.h"

using ft26::com_protocol::LogFileNameInfo;
using ft26::com_protocol::RtcFields;

void setUp() {}

void tearDown() {}

void test_log_file_name_validation_and_recovery_order() {
  LogFileNameInfo info = {};
  TEST_ASSERT_TRUE(ft26::com_protocol::parseLogFileName("session.log", info));
  TEST_ASSERT_EQUAL_UINT16(0, info.recovery_index);
  TEST_ASSERT_TRUE(ft26::com_protocol::parseLogFileName("session.LOG10", info));
  TEST_ASSERT_EQUAL_UINT16(10, info.recovery_index);
  TEST_ASSERT_FALSE(
      ft26::com_protocol::parseLogFileName("session.LOG1.tmp", info));
  TEST_ASSERT_FALSE(ft26::com_protocol::parseLogFileName("session.LOG0", info));
  TEST_ASSERT_FALSE(ft26::com_protocol::parseLogFileName(".log", info));
  TEST_ASSERT_GREATER_THAN(
      0, ft26::com_protocol::compareLogFileNames("session.LOG10",
                                                 "session.LOG9"));
  TEST_ASSERT_GREATER_THAN(
      0, ft26::com_protocol::compareLogFileNames("session.LOG1",
                                                 "session.log"));
}

void test_index_command_requires_a_complete_integer_token() {
  size_t index = 0;
  TEST_ASSERT_TRUE(
      ft26::com_protocol::parseIndexedCommand("DEL 1", "DEL", 3, index));
  TEST_ASSERT_EQUAL_UINT32(1, index);
  TEST_ASSERT_FALSE(
      ft26::com_protocol::parseIndexedCommand("DEL 1junk", "DEL", 3, index));
  TEST_ASSERT_FALSE(
      ft26::com_protocol::parseIndexedCommand("DEL -1", "DEL", 3, index));
  TEST_ASSERT_FALSE(
      ft26::com_protocol::parseIndexedCommand("DEL 3", "DEL", 3, index));
}

void test_rtc_parser_validates_calendar_and_trailing_text() {
  RtcFields fields = {};
  TEST_ASSERT_TRUE(ft26::com_protocol::parseRtcTimestamp(
      "2028-02-29-12-34-56-789", fields));
  TEST_ASSERT_EQUAL_INT(789, fields.millisecond);
  TEST_ASSERT_FALSE(ft26::com_protocol::parseRtcTimestamp(
      "2027-02-29-12-34-56-789", fields));
  TEST_ASSERT_FALSE(ft26::com_protocol::parseRtcTimestamp(
      "2028-02-29-12-34-56-789junk", fields));
  TEST_ASSERT_FALSE(ft26::com_protocol::parseRtcTimestamp(
      "2028-2-29-12-34-56-789", fields));
}

void test_binary_log_checksum_matches_the_viewer_contract() {
  const uint32_t uid[3] = {0x12345678, 0x90ABCDEF, 1};
  const ft26::log_format::BootTime boot_time = {26, 8, 10, 12, 34, 56, 789};
  const auto header =
      ft26::log_format::makeHeader(500, uid, 100, 2500, boot_time);
  const auto record = ft26::log_format::makeRecord(510, 1234, -567, 1245, 2534);
  TEST_ASSERT_EQUAL_UINT8(ft26::log_format::LOG_MAGIC, header.magic);
  TEST_ASSERT_EQUAL_UINT16(0,
                           ft26::log_format::checksum16(&header, sizeof(header)));
  TEST_ASSERT_EQUAL_UINT16(0,
                           ft26::log_format::checksum16(&record, sizeof(record)));
  TEST_ASSERT_EQUAL_UINT32(510, record.timestamp);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_log_file_name_validation_and_recovery_order);
  RUN_TEST(test_index_command_requires_a_complete_integer_token);
  RUN_TEST(test_rtc_parser_validates_calendar_and_trailing_text);
  RUN_TEST(test_binary_log_checksum_matches_the_viewer_contract);
  return UNITY_END();
}
