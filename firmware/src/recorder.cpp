#include "recorder.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "boot.h"
#include "config.h"
#include "log_format.h"
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
uint32_t last_temp_ms = 0;
uint32_t last_sync_ms = 0;
int16_t last_temperature_log = 0;
int32_t current_zero_uv = sensors::HV_CURRENT_ZERO_UV;
char current_filename[96] = {};
uint32_t hv_current_range_since_ms = 0;
uint32_t hv_voltage_range_since_ms = 0;

bool flushBatch();

constexpr uint32_t ADS_CONVERSION_MIN_US = 1200;
constexpr uint32_t ADS_CONVERSION_TIMEOUT_US = 5000;

// ADS1115 비동기 측정 작업 종류입니다.
enum class AdsTask : uint8_t {
  HvVoltage,    // 고전압 전압 채널입니다.
  HvCurrent,    // 고전압 전류 채널입니다.
  LvVoltage,    // 저전압 채널입니다.
  Temperature,  // 온도 채널입니다.
};

// ADS1115 비동기 변환 상태입니다.
struct AdsScheduler {
  bool running;                  // 변환이 진행 중인지 나타냅니다.
  bool hv_voltage_turn;          // HV 전압/전류 번갈이 순서입니다.
  AdsTask task;                  // 현재 변환 중인 작업입니다.
  uint32_t started_us;           // 변환 시작 시각입니다.
  uint32_t last_lv_ms;           // 마지막 LV 갱신 시각입니다.
  uint32_t last_temp_ms;         // 마지막 온도 갱신 시각입니다.
  sensors::HvVoltageReading hv_voltage;       // 마지막 HV 전압 계산값입니다.
  sensors::HvCurrentReading hv_current;       // 마지막 HV 전류 계산값입니다.
  sensors::LvVoltageReading lv_voltage;       // 마지막 LV 전압 계산값입니다.
  sensors::TemperatureReading temperature;     // 마지막 온도 계산값입니다.
  bool hv_voltage_valid;         // HV 전압 최신값 유효 여부입니다.
  bool hv_current_valid;         // HV 전류 최신값 유효 여부입니다.
  bool lv_valid;                 // LV 최신값 유효 여부입니다.
  bool temp_valid;               // 온도 최신값 유효 여부입니다.
};

AdsScheduler ads_scheduler = {};

void logLine(const char* level, const char* message) {
  Serial.printf("[%s] %s\n", level, message);
}

void logLinef(const char* level, const char* fmt, ...) {
  char message[180] = {};
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  logLine(level, message);
}

void markAdcError(const char* channel_name) {
  recorder_status.adc_error = true;
  recorder_status.state = State::Fault;
  status_led::setFault(status_led::FaultGroup::Adc);
  logLinef("ERROR", "ADS1115 read failed channel=%s", channel_name);
}

void markSdError(const char* action) {
  recorder_status.sd_error = true;
  recorder_status.state = State::Fault;
  status_led::setFault(status_led::FaultGroup::Sd);
  logLinef("ERROR", "SD log %s failed", action);
}

void markRangeError(const char* source) {
  if (!recorder_status.range_error) {
    logLinef("ERROR", "Range fault source=%s", source);
  }
  recorder_status.range_error = true;
  status_led::setFault(status_led::FaultGroup::Range);
}

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

uint8_t channelForTask(AdsTask task) {
  switch (task) {
    case AdsTask::HvVoltage:
      return ft26::ADS_CH_HV_VOLTAGE;
    case AdsTask::HvCurrent:
      return ft26::ADS_CH_HV_CURRENT;
    case AdsTask::LvVoltage:
      return ft26::ADS_CH_LV_VOLTAGE;
    case AdsTask::Temperature:
      return ft26::ADS_CH_EXTERNAL_TEMP;
  }
  return ft26::ADS_CH_HV_VOLTAGE;
}

const char* nameForTask(AdsTask task) {
  switch (task) {
    case AdsTask::HvVoltage:
      return "hv-voltage";
    case AdsTask::HvCurrent:
      return "hv-current";
    case AdsTask::LvVoltage:
      return "lv";
    case AdsTask::Temperature:
      return "temperature";
  }
  return "unknown";
}

AdsTask chooseNextAdsTask(uint32_t now) {
  if (ads_scheduler.last_lv_ms == 0 ||
      now - ads_scheduler.last_lv_ms >= ft26::SLOW_CHANNEL_INTERVAL_MS) {
    return AdsTask::LvVoltage;
  }

  if (ads_scheduler.last_temp_ms == 0 ||
      now - ads_scheduler.last_temp_ms >= ft26::SLOW_CHANNEL_INTERVAL_MS) {
    return AdsTask::Temperature;
  }

  ads_scheduler.hv_voltage_turn = !ads_scheduler.hv_voltage_turn;
  return ads_scheduler.hv_voltage_turn ? AdsTask::HvVoltage : AdsTask::HvCurrent;
}

