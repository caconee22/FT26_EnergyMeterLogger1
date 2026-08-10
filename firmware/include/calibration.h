#pragma once

#include <stdint.h>

namespace ft26::calibration {

// 부팅 zero calibration 결과입니다.
struct Result {
  bool ok;                       // calibration 성공 여부입니다.
  bool power_lost;               // calibration 중 전원 차단 여부입니다.
  bool adc_error;                // calibration 중 ADS 오류 여부입니다.
  uint32_t completed_ms;         // calibration 완료 시각입니다.
  int16_t hv_voltage_zero_deci_v; // HV 전압 zero calibration 값입니다.
  int32_t hv_current_zero_uv;    // HV 전류 센서 zero calibration 전압입니다.
};

// 원본 로거 방식으로 300ms 대기 후 HV 전압/전류 zero calibration을 수행합니다.
Result runHvZeroCalibration();

}  // namespace ft26::calibration
