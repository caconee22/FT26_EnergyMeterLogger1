#include "measurements.h"

#include <Arduino.h>
#include <math.h>

#include "config.h"

namespace ft26::measurements {
namespace {

// 64비트 중간값을 가장 가까운 정수로 나누어 overflow를 줄입니다.
int32_t divideRoundNearest64(int64_t value, int64_t divisor) {
  if (divisor == 0) {
    return 0;
  }

  const int64_t half = divisor / 2;
  if (value >= 0) {
    return static_cast<int32_t>((value + half) / divisor);
  }
  return static_cast<int32_t>((value - half) / divisor);
}

}  // namespace

// ADS1115 raw count를 GAIN_TWOTHIRDS 기준 마이크로볼트로 바꿉니다.
int32_t adsRawToMicrovolts(int16_t raw) {
  return (static_cast<int32_t>(raw) *
          ft26::ADS1115_GAIN_TWOTHIRDS_UV_PER_COUNT_X10) /
         10;
}

// HV 전류 센서 전압과 zero offset으로 실제 전류를 계산합니다.
HvCurrentReading calculateHvCurrent(int16_t adc_raw, int32_t zero_offset_uv) {
  HvCurrentReading reading = {};
  reading.adc_raw = adc_raw;
  reading.sensor_uv = adsRawToMicrovolts(adc_raw);
  reading.zero_offset_uv = zero_offset_uv;

  const int32_t delta_uv = reading.sensor_uv - zero_offset_uv;
  reading.current_ma =
      divideRoundNearest64(static_cast<int64_t>(delta_uv) * 1000,
                           HV_CURRENT_SENSITIVITY_UV_PER_A);
  reading.log_deciamp =
      static_cast<int16_t>(divideRoundNearest64(delta_uv, 500));
  reading.below_range = reading.sensor_uv <= HV_CURRENT_RANGE_LOW_UV;
  reading.above_range = reading.sensor_uv >= HV_CURRENT_RANGE_HIGH_UV;
  return reading;
}

// HV 전압 ADC 값을 실제 고전압 로그 단위로 계산합니다.
HvVoltageReading calculateHvVoltage(int16_t adc_raw) {
  HvVoltageReading reading = {};
  reading.adc_raw = adc_raw;
  reading.adc_uv = adsRawToMicrovolts(adc_raw);
  reading.log_deci_v = static_cast<int16_t>(divideRoundNearest64(
      static_cast<int64_t>(reading.adc_uv) * HV_VOLTAGE_DIVIDER_RATIO_X100,
      10000000));
  reading.adc_over_range = reading.adc_uv > HV_VOLTAGE_ADC_RANGE_UV;
  reading.hv_over_range = reading.log_deci_v > HV_VOLTAGE_RANGE_DECI_V;
  return reading;
}

// LV 전압 ADC 값을 실제 VBUS 로그 단위로 계산합니다.
LvVoltageReading calculateLvVoltage(int16_t adc_raw) {
  LvVoltageReading reading = {};
  reading.adc_raw = adc_raw;
  reading.adc_uv = adsRawToMicrovolts(adc_raw);
  reading.log_centi_v = static_cast<int16_t>(divideRoundNearest64(
      static_cast<int64_t>(reading.adc_uv) * LV_VOLTAGE_RATIO_X100, 1000000));
  return reading;
}

// 기본 5V 분압 기준으로 NTC 온도를 계산합니다.
TemperatureReading calculateTemperature(int16_t adc_raw) {
  return calculateTemperature(adc_raw, TEMP_SUPPLY_UV);
}

// 지정된 분압 공급 전압 기준으로 NTC 온도를 보정 계산합니다.
TemperatureReading calculateTemperature(int16_t adc_raw, int32_t supply_uv) {
  TemperatureReading reading = {};
  reading.adc_raw = adc_raw;
  reading.adc_uv = adsRawToMicrovolts(adc_raw);
  reading.supply_uv = supply_uv;
  reading.below_range = reading.adc_uv <= TEMP_ADC_RANGE_LOW_UV;
  reading.above_range = reading.adc_uv >= TEMP_ADC_RANGE_HIGH_UV;

  if (supply_uv <= 0 || reading.adc_uv <= 0 || reading.adc_uv >= supply_uv) {
    reading.valid = false;
    reading.temperature_c = NAN;
    return reading;
  }

  const int64_t numerator =
      static_cast<int64_t>(TEMP_FIXED_RESISTOR_OHM) *
      (supply_uv - reading.adc_uv);
  reading.ntc_ohm = divideRoundNearest64(numerator, reading.adc_uv);

  const float ratio =
      static_cast<float>(reading.ntc_ohm) / static_cast<float>(TEMP_NTC_R0_OHM);
  const float temp_k =
      1.0f / ((1.0f / TEMP_NTC_T0_K) + (logf(ratio) / TEMP_NTC_BETA));
  reading.temperature_c = temp_k - 273.15f;
  reading.log_centi_c =
      static_cast<int16_t>(lroundf(reading.temperature_c * 100.0f));
  reading.valid = isfinite(reading.temperature_c);
  return reading;
}

}  // namespace ft26::measurements