void startNextAdsConversion(uint32_t now) {
  ads_scheduler.task = chooseNextAdsTask(now);
  if (!sensors::startAdsChannel(channelForTask(ads_scheduler.task))) {
    markAdcError(nameForTask(ads_scheduler.task));
    ads_scheduler.running = false;
    return;
  }

  ads_scheduler.started_us = micros();
  ads_scheduler.running = true;
}

void applyAdsResult(AdsTask task, int16_t raw, uint32_t now) {
  switch (task) {
    case AdsTask::HvVoltage:
      ads_scheduler.hv_voltage = sensors::calculateHvVoltage(raw);
      ads_scheduler.hv_voltage_valid = true;
      break;
    case AdsTask::HvCurrent:
      ads_scheduler.hv_current = sensors::calculateHvCurrent(raw, current_zero_uv);
      ads_scheduler.hv_current_valid = true;
      break;
    case AdsTask::LvVoltage:
      ads_scheduler.lv_voltage = sensors::calculateLvVoltage(raw);
      ads_scheduler.lv_valid = true;
      ads_scheduler.last_lv_ms = now;
      break;
    case AdsTask::Temperature:
      ads_scheduler.temperature = sensors::calculateTemperature(raw);
      ads_scheduler.temp_valid = ads_scheduler.temperature.valid;
      ads_scheduler.last_temp_ms = now;
      if (ads_scheduler.temperature.below_range || ads_scheduler.temperature.above_range) {
        markRangeError("temperature");
      }
      break;
  }
}

void tickAdsScheduler(uint32_t now) {
  if (!boot::status().ads_ready) {
    markAdcError("not-ready");
    return;
  }

  if (!ads_scheduler.running) {
    startNextAdsConversion(now);
    return;
  }

  const uint32_t elapsed_us = micros() - ads_scheduler.started_us;
  if (elapsed_us < ADS_CONVERSION_MIN_US) {
    return;
  }

  if (!sensors::adsConversionReady()) {
    if (elapsed_us >= ADS_CONVERSION_TIMEOUT_US) {
      markAdcError(nameForTask(ads_scheduler.task));
      ads_scheduler.running = false;
    }
    return;
  }

  int16_t raw = 0;
  if (!sensors::readAdsLastRaw(raw)) {
    markAdcError(nameForTask(ads_scheduler.task));
    ads_scheduler.running = false;
    return;
  }

  applyAdsResult(ads_scheduler.task, raw, now);
  ads_scheduler.running = false;
  startNextAdsConversion(now);
}

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

bool startFileLogging(uint32_t now) {
  if (recorder_status.file_started) {
    return true;
  }

  const boot::HardwareStatus& hw = boot::status();
  log_format::Header header =
      log_format::makeHeader(now, hw.uid, 0, static_cast<uint16_t>(current_zero_uv / 1000),
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

void captureRecord(uint32_t now) {
  if (ads_scheduler.hv_current_valid) {
    updateHeldRangeFault(ads_scheduler.hv_current.below_range ||
                             ads_scheduler.hv_current.above_range,
                         sensors::HV_CURRENT_RANGE_HOLD_MS, now,
                         hv_current_range_since_ms, "hv-current");
  }

  if (ads_scheduler.hv_voltage_valid) {
    updateHeldRangeFault(ads_scheduler.hv_voltage.adc_over_range ||
                             ads_scheduler.hv_voltage.hv_over_range,
                         sensors::HV_VOLTAGE_RANGE_HOLD_MS, now,
                         hv_voltage_range_since_ms, "hv-voltage");
  }

  const log_format::Log record =
      log_format::makeRecord(now,
                             ads_scheduler.hv_voltage_valid
                                 ? ads_scheduler.hv_voltage.log_deci_v
                                 : 0,
                             ads_scheduler.hv_current_valid
                                 ? ads_scheduler.hv_current.log_deciamp
                                 : 0,
                             ads_scheduler.lv_valid
                                 ? ads_scheduler.lv_voltage.log_centi_v
                                 : 0,
                             ads_scheduler.temp_valid
                                 ? ads_scheduler.temperature.log_centi_c
                                 : 0);
  ++recorder_status.records_captured;
  appendRecord(record);
}

}  // namespace

void begin() {
  recorder_status = {};
  recorder_status.state = State::Buffering;
  recorder_status.started_ms = millis();
  next_record_ms = recorder_status.started_ms;
  last_temp_ms = 0;
  last_sync_ms = recorder_status.started_ms;
  write_batch_count = 0;
  current_zero_uv = sensors::HV_CURRENT_ZERO_UV;
  hv_current_range_since_ms = 0;
  hv_voltage_range_since_ms = 0;
  ads_scheduler = {};
  ads_scheduler.hv_voltage_turn = false;
  memset(current_filename, 0, sizeof(current_filename));
  logLine("INFO", "Recorder buffering started");
}

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

  tickAdsScheduler(now);

  if (!recorder_status.file_started &&
      now - recorder_status.started_ms >= ft26::FILE_LOG_START_DELAY_MS) {
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

const Status& status() {
  return recorder_status;
}

}  // namespace ft26::recorder
