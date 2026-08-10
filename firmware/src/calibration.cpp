#include "calibration.h"

#include <Arduino.h>

#include "config.h"
#include "measurements.h"
#include "sensors.h"

namespace ft26::calibration {
namespace {

// calibration 대기 중에도 전원 차단을 빠르게 확인합니다.
bool waitWithPowerCheck(uint32_t wait_ms) {
  const uint32_t start_ms = millis();
  while (millis() - start_ms < wait_ms) {
    if (!sensors::readPowerSense().present) {
      return false;
    }
    delay(5);
  }
  return true;
}

}  // namespace

// 원본 로거 방식으로 300ms 대기 후 HV 전압/전류 zero calibration을 수행합니다.
Result runHvZeroCalibration() {
  Result result = {};
  result.hv_current_zero_uv = measurements::HV_CURRENT_ZERO_UV;

  Serial.printf("[INFO] Calibration wait %lu ms\n",
                static_cast<unsigned long>(ft26::CALIBRATION_WAIT_MS));
  if (!waitWithPowerCheck(ft26::CALIBRATION_WAIT_MS)) {
    result.power_lost = true;
    Serial.println("[ERROR] Calibration stopped by power loss");
    return result;
  }

  Serial.printf("[INFO] Calibration start samples=%u\n",
                ft26::CALIBRATION_SAMPLE_COUNT);
  int64_t hv_voltage_sum_deci_v = 0;
  int64_t hv_current_sum_uv = 0;

  for (uint8_t i = 0; i < ft26::CALIBRATION_SAMPLE_COUNT; ++i) {
    if (!sensors::readPowerSense().present) {
      result.power_lost = true;
      Serial.println("[ERROR] Calibration stopped by power loss");
      return result;
    }

    int16_t hv_voltage_raw = 0;
    int16_t hv_current_raw = 0;
    if (!sensors::readAdsChannel(ft26::ADS_CH_HV_VOLTAGE, hv_voltage_raw)) {
      result.adc_error = true;
      Serial.println("[ERROR] Calibration ADS read failed channel=hv-voltage");
      return result;
    }
    if (!sensors::readAdsChannel(ft26::ADS_CH_HV_CURRENT, hv_current_raw)) {
      result.adc_error = true;
      Serial.println("[ERROR] Calibration ADS read failed channel=hv-current");
      return result;
    }

    const measurements::HvVoltageReading hv_voltage =
        measurements::calculateHvVoltage(hv_voltage_raw);
    hv_voltage_sum_deci_v += hv_voltage.log_deci_v;
    hv_current_sum_uv += measurements::adsRawToMicrovolts(hv_current_raw);
    delay(ft26::CALIBRATION_SAMPLE_DELAY_MS);
  }

  result.hv_voltage_zero_deci_v =
      static_cast<int16_t>(hv_voltage_sum_deci_v / ft26::CALIBRATION_SAMPLE_COUNT);
  result.hv_current_zero_uv =
      static_cast<int32_t>(hv_current_sum_uv / ft26::CALIBRATION_SAMPLE_COUNT);
  result.completed_ms = millis();
  result.ok = true;

  Serial.printf("[INFO] Calibration done hv_zero=%.1f V current_zero=%ld uV\n",
                result.hv_voltage_zero_deci_v / 10.0f,
                static_cast<long>(result.hv_current_zero_uv));
  return result;
}

}  // namespace ft26::calibration
