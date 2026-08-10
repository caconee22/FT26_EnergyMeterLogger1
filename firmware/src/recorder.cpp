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
log_format::Log pending_records[ft26::STORAGE_PENDING_RECORD_CAPACITY] = {};
size_t pending_record_count = 0;
uint32_t next_record_ms = 0;
uint32_t last_sync_ms = 0;
uint32_t file_start_reference_ms = 0;
uint16_t prebuffer_dump_index = 0;
uint32_t power_missing_since_ms = 0;
bool power_shutdown_complete = false;
int32_t current_zero_uv = measurements::HV_CURRENT_ZERO_UV;
int16_t hv_voltage_zero_deci_v = 0;
char current_filename[96] = {};
uint32_t hv_current_range_since_ms = 0;
uint32_t hv_voltage_range_since_ms = 0;
TaskHandle_t measurement_task = nullptr;
TaskHandle_t storage_task = nullptr;
portMUX_TYPE recorder_mux = portMUX_INITIALIZER_UNLOCKED;
bool tasks_started = false;
bool sd_recovery_mode = false;
uint32_t last_sd_remount_ms = 0;

bool dumpPrebufferChunk(uint32_t now);
bool flushRemainingPrebuffer(bool report_errors);
bool drainStorageQueueChunk(uint32_t now);
bool flushStorageQueue(bool report_errors);
bool isFileStarted();
void enterSdRecoveryMode();
bool isSdRecoveryMode();
bool startFileLogging(uint32_t now);
bool tryRecoverSd(uint32_t now, bool force);

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
  portEXIT_CRITICAL(&recorder_mux);
  status_led::setFault(status_led::FaultGroup::Adc);
}

// SD 파일 작업 오류를 상태와 LED fault에 반영합니다.
void markSdError(const char* action) {
  portENTER_CRITICAL(&recorder_mux);
  recorder_status.sd_error = true;
  recorder_status.sd_recovering = true;
  portEXIT_CRITICAL(&recorder_mux);
  enterSdRecoveryMode();
  status_led::setFault(status_led::FaultGroup::Sd);
  logLinef("ERROR", "SD operation failed: %s", action);
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
  const size_t queued = record_queue::count() + pending_record_count;
  recorder_status.queued_records =
      queued > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(queued);
  recorder_status.records_dropped = record_queue::droppedCount();
}

// 100Hz 측정 루프가 SD에 직접 접근하지 않도록 record를 RAM queue에 넣습니다.
bool enqueueStorageRecord(const log_format::Log& record) {
  const bool kept_all_records = record_queue::push(record);
  updateStorageQueueStatus();
  return kept_all_records;
}

// 입력 전원 차단 시 파일을 마무리하고 LED를 끕니다.
bool lockSd(uint32_t timeout_ms) {
  return storage::lockCard(timeout_ms);
}

