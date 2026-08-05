#pragma once

#include <stdint.h>

namespace ft26::sensors {

// 전원 감시용 ESP32 내장 ADC 측정 결과입니다.
struct PowerSenseReading {
  int raw;              // ADC 원시값입니다.
  uint32_t millivolts;  // Arduino 보정 기준의 전압 근사값입니다.
  bool present;         // 전원 입력이 유효하다고 판단되었는지 나타냅니다.
};

constexpr int32_t HV_CURRENT_ZERO_UV = 2500000;              // QNHCK1-21 400A 무전류 기준 전압입니다.
constexpr int32_t HV_CURRENT_SENSITIVITY_UV_PER_A = 5000;    // QNHCK1-21 400A 감도입니다.
constexpr int32_t HV_CURRENT_RANGE_LOW_UV = 400000;          // 전류 센서 저전압 range 기준입니다.
constexpr int32_t HV_CURRENT_RANGE_HIGH_UV = 4600000;        // 전류 센서 고전압 range 기준입니다.
constexpr uint32_t HV_CURRENT_RANGE_HOLD_MS = 3000;          // 전류 센서 range fault 지속시간 기준입니다.

constexpr int32_t HV_VOLTAGE_DIVIDER_RATIO_X100 = 15029;     // HV:ADC = 150.29:1 입니다.
constexpr int32_t HV_VOLTAGE_ADC_RANGE_UV = 2000000;         // HV ADC 입력 range 기준입니다.
constexpr int16_t HV_VOLTAGE_RANGE_DECI_V = 3000;            // HV 300.0V range 기준입니다.
constexpr uint32_t HV_VOLTAGE_RANGE_HOLD_MS = 1000;          // HV range fault 지속시간 기준입니다.

constexpr int32_t LV_VOLTAGE_RATIO_X100 = 570;               // VBUS:ADC = 5.7:1 입니다.

// HV 전류 센서 전압을 전류 단위로 변환한 결과입니다.
struct HvCurrentReading {
  int16_t adc_raw;        // ADS1115 원시값입니다.
  int32_t sensor_uv;      // 센서 출력 전압입니다.
  int32_t zero_offset_uv; // calibration으로 잡은 무전류 기준 전압입니다.
  int32_t current_ma;     // 계산된 전류 mA입니다.
  int16_t log_deciamp;    // 원본 로그 포맷에 넣을 0.1A 단위 값입니다.
  bool below_range;       // 0.4V 이하 즉시 range 상태입니다.
  bool above_range;       // 4.6V 이상 즉시 range 상태입니다.
};

// HV 전압 ADC 값을 원본 로그 단위로 변환한 결과입니다.
struct HvVoltageReading {
  int16_t adc_raw;       // ADS1115 원시값입니다.
  int32_t adc_uv;        // ADS1115 입력 전압입니다.
  int16_t log_deci_v;    // 원본 로그 포맷에 넣을 0.1V 단위 값입니다.
  bool adc_over_range;   // ADC 입력이 2.0V를 넘었는지 나타냅니다.
  bool hv_over_range;    // 계산된 HV가 300V를 넘었는지 나타냅니다.
};

// LV 전압 ADC 값을 원본 로그 단위로 변환한 결과입니다.
struct LvVoltageReading {
  int16_t adc_raw;        // ADS1115 원시값입니다.
  int32_t adc_uv;         // ADS1115 입력 전압입니다.
  int16_t log_centi_v;    // 원본 로그 포맷에 넣을 0.01V 단위 값입니다.
};

// 전원 감시 ADC 핀을 초기화합니다.
bool beginPowerSense();

// 전원 감시 ADC 값을 1회 읽습니다.
PowerSenseReading readPowerSense();

// ADS1115를 초기화하고 측정 설정을 적용합니다.
bool beginAds1115();

// ADS1115가 정상 초기화되었는지 반환합니다.
bool adsReady();

// ADS1115 4개 채널의 원시값을 읽습니다.
bool readAdsRaw(int16_t channels[4]);

// ADS1115 원시값을 GAIN_TWOTHIRDS 기준 마이크로볼트로 변환합니다.
int32_t adsRawToMicrovolts(int16_t raw);

// HV 전류 센서 전압과 zero offset으로 전류를 계산합니다.
HvCurrentReading calculateHvCurrent(int16_t adc_raw, int32_t zero_offset_uv);

// HV 전압 ADC 값을 실제 고전압과 원본 로그 단위로 계산합니다.
HvVoltageReading calculateHvVoltage(int16_t adc_raw);

// LV 전압 ADC 값을 실제 VBUS와 원본 로그 단위로 계산합니다.
LvVoltageReading calculateLvVoltage(int16_t adc_raw);

}  // namespace ft26::sensors
