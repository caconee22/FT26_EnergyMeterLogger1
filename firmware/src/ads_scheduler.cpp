#include "ads_scheduler.h"

#include <Arduino.h>

#include "config.h"
#include "sensors.h"

namespace ft26::ads_scheduler {
namespace {

constexpr uint32_t ADS_CONVERSION_MIN_US = 1200;
constexpr uint32_t ADS_CONVERSION_TIMEOUT_US = 5000;

enum class AdsTask : uint8_t {
  HvVoltage,
  HvCurrent,
  LvVoltage,
  Temperature,
};

struct SchedulerState {
  bool running;
  AdsTask task;
  AdsTask next_task;
  uint32_t started_us;
  uint32_t last_lv_ms;
  uint32_t last_temp_ms;
  int32_t current_zero_uv;
  measurements::HvVoltageReading pending_hv_voltage;
  bool pending_hv_voltage_valid;
  LatestReadings latest;
};

SchedulerState state = {};

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

AdsTask chooseNextTask(uint32_t now_ms) {
  const AdsTask selected = state.next_task;
  if (selected == AdsTask::HvVoltage) {
    state.next_task = AdsTask::HvCurrent;
  } else if (selected == AdsTask::HvCurrent) {
    if (state.last_lv_ms == 0 ||
        now_ms - state.last_lv_ms >= ft26::SLOW_CHANNEL_INTERVAL_MS) {
      state.next_task = AdsTask::LvVoltage;
    } else if (state.last_temp_ms == 0 ||
               now_ms - state.last_temp_ms >= ft26::SLOW_CHANNEL_INTERVAL_MS) {
      state.next_task = AdsTask::Temperature;
    } else {
      state.next_task = AdsTask::HvVoltage;
    }
  } else {
    state.next_task = AdsTask::HvVoltage;
  }
  return selected;
}

TickResult startNextConversion(uint32_t now_ms) {
  TickResult result = {};
  state.task = chooseNextTask(now_ms);
  if (!sensors::startAdsChannel(channelForTask(state.task))) {
    state.running = false;
    invalidateAll();
    result.adc_error = true;
    return result;
  }

  state.started_us = micros();
  state.running = true;
  return result;
}

TickResult applyResult(AdsTask task, int16_t raw, uint32_t now_ms) {
  TickResult result = {};
  switch (task) {
    case AdsTask::HvVoltage:
      state.pending_hv_voltage = measurements::calculateHvVoltage(raw);
      state.pending_hv_voltage_valid = true;
      break;
    case AdsTask::HvCurrent:
      if (state.pending_hv_voltage_valid) {
        state.latest.hv_voltage = state.pending_hv_voltage;
        state.latest.hv_current =
            measurements::calculateHvCurrent(raw, state.current_zero_uv);
        state.latest.hv_voltage_valid = true;
        state.latest.hv_current_valid = true;
        state.latest.hv_pair_updated_ms = now_ms;
        state.pending_hv_voltage_valid = false;
      }
      break;
    case AdsTask::LvVoltage:
      state.latest.lv_voltage = measurements::calculateLvVoltage(raw);
      state.latest.lv_valid = true;
      state.latest.lv_updated_ms = now_ms;
      state.last_lv_ms = now_ms;
      break;
    case AdsTask::Temperature:
      state.latest.temperature = measurements::calculateTemperature(raw);
      state.latest.temp_valid = state.latest.temperature.valid;
      state.latest.temp_updated_ms = now_ms;
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

void begin(int32_t current_zero_uv) {
  state = {};
  state.current_zero_uv = current_zero_uv;
  state.next_task = AdsTask::HvVoltage;
}

void setCurrentZeroUv(int32_t current_zero_uv) {
  state.current_zero_uv = current_zero_uv;
}

TickResult tick(uint32_t now_ms) {
  if (!sensors::adsReady()) {
    invalidateAll();
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
      invalidateAll();
      TickResult result = {};
      result.adc_error = true;
      return result;
    }
    return {};
  }

  int16_t raw = 0;
  if (!sensors::readAdsLastRaw(raw)) {
    state.running = false;
    invalidateAll();
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

const LatestReadings& latest() {
  return state.latest;
}

void invalidateAll() {
  state.latest.hv_voltage_valid = false;
  state.latest.hv_current_valid = false;
  state.latest.lv_valid = false;
  state.latest.temp_valid = false;
  state.pending_hv_voltage_valid = false;
}

}  // namespace ft26::ads_scheduler
