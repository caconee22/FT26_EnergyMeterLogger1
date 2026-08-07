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
#include "record_queue.h"
#include "sensors.h"
#include "status_led.h"
#include "storage.h"

namespace ft26::recorder {
namespace {

Status recorder_status = {};
log_format::Log prelog_records[ft26::PRELOG_RECORD_CAPACITY] = {};
uint32_t next_record_ms = 0;
uint32_t last_sync_ms = 0;
uint32_t file_start_reference_ms = 0;
uint16_t prebuffer_dump_index = 0;
uint32_t power_missing_since_ms = 0;
int32_t current_zero_uv = measurements::HV_CURRENT_ZERO_UV;
int16_t hv_voltage_zero_deci_v = 0;
char current_filename[96] = {};
uint32_t hv_current_range_since_ms = 0;
uint32_t hv_voltage_range_since_ms = 0;
TaskHandle_t measurement_task = nullptr;
TaskHandle_t storage_task = nullptr;
SemaphoreHandle_t sd_mutex = nullptr;
portMUX_TYPE recorder_mux = portMUX_INITIALIZER_UNLOCKED;
bool tasks_started = false;

bool dumpPrebufferChunk(uint32_t now);
bool flushRemainingPrebuffer(bool report_errors);
bool drainStorageQueueChunk(uint32_t now);
bool flushStorageQueue(bool report_errors);
void closeOpenLogForPowerLoss();
bool isFileStarted();

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
void markAdcError() {
  portENTER_CRITICAL(&recorder_mux);
  recorder_status.adc_error = true;
  recorder_status.state = State::Fault;
  portEXIT_CRITICAL(&recorder_mux);
  status_led::setFault(status_led::FaultGroup::Adc);
}

// SD 파일 작업 오류를 상태와 LED fault에 반영합니다.
void markSdError(const char* action) {
  (void)action;
  portENTER_CRITICAL(&recorder_mux);
  recorder_status.sd_error = true;
  recorder_status.state = State::Fault;
  portEXIT_CRITICAL(&recorder_mux);
  status_led::setFault(status_led::FaultGroup::Sd);
}

// 측정 범위 오류를 상태와 LED fault에 반영합니다.
void markRangeError() {
  portENTER_CRITICAL(&recorder_mux);
  recorder_status.range_error = true;
  portEXIT_CRITICAL(&recorder_mux);
  status_led::setFault(status_led::FaultGroup::Range);
}

// SD writer queue 상태를 status에 반영합니다.
void updateStorageQueueStatus() {
  recorder_status.queued_records = record_queue::count();
}

// 100Hz 측정 루프가 SD에 직접 접근하지 않도록 record를 RAM queue에 넣습니다.
bool enqueueStorageRecord(const log_format::Log& record) {
  if (!record_queue::push(record)) {
    markSdError("storage queue full");
    return false;
  }

  updateStorageQueueStatus();
  return true;
}

// 입력 전원 차단 시 파일을 마무리하고 LED를 끕니다.
bool lockSd(uint32_t timeout_ms) {
  if (sd_mutex == nullptr) {
    return true;
  }
  return xSemaphoreTake(sd_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void unlockSd() {
  if (sd_mutex != nullptr) {
    xSemaphoreGive(sd_mutex);
  }
}

void closeOpenLogForPowerLoss() {
  if (!storage::logFileOpen()) {
    return;
  }

  if (isFileStarted()) {
    flushRemainingPrebuffer(false);
    flushStorageQueue(false);
  }
  storage::syncLogFile();
  storage::closeLogFile();
}

bool isPowerLost() {
  portENTER_CRITICAL(&recorder_mux);
  const bool lost = recorder_status.power_lost;
  portEXIT_CRITICAL(&recorder_mux);
  return lost;
}

bool isFileStarted() {
  portENTER_CRITICAL(&recorder_mux);
  const bool started = recorder_status.file_started;
  portEXIT_CRITICAL(&recorder_mux);
  return started;
}

bool isRecorderIdle() {
  portENTER_CRITICAL(&recorder_mux);
  const bool idle = recorder_status.state == State::Idle;
  portEXIT_CRITICAL(&recorder_mux);
  return idle;
}

void latchPowerLost() {
  portENTER_CRITICAL(&recorder_mux);
  recorder_status.power_lost = true;
  recorder_status.state = State::Fault;
  portEXIT_CRITICAL(&recorder_mux);
}

void handlePowerLoss() {
  if (isPowerLost()) {
    return;
  }

  latchPowerLost();
  status_led::powerFailOff();

  bool flushed = true;
  bool synced = true;
  if (!lockSd(ft26::SD_EMERGENCY_MUTEX_WAIT_MS)) {
    portENTER_CRITICAL(&recorder_mux);
    recorder_status.sd_error = true;
    portEXIT_CRITICAL(&recorder_mux);
    return;
  }

  const bool log_open = storage::logFileOpen();
  if (log_open) {
    if (isFileStarted()) {
      flushed = flushRemainingPrebuffer(false);
      flushed = flushStorageQueue(false) && flushed;
    }
    synced = storage::syncLogFile();
    storage::closeLogFile();
  }
  unlockSd();

  if (!flushed || !synced) {
    portENTER_CRITICAL(&recorder_mux);
    recorder_status.sd_error = true;
    portEXIT_CRITICAL(&recorder_mux);
    return;
  }
}

// 로그 헤더용 calibration 값을 uint16 범위로 제한합니다.
bool confirmPowerLoss(uint32_t now) {
  const sensors::PowerSenseReading power = sensors::readPowerSense();
  if (power.present) {
    power_missing_since_ms = 0;
    return false;
  }

  if (power_missing_since_ms == 0) {
    power_missing_since_ms = now;
    return false;
  }

  return now - power_missing_since_ms >= ft26::POWER_LOSS_CONFIRM_MS;
}

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
                          uint32_t& since_ms) {
  if (!active) {
    since_ms = 0;
    return;
  }

  if (since_ms == 0) {
    since_ms = now;
    return;
  }

  if (now - since_ms >= hold_ms) {
    markRangeError();
  }
}

// 파일 시작 전에는 RAM에 쌓고, 파일 시작 후에는 write batch에 추가합니다.
bool appendRecord(const log_format::Log& record) {
  bool use_queue = false;
  bool prebuffer_full = false;

  portENTER_CRITICAL(&recorder_mux);
  if (recorder_status.file_started) {
    use_queue = true;
  } else {
    if (recorder_status.buffered_records < ft26::PRELOG_RECORD_CAPACITY) {
      prelog_records[recorder_status.buffered_records++] = record;
      portEXIT_CRITICAL(&recorder_mux);
      return true;
    }

    prebuffer_full = true;
  }
  portEXIT_CRITICAL(&recorder_mux);

  if (prebuffer_full) {
    markSdError("prebuffer full");
    return false;
  }

  return use_queue ? enqueueStorageRecord(record) : true;
}

// 쌓여 있는 write batch를 SD 파일에 기록합니다.
bool flushBatch() {
  return true;
}

// 20초 지연 후 로그 파일을 만들고 header와 prebuffer를 기록합니다.
bool startFileLogging(uint32_t now) {
  if (isFileStarted()) {
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

  portENTER_CRITICAL(&recorder_mux);
  recorder_status.file_started = true;
  recorder_status.state = State::FileLogging;
  prebuffer_dump_index = 0;
  recorder_status.prebuffer_dumped_records = 0;
  recorder_status.prebuffer_dump_done = recorder_status.buffered_records == 0;
  portEXIT_CRITICAL(&recorder_mux);

  logLinef("INFO", "Log file started %s buffered=%u",
           current_filename, recorder_status.buffered_records);

  last_sync_ms = now;
  return true;
}

// 초기 RAM record를 작은 조각으로 SD에 옮겨 100Hz 측정 루프가 오래 막히지 않게 합니다.
bool dumpPrebufferChunk(uint32_t now) {
  (void)now;
  if (!isFileStarted() || recorder_status.prebuffer_dump_done) {
    return true;
  }

  const uint32_t current_ms = millis();
  if (static_cast<int32_t>(next_record_ms - current_ms) <=
      static_cast<int32_t>(ft26::PRELOG_DUMP_MIN_IDLE_MS)) {
    return true;
  }

  if (prebuffer_dump_index >= recorder_status.buffered_records) {
    recorder_status.prebuffer_dump_done = true;
    status_led::notifySdWrite();
    return storage::syncLogFile();
  }

  const uint16_t remaining = recorder_status.buffered_records - prebuffer_dump_index;
  const size_t count =
      remaining < ft26::PRELOG_DUMP_RECORDS_PER_TICK
          ? remaining
          : ft26::PRELOG_DUMP_RECORDS_PER_TICK;

  if (!storage::writeRecords(&prelog_records[prebuffer_dump_index], count)) {
    markSdError("write prebuffer chunk");
    return false;
  }

  prebuffer_dump_index += static_cast<uint16_t>(count);
  recorder_status.prebuffer_dumped_records = prebuffer_dump_index;
  recorder_status.records_written += count;
  status_led::notifySdWrite();
  return true;
}

// 전원 차단 순간에는 남은 초기 RAM record를 한 번에 써서 로그 보존을 우선합니다.
bool flushRemainingPrebuffer(bool report_errors) {
  if (!isFileStarted() || recorder_status.prebuffer_dump_done) {
    return true;
  }

  if (prebuffer_dump_index >= recorder_status.buffered_records) {
    recorder_status.prebuffer_dump_done = true;
    recorder_status.prebuffer_dumped_records = prebuffer_dump_index;
    return true;
  }

  const uint16_t remaining = recorder_status.buffered_records - prebuffer_dump_index;
  if (!storage::writeRecords(&prelog_records[prebuffer_dump_index], remaining)) {
    if (report_errors) {
      markSdError("flush remaining prebuffer");
    } else {
      portENTER_CRITICAL(&recorder_mux);
      recorder_status.sd_error = true;
      portEXIT_CRITICAL(&recorder_mux);
      status_led::setFault(status_led::FaultGroup::Sd);
    }
    return false;
  }

  prebuffer_dump_index = recorder_status.buffered_records;
  recorder_status.prebuffer_dumped_records = prebuffer_dump_index;
  recorder_status.records_written += remaining;
  recorder_status.prebuffer_dump_done = true;
  status_led::notifySdWrite();
  return true;
}

// SD writer queue를 한 조각씩 비워 100Hz 측정 루프와 SD write를 분리합니다.
bool drainStorageQueueChunk(uint32_t now) {
  (void)now;
  if (!isFileStarted() || !recorder_status.prebuffer_dump_done ||
      record_queue::empty()) {
    return true;
  }

  const uint32_t current_ms = millis();
  if (static_cast<int32_t>(next_record_ms - current_ms) <=
      static_cast<int32_t>(ft26::PRELOG_DUMP_MIN_IDLE_MS)) {
    return true;
  }

  size_t count = 0;
  const log_format::Log* records =
      record_queue::readBlock(ft26::STORAGE_WRITE_RECORDS_PER_TICK, count);

  if (records == nullptr || count == 0) {
    return true;
  }

  if (!storage::writeRecords(records, count)) {
    markSdError("write queue chunk");
    return false;
  }

  record_queue::pop(count);
  updateStorageQueueStatus();
  recorder_status.records_written += count;
  status_led::notifySdWrite();
  return true;
}

// 전원 차단 순간에는 남은 SD writer queue를 한 번에 써서 로그 보존을 우선합니다.
bool flushStorageQueue(bool report_errors) {
  while (!record_queue::empty()) {
    size_t count = 0;
    const log_format::Log* records =
        record_queue::readBlock(ft26::STORAGE_QUEUE_RECORD_CAPACITY, count);

    if (records == nullptr || count == 0) {
      return false;
    }

    if (!storage::writeRecords(records, count)) {
      if (report_errors) {
        markSdError("flush storage queue");
      } else {
        portENTER_CRITICAL(&recorder_mux);
        recorder_status.sd_error = true;
        portEXIT_CRITICAL(&recorder_mux);
        status_led::setFault(status_led::FaultGroup::Sd);
      }
      return false;
    }

    record_queue::pop(count);
    recorder_status.records_written += count;
    status_led::notifySdWrite();
  }

  updateStorageQueueStatus();
  return true;
}

// 설정된 주기마다 batch 기록과 SD flush를 수행합니다.
void syncIfNeeded(uint32_t now) {
  if (!isFileStarted() || now - last_sync_ms < ft26::FILE_SYNC_INTERVAL_MS) {
    return;
  }

  if (!recorder_status.prebuffer_dump_done) {
    return;
  }

  if (!record_queue::empty()) {
    return;
  }

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
                         hv_current_range_since_ms);
  }

  if (latest.hv_voltage_valid) {
    updateHeldRangeFault(latest.hv_voltage.adc_over_range ||
                             latest.hv_voltage.hv_over_range,
                         measurements::HV_VOLTAGE_RANGE_HOLD_MS, now,
                         hv_voltage_range_since_ms);
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
    markAdcError();
  }
  if (result.range_error) {
    markRangeError();
  }
}

}  // namespace

// calibration 결과를 적용하고 100Hz 측정/버퍼링 상태로 들어갑니다.
void runMeasurementStep(uint32_t now);
void runStorageStep(uint32_t now);
void measurementTaskMain(void* parameter);
void storageTaskMain(void* parameter);

bool startRecorderTasks() {
  if (tasks_started) {
    return true;
  }

  if (sd_mutex == nullptr) {
    sd_mutex = xSemaphoreCreateMutex();
  }

  if (sd_mutex == nullptr) {
    logLine("ERROR", "FreeRTOS SD mutex create failed");
    recorder_status.state = State::Fault;
    recorder_status.sd_error = true;
    return false;
  }

  BaseType_t ok = xTaskCreate(measurementTaskMain, "ft26_measure", 4096, nullptr,
                              3, &measurement_task);
  if (ok != pdPASS) {
    logLine("ERROR", "FreeRTOS measurement task create failed");
    recorder_status.state = State::Fault;
    recorder_status.adc_error = true;
    return false;
  }

  ok = xTaskCreate(storageTaskMain, "ft26_storage", 6144, nullptr, 2,
                   &storage_task);
  if (ok != pdPASS) {
    if (measurement_task != nullptr) {
      vTaskDelete(measurement_task);
      measurement_task = nullptr;
    }
    logLine("ERROR", "FreeRTOS storage task create failed");
    recorder_status.state = State::Fault;
    recorder_status.sd_error = true;
    return false;
  }

  tasks_started = true;
  logLine("INFO", "FreeRTOS recorder tasks started");
  return true;
}

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
  prebuffer_dump_index = 0;
  power_missing_since_ms = 0;
  record_queue::reset();
  current_zero_uv = calibration_result.hv_current_zero_uv;
  hv_voltage_zero_deci_v = calibration_result.hv_voltage_zero_deci_v;
  file_start_reference_ms = boot::status().boot_millis;
  hv_current_range_since_ms = 0;
  hv_voltage_range_since_ms = 0;
  memset(current_filename, 0, sizeof(current_filename));
  updateStorageQueueStatus();
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
    return;
  }

  recorder_status.state = State::Buffering;
  recorder_status.prebuffer_dump_done = false;
  status_led::setMode(status_led::Mode::SlowPulse);
  logLine("INFO", "Recorder buffering started");
  startRecorderTasks();
}

