#include "recorder.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ads_scheduler.h"
#include "boot.h"
#include "config.h"
#include "log_format.h"
#include "measurements.h"
#include "sensors.h"
#include "status_led.h"
#include "storage.h"

namespace ft26::recorder {
namespace {

Status recorder_status = {};
log_format::Log prelog_records[ft26::PRELOG_RECORD_CAPACITY] = {};
log_format::Log write_batch[ft26::WRITE_BATCH_RECORD_COUNT] = {};
uint16_t write_batch_count = 0;
uint32_t next_record_ms = 0;
uint32_t last_sync_ms = 0;
uint32_t file_start_reference_ms = 0;
int32_t current_zero_uv = measurements::HV_CURRENT_ZERO_UV;
int16_t hv_voltage_zero_deci_v = 0;
char current_filename[96] = {};
uint32_t hv_current_range_since_ms = 0;
uint32_t hv_voltage_range_since_ms = 0;

bool flushBatch();

// 시리얼에 한 줄짜리 상태/오류 로그를 출력합니다.
void logLine(const char* level, const char* message) {
  Serial.printf("[%s] %s\n", level, message);
}

// printf 형식으로 시리얼 상태/오류 로그를 출력합니다.
void logLinef(const char* level, const char* fmt, ...) {
  char message[180] = {};
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  logLine(level, message);
}

// ADS1115 관련 오류를 상태와 LED fault에 반영합니다.
void markAdcError(const char* channel_name) {
  recorder_status.adc_error = true;
  recorder_status.state = State::Fault;
  status_led::setFault(status_led::FaultGroup::Adc);
  logLinef("ERROR", "ADS1115 failed source=%s", channel_name);
}

// SD 파일 작업 오류를 상태와 LED fault에 반영합니다.
void markSdError(const char* action) {
  recorder_status.sd_error = true;
  recorder_status.state = State::Fault;
  status_led::setFault(status_led::FaultGroup::Sd);
  logLinef("ERROR", "SD log %s failed", action);
}

// 측정 범위 오류를 상태와 LED fault에 반영합니다.
void markRangeError(const char* source) {
  if (!recorder_status.range_error) {
    logLinef("ERROR", "Range fault source=%s", source);
  }
  recorder_status.range_error = true;
  status_led::setFault(status_led::FaultGroup::Range);
}

// 입력 전원 차단 시 파일을 마무리하고 LED를 끕니다.
void handlePowerLoss() {
  if (recorder_status.power_lost) {
    return;
  }

  recorder_status.power_lost = true;
  recorder_status.state = State::Fault;
  logLine("ERROR", "Input power lost; closing log file");

  if (recorder_status.file_started) {
    flushBatch();
    storage::syncLogFile();
    storage::closeLogFile();
  }

  status_led::powerFailOff();
}

// 로그 헤더용 calibration 값을 uint16 범위로 제한합니다.
uint16_t clampUint16(int32_t value) {
  if (value <= 0) {
    return 0;
  }
  if (value > 0xFFFF) {
    return 0xFFFF;
  }
  return static_cast<uint16_t>(value);
}

// 일정 시간 이상 유지된 range 상태만 fault로 확정합니다.
void updateHeldRangeFault(bool active, uint32_t hold_ms, uint32_t now,
                          uint32_t& since_ms, const char* source) {
  if (!active) {
    since_ms = 0;
    return;
  }

  if (since_ms == 0) {
    since_ms = now;
    return;
  }

  if (now - since_ms >= hold_ms) {
    markRangeError(source);
  }
}

// 파일 시작 전에는 RAM에 쌓고, 파일 시작 후에는 write batch에 추가합니다.
bool appendRecord(const log_format::Log& record) {
  if (!recorder_status.file_started) {
    if (recorder_status.buffered_records < ft26::PRELOG_RECORD_CAPACITY) {
      prelog_records[recorder_status.buffered_records++] = record;
      return true;
    }

    markSdError("prebuffer full");
    return false;
  }

  write_batch[write_batch_count++] = record;
  if (write_batch_count < ft26::WRITE_BATCH_RECORD_COUNT) {
    return true;
  }

  if (!storage::writeRecords(write_batch, write_batch_count)) {
    markSdError("write batch");
    write_batch_count = 0;
    return false;
  }

  recorder_status.records_written += write_batch_count;
  write_batch_count = 0;
  status_led::notifySdWrite();
  return true;
}

// 쌓여 있는 write batch를 SD 파일에 기록합니다.
bool flushBatch() {
  if (write_batch_count == 0) {
    return true;
  }

  if (!storage::writeRecords(write_batch, write_batch_count)) {
    markSdError("write tail");
    write_batch_count = 0;
    return false;
  }

  recorder_status.records_written += write_batch_count;
  write_batch_count = 0;
  status_led::notifySdWrite();
  return true;
}

// 20초 지연 후 로그 파일을 만들고 header와 prebuffer를 기록합니다.
bool startFileLogging(uint32_t now) {
  if (recorder_status.file_started) {
    return true;
  }

  const boot::HardwareStatus& hw = boot::status();
  log_format::Header header =
      log_format::makeHeader(now, hw.uid,
                             clampUint16(static_cast<int32_t>(hv_voltage_zero_deci_v) * 100),
                             clampUint16(current_zero_uv / 1000),
                             hw.boot_time);

  if (!storage::openLogFile(header, current_filename, sizeof(current_filename))) {
    markSdError("open");
    return false;
  }

  recorder_status.file_started = true;
  recorder_status.state = State::FileLogging;
  logLinef("INFO", "Log file started %s buffered=%u",
           current_filename, recorder_status.buffered_records);

  if (recorder_status.buffered_records > 0) {
    if (!storage::writeRecords(prelog_records, recorder_status.buffered_records)) {
      markSdError("write prebuffer");
      return false;
    }
    recorder_status.records_written += recorder_status.buffered_records;
    status_led::notifySdWrite();
  }

  last_sync_ms = now;
  return storage::syncLogFile();
}

// 설정된 주기마다 batch 기록과 SD flush를 수행합니다.
void syncIfNeeded(uint32_t now) {
  if (!recorder_status.file_started || now - last_sync_ms < ft26::FILE_SYNC_INTERVAL_MS) {
    return;
  }

  flushBatch();
  if (!storage::syncLogFile()) {
    markSdError("sync");
    return;
  }
  last_sync_ms = now;
}

// 최신 측정값의 range 상태를 fault 지속시간 기준으로 검사합니다.
void updateRangeFaults(uint32_t now, const ads_scheduler::LatestReadings& latest) {
  if (latest.hv_current_valid) {
    updateHeldRangeFault(latest.hv_current.below_range ||
                             latest.hv_current.above_range,
                         measurements::HV_CURRENT_RANGE_HOLD_MS, now,
                         hv_current_range_since_ms, "hv-current");
  }

  if (latest.hv_voltage_valid) {
    updateHeldRangeFault(latest.hv_voltage.adc_over_range ||
                             latest.hv_voltage.hv_over_range,
                         measurements::HV_VOLTAGE_RANGE_HOLD_MS, now,
                         hv_voltage_range_since_ms, "hv-voltage");
  }
}

// 최신 측정값으로 원본 호환 100Hz record를 생성합니다.
void captureRecord(uint32_t now) {
  const ads_scheduler::LatestReadings& latest = ads_scheduler::latest();
  updateRangeFaults(now, latest);

  const log_format::Log record =
      log_format::makeRecord(now,
                             latest.hv_voltage_valid
                                 ? static_cast<int16_t>(
                                       latest.hv_voltage.log_deci_v -
                                       hv_voltage_zero_deci_v)
                                 : 0,
                             latest.hv_current_valid
                                 ? latest.hv_current.log_deciamp
                                 : 0,
                             latest.lv_valid ? latest.lv_voltage.log_centi_v : 0,
                             latest.temp_valid ? latest.temperature.log_centi_c : 0);
  ++recorder_status.records_captured;
  appendRecord(record);
}

// ADS 스케줄러 결과를 recorder 오류 상태로 반영합니다.
void handleAdsSchedulerResult(const ads_scheduler::TickResult& result) {
  if (result.adc_error) {
    markAdcError(result.adc_error_source);
  }
  if (result.range_error) {
    markRangeError(result.range_error_source);
  }
}

}  // namespace

// calibration 결과를 적용하고 100Hz 측정/버퍼링 상태로 들어갑니다.
void begin(const calibration::Result& calibration_result) {
  recorder_status = {};
  recorder_status.started_ms = millis();
  recorder_status.calibrated_ms = calibration_result.completed_ms;
  recorder_status.calibrated = calibration_result.ok;
  recorder_status.hv_current_zero_uv = calibration_result.hv_current_zero_uv;
  recorder_status.hv_voltage_zero_deci_v =
      calibration_result.hv_voltage_zero_deci_v;

  next_record_ms = recorder_status.started_ms;
  last_sync_ms = recorder_status.started_ms;
  write_batch_count = 0;
  current_zero_uv = calibration_result.hv_current_zero_uv;
  hv_voltage_zero_deci_v = calibration_result.hv_voltage_zero_deci_v;
  file_start_reference_ms = boot::status().boot_millis;
  hv_current_range_since_ms = 0;
  hv_voltage_range_since_ms = 0;
  memset(current_filename, 0, sizeof(current_filename));
  ads_scheduler::begin(current_zero_uv);

  if (calibration_result.power_lost) {
    recorder_status.power_lost = true;
    recorder_status.state = State::Fault;
    status_led::powerFailOff();
    return;
  }

  if (!calibration_result.ok) {
    recorder_status.state = State::Fault;
    recorder_status.adc_error = calibration_result.adc_error;
    if (calibration_result.adc_error) {
      status_led::setFault(status_led::FaultGroup::Adc);
    }
    logLinef("ERROR", "Recorder start with calibration failure source=%s",
             calibration_result.error_source ? calibration_result.error_source : "unknown");
    return;
  }

  recorder_status.state = State::Buffering;
  status_led::setMode(status_led::Mode::SlowPulse);
  logLine("INFO", "Recorder buffering started");
}

// 전원 감시, ADS 측정, record 생성, SD 기록을 순차 처리합니다.
void tick() {
  const uint32_t now = millis();

  if (recorder_status.state == State::Idle) {
    return;
  }

  const sensors::PowerSenseReading power = sensors::readPowerSense();
  if (!power.present) {
    handlePowerLoss();
    return;
  }

  if (recorder_status.power_lost) {
    return;
  }

  handleAdsSchedulerResult(ads_scheduler::tick(now));

  if (!recorder_status.file_started &&
      now - file_start_reference_ms >= ft26::FILE_LOG_START_DELAY_MS) {
    startFileLogging(now);
  }

  if (static_cast<int32_t>(now - next_record_ms) >= 0) {
    captureRecord(now);
    next_record_ms += ft26::RECORD_INTERVAL_MS;
    if (static_cast<int32_t>(now - next_record_ms) >= 0) {
      next_record_ms = now + ft26::RECORD_INTERVAL_MS;
    }
  }

  syncIfNeeded(now);
}

// recorder의 현재 상태를 반환합니다.
const Status& status() {
  return recorder_status;
}

}  // namespace ft26::recorder
