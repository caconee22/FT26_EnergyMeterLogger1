#pragma once

#include <stdint.h>

#include "measurements.h"

namespace ft26::ads_scheduler {

// ADS 비동기 스케줄러가 보관하는 최신 측정값 묶음입니다.
struct LatestReadings {
  measurements::HvVoltageReading hv_voltage;       // 마지막 HV 전압 계산값입니다.
  measurements::HvCurrentReading hv_current;       // 마지막 HV 전류 계산값입니다.
  measurements::LvVoltageReading lv_voltage;       // 마지막 LV 전압 계산값입니다.
  measurements::TemperatureReading temperature;     // 마지막 온도 계산값입니다.
  bool hv_voltage_valid;                            // HV 전압 최신값 유효 여부입니다.
  bool hv_current_valid;                            // HV 전류 최신값 유효 여부입니다.
  bool lv_valid;                                    // LV 최신값 유효 여부입니다.
  bool temp_valid;                                  // 온도 최신값 유효 여부입니다.
};

// ADS 비동기 스케줄러 1회 진행 결과입니다.
struct TickResult {
  bool adc_error;              // ADS 통신/변환 오류 발생 여부입니다.
  bool range_error;            // 즉시 range 오류 발생 여부입니다.
  const char* adc_error_source;   // ADS 오류 채널 이름입니다.
  const char* range_error_source; // range 오류 채널 이름입니다.
};

// ADS 비동기 스케줄러를 초기화하고 전류 zero offset을 적용합니다.
void begin(int32_t current_zero_uv);

// 전류 zero offset을 갱신합니다.
void setCurrentZeroUv(int32_t current_zero_uv);

// ADS1115 비동기 변환 상태를 한 단계 진행합니다.
TickResult tick(uint32_t now_ms);

// 최신 측정값 묶음을 반환합니다.
const LatestReadings& latest();

}  // namespace ft26::ads_scheduler
