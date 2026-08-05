#include "sensors.h"

#include <Adafruit_ADS1X15.h>
#include <Arduino.h>

#include "config.h"

namespace ft26::sensors {
namespace {

Adafruit_ADS1115 ads;
bool ads_ready = false;

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

bool beginPowerSense() {
  analogReadResolution(12);
  analogSetPinAttenuation(ft26::PIN_POWER_SENSE_ADC, ADC_11db);
  return true;
}

PowerSenseReading readPowerSense() {
  PowerSenseReading reading = {};
  reading.raw = analogRead(ft26::PIN_POWER_SENSE_ADC);
  reading.millivolts = analogReadMilliVolts(ft26::PIN_POWER_SENSE_ADC);
  reading.present = reading.millivolts >= ft26::POWER_SENSE_PRESENT_MV_MIN;
  return reading;
}

bool beginAds1115() {
  ads_ready = ads.begin(ft26::I2C_ADDR_ADS1115, &Wire);
  if (!ads_ready) {
    return false;
  }

  ads.setGain(GAIN_TWOTHIRDS);
  ads.setDataRate(RATE_ADS1115_860SPS);
  return true;
}

bool adsReady() {
  return ads_ready;
}

bool readAdsRaw(int16_t channels[4]) {
  if (!ads_ready || channels == nullptr) {
    return false;
  }

  for (uint8_t ch = 0; ch < 4; ++ch) {
    channels[ch] = ads.readADC_SingleEnded(ch);
  }

  return true;
}

int32_t adsRawToMicrovolts(int16_t raw) {
  return (static_cast<int32_t>(raw) *
          ft26::ADS1115_GAIN_TWOTHIRDS_UV_PER_COUNT_X10) /
         10;
}

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

LvVoltageReading calculateLvVoltage(int16_t adc_raw) {
  LvVoltageReading reading = {};
  reading.adc_raw = adc_raw;
  reading.adc_uv = adsRawToMicrovolts(adc_raw);
  reading.log_centi_v = static_cast<int16_t>(divideRoundNearest64(
      static_cast<int64_t>(reading.adc_uv) * LV_VOLTAGE_RATIO_X100, 1000000));
  return reading;
}

}  // namespace ft26::sensors