void unlockSd() {
  storage::unlockCard();
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

// SD 오류 뒤에는 기존 파일을 믿지 않고 새 LOGn 파일을 열 때까지 RAM queue에만 쌓습니다.
void enterSdRecoveryMode() {
  portENTER_CRITICAL(&recorder_mux);
  sd_recovery_mode = true;
  recorder_status.sd_recovering = true;
  portEXIT_CRITICAL(&recorder_mux);
}

bool isSdRecoveryMode() {
  portENTER_CRITICAL(&recorder_mux);
  const bool recovering = sd_recovery_mode;
  portEXIT_CRITICAL(&recorder_mux);
  return recovering;
}

void handlePowerLoss() {
  if (power_shutdown_complete) {
    return;
  }

  if (!isPowerLost()) {
    latchPowerLost();
  }
  status_led::powerFailOff();

  if (!isFileStarted()) {
    logLine("INFO", "Power loss before log start; buffered records discarded");
    power_shutdown_complete = true;
    return;
  }

  bool flushed = true;
  bool synced = true;
  if (!lockSd(ft26::SD_EMERGENCY_MUTEX_WAIT_MS)) {
    portENTER_CRITICAL(&recorder_mux);
    recorder_status.sd_error = true;
    portEXIT_CRITICAL(&recorder_mux);
    return;
  }

  if (!storage::logFileOpen() && isSdRecoveryMode()) {
    tryRecoverSd(millis(), true);
  }

  const bool log_open = storage::logFileOpen();
  if (log_open) {
    if (isFileStarted()) {
      flushed = flushRemainingPrebuffer(true);
      flushed = flushStorageQueue(true) && flushed;
    }
    synced = storage::syncLogFile();
    if (!synced) {
      markSdError("power loss sync");
    }
    storage::closeLogFile();
  } else {
    portENTER_CRITICAL(&recorder_mux);
    const bool buffered_data = recorder_status.buffered_records > 0;
    portEXIT_CRITICAL(&recorder_mux);
    flushed = !buffered_data && pending_record_count == 0 &&
              record_queue::empty();
  }
  unlockSd();

  if (!flushed || !synced) {
    portENTER_CRITICAL(&recorder_mux);
    recorder_status.sd_error = true;
    portEXIT_CRITICAL(&recorder_mux);
    return;
  }
  power_shutdown_complete = true;
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

// 현재 세션 정보로 원본 호환 로그 header를 만듭니다.
log_format::Header makeCurrentHeader(uint32_t now) {
  const boot::HardwareStatus& hw = boot::status();
  return log_format::makeHeader(
      now, hw.uid, clampUint16(static_cast<int32_t>(hv_voltage_zero_deci_v) * 100),
      clampUint16(current_zero_uv / 1000), hw.boot_time);
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
  if (recorder_status.file_started || sd_recovery_mode) {
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

// 20초 지연 후 로그 파일을 만들고 header와 prebuffer를 기록합니다.
bool startFileLogging(uint32_t now) {
  if (isFileStarted()) {
    return true;
  }

  const log_format::Header header = makeCurrentHeader(now);

  if (!storage::openLogFile(header, current_filename, sizeof(current_filename))) {
    markSdError("open");
    return false;
  }

  portENTER_CRITICAL(&recorder_mux);
  recorder_status.file_started = true;
  if (!recorder_status.power_lost) {
    recorder_status.state = State::FileLogging;
  }
  prebuffer_dump_index = 0;
  recorder_status.prebuffer_dumped_records = 0;
  recorder_status.prebuffer_dump_done = recorder_status.buffered_records == 0;
  portEXIT_CRITICAL(&recorder_mux);

  logLinef("INFO", "Log file started %s buffered=%u",
           current_filename, recorder_status.buffered_records);

  last_sync_ms = now;
  return true;
}

// SD가 돌아오면 기존 파일을 건드리지 않고 LOG1, LOG2 같은 새 파일을 열어 queue를 비웁니다.
bool tryRecoverSd(uint32_t now, bool force) {
  if (!isSdRecoveryMode()) {
    return true;
  }

  if (!force && last_sd_remount_ms != 0 &&
      now - last_sd_remount_ms < ft26::SD_REMOUNT_INTERVAL_MS) {
    return false;
  }
  last_sd_remount_ms = now;

  if (!storage::remountCard()) {
    return false;
  }

  const log_format::Header header = makeCurrentHeader(now);
  if (!storage::openRecoveryLogFile(header, current_filename,
                                    sizeof(current_filename))) {
    return false;
  }

  portENTER_CRITICAL(&recorder_mux);
  recorder_status.file_started = true;
  if (!recorder_status.power_lost) {
    recorder_status.state = State::FileLogging;
  }
  recorder_status.sd_recovering = false;
  ++recorder_status.sd_recovery_count;
  sd_recovery_mode = false;
  portEXIT_CRITICAL(&recorder_mux);

  last_sync_ms = now;
  logLinef("INFO", "SD recovered %s queued=%u", current_filename,
           record_queue::count());
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
      (pending_record_count == 0 && record_queue::empty())) {
    return true;
  }

  const uint32_t current_ms = millis();
  if (static_cast<int32_t>(next_record_ms - current_ms) <=
      static_cast<int32_t>(ft26::PRELOG_DUMP_MIN_IDLE_MS)) {
    return true;
  }

  if (pending_record_count == 0) {
    pending_record_count = record_queue::popBlock(
        pending_records, ft26::STORAGE_WRITE_RECORDS_PER_TICK);
    updateStorageQueueStatus();
  }

  if (pending_record_count == 0) {
    return true;
  }

  if (!storage::writeRecords(pending_records, pending_record_count)) {
    markSdError("write queue chunk");
    return false;
  }

  recorder_status.records_written += pending_record_count;
  pending_record_count = 0;
  updateStorageQueueStatus();
  status_led::notifySdWrite();
  return true;
}

// 전원 차단 순간에는 남은 SD writer queue를 한 번에 써서 로그 보존을 우선합니다.
bool flushStorageQueue(bool report_errors) {
  while (pending_record_count > 0 || !record_queue::empty()) {
    if (pending_record_count == 0) {
      pending_record_count = record_queue::popBlock(
          pending_records, ft26::STORAGE_PENDING_RECORD_CAPACITY);
      updateStorageQueueStatus();
      if (pending_record_count == 0) {
        return false;
      }
    }

    if (!storage::writeRecords(pending_records, pending_record_count)) {
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

    recorder_status.records_written += pending_record_count;
    pending_record_count = 0;
    updateStorageQueueStatus();
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

  if (pending_record_count > 0 || !record_queue::empty()) {
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
  const bool fast_fresh = latest.hv_voltage_valid && latest.hv_current_valid &&
                          now - latest.hv_pair_updated_ms <=
                              ft26::FAST_SAMPLE_MAX_AGE_MS;
  const bool lv_fresh = latest.lv_valid &&
                        now - latest.lv_updated_ms <=
                            ft26::SLOW_SAMPLE_MAX_AGE_MS;
  const bool temp_fresh = latest.temp_valid &&
                          now - latest.temp_updated_ms <=
                              ft26::SLOW_SAMPLE_MAX_AGE_MS;
  if (!fast_fresh || !lv_fresh || !temp_fresh) {
    ++recorder_status.records_skipped_invalid;
    return;
  }

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
  pending_record_count = 0;
  power_missing_since_ms = 0;
  power_shutdown_complete = false;
  record_queue::reset();
  sd_recovery_mode = false;
  last_sd_remount_ms = 0;
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

  if (isSdRecoveryMode() && !tryRecoverSd(now, false)) {
    unlockSd();
    return;
  }

  if (!isFileStarted() &&
      now - file_start_reference_ms >= ft26::FILE_LOG_START_DELAY_MS) {
    if (!startFileLogging(now)) {
      unlockSd();
      return;
    }
  }

  if (!dumpPrebufferChunk(now)) {
    unlockSd();
    return;
  }
  if (!drainStorageQueueChunk(now)) {
    unlockSd();
    return;
  }
  syncIfNeeded(now);

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
  vTaskDelay(pdMS_TO_TICKS(1));
}

// recorder의 현재 상태를 반환합니다.
Status status() {
  portENTER_CRITICAL(&recorder_mux);
  const Status snapshot = recorder_status;
  portEXIT_CRITICAL(&recorder_mux);
  return snapshot;
}

bool active() {
  portENTER_CRITICAL(&recorder_mux);
  const bool is_active = recorder_status.state == State::Buffering ||
                         recorder_status.state == State::FileLogging;
  portEXIT_CRITICAL(&recorder_mux);
  return is_active;
}

}  // namespace ft26::recorder
