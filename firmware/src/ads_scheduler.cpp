#include "ads_scheduler.h"

#include <Arduino.h>

#include "config.h"
#include "sensors.h"

namespace ft26::ads_scheduler {
namespace {

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
struct SchedulerState {
  bool running;             // 변환이 진행 중인지 나타냅니다.
  bool hv_voltage_turn;     // HV 전압/전류 번갈이 순서입니다.
  AdsTask task;             // 현재 변환 중인 작업입니다.
  uint32_t started_us;      // 변환 시작 시각입니다.
  uint32_t last_lv_ms;      // 마지막 LV 갱신 시각입니다.
  uint32_t last_temp_ms;    // 마지막 온도 갱신 시각입니다.
  int32_t current_zero_uv;  // HV 전류 zero offset입니다.
  LatestReadings latest;    // 최신 측정값 묶음입니다.
};

SchedulerState state = {};

// ADS 측정 작업 종류를 실제 ADS1115 채널 번호로 변환합니다.
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

// ADS 측정 작업 이름을 오류 로그용 문자열로 변환합니다.
// LV/온도 주기와 HV 번갈이 규칙에 따라 다음 ADS 작업을 고릅니다.
AdsTask chooseNextTask(uint32_t now_ms) {
  if (state.last_lv_ms == 0 ||
      now_ms - state.last_lv_ms >= ft26::SLOW_CHANNEL_INTERVAL_MS) {
    return AdsTask::LvVoltage;
  }

  if (state.last_temp_ms == 0 ||
      now_ms - state.last_temp_ms >= ft26::SLOW_CHANNEL_INTERVAL_MS) {
    return AdsTask::Temperature;
  }

  state.hv_voltage_turn = !state.hv_voltage_turn;
  return state.hv_voltage_turn ? AdsTask::HvVoltage : AdsTask::HvCurrent;
}

// 선택된 다음 ADS 채널 변환을 시작하고 기다리지 않습니다.
TickResult startNextConversion(uint32_t now_ms) {
  TickResult result = {};
  state.task = chooseNextTask(now_ms);
  if (!sensors::startAdsChannel(channelForTask(state.task))) {
    state.running = false;
    result.adc_error = true;
    return result;
  }

  state.started_us = micros();
  state.running = true;
  return result;
}

// 완료된 ADS 원시값을 채널별 계산값으로 반영합니다.
TickResult applyResult(AdsTask task, int16_t raw, uint32_t now_ms) {
  TickResult result = {};
  switch (task) {
    case AdsTask::HvVoltage:
      state.latest.hv_voltage = measurements::calculateHvVoltage(raw);
      state.latest.hv_voltage_valid = true;
      break;
    case AdsTask::HvCurrent:
      state.latest.hv_current =
          measurements::calculateHvCurrent(raw, state.current_zero_uv);
      state.latest.hv_current_valid = true;
      break;
    case AdsTask::LvVoltage:
      state.latest.lv_voltage = measurements::calculateLvVoltage(raw);
      state.latest.lv_valid = true;
      state.last_lv_ms = now_ms;
      break;
    case AdsTask::Temperature:
      state.latest.temperature = measurements::calculateTemperature(raw);
      state.latest.temp_valid = state.latest.temperature.valid;
      state.last_temp_ms = now_ms;
      if (state.latest.temperature.below_range ||
          state.latest.temperature.above_range) {
        result.range_error = true;
      }
      break;
  }
  return result;
}

}  // namespace

// ADS 비동기 스케줄러를 초기화하고 전류 zero offset을 적용합니다.
void begin(int32_t current_zero_uv) {
  state = {};
  state.current_zero_uv = current_zero_uv;
}

// 전류 zero offset을 갱신합니다.
void setCurrentZeroUv(int32_t current_zero_uv) {
  state.current_zero_uv = current_zero_uv;
}

// ADS1115 비동기 변환 상태를 한 단계 진행합니다.
TickResult tick(uint32_t now_ms) {
  if (!sensors::adsReady()) {
    TickResult result = {};
    result.adc_error = true;
    return result;
  }

  if (!state.running) {
    return startNextConversion(now_ms);
  }

  const uint32_t elapsed_us = micros() - state.started_us;
  if (elapsed_us < ADS_CONVERSION_MIN_US) {
    return {};
  }

  if (!sensors::adsConversionReady()) {
    if (elapsed_us >= ADS_CONVERSION_TIMEOUT_US) {
      state.running = false;
      TickResult result = {};
      result.adc_error = true;
      return result;
    }
    return {};
  }

  int16_t raw = 0;
  if (!sensors::readAdsLastRaw(raw)) {
    state.running = false;
    TickResult result = {};
    result.adc_error = true;
    return result;
  }

  TickResult result = applyResult(state.task, raw, now_ms);
  state.running = false;

  const TickResult start_result = startNextConversion(now_ms);
  if (start_result.adc_error) {
    return start_result;
  }
  return result;
}

// 최신 측정값 묶음을 반환합니다.
const LatestReadings& latest() {
  return state.latest;
}

}  // namespace ft26::ads_scheduler