// 전원 감시, ADS 측정, record 생성, SD 기록을 순차 처리합니다.
void runMeasurementStep(uint32_t now) {
  if (isRecorderIdle()) {
    return;
  }

  if (confirmPowerLoss(now)) {
    handlePowerLoss();
    return;
  }

  if (isPowerLost()) {
    return;
  }

  handleAdsSchedulerResult(ads_scheduler::tick(now));

  if (static_cast<int32_t>(now - next_record_ms) >= 0) {
    captureRecord(now);
    next_record_ms += ft26::RECORD_INTERVAL_MS;
    if (static_cast<int32_t>(now - next_record_ms) >= 0) {
      next_record_ms = now + ft26::RECORD_INTERVAL_MS;
    }
  }
}

void runStorageStep(uint32_t now) {
  if (isRecorderIdle() || isPowerLost()) {
    return;
  }

  if (!lockSd(ft26::SD_MUTEX_WAIT_MS)) {
    return;
  }

  if (!isFileStarted() &&
      now - file_start_reference_ms >= ft26::FILE_LOG_START_DELAY_MS) {
    startFileLogging(now);
  }

  if (isPowerLost()) {
    closeOpenLogForPowerLoss();
    unlockSd();
    return;
  }

  dumpPrebufferChunk(now);
  drainStorageQueueChunk(now);
  syncIfNeeded(now);

  if (isPowerLost()) {
    closeOpenLogForPowerLoss();
  }

  unlockSd();
}

void measurementTaskMain(void* parameter) {
  (void)parameter;
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    runMeasurementStep(millis());
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(ft26::MEASUREMENT_TASK_INTERVAL_MS));
  }
}

void storageTaskMain(void* parameter) {
  (void)parameter;
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    runStorageStep(millis());
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(ft26::STORAGE_TASK_INTERVAL_MS));
  }
}

// loop는 FreeRTOS task가 돌 수 있게 CPU를 양보합니다.
void tick() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// recorder의 현재 상태를 반환합니다.
const Status& status() {
  return recorder_status;
}

}  // namespace ft26::recorder
