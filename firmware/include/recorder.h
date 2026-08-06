#pragma once

#include <stdint.h>

#include "calibration.h"

namespace ft26::recorder {

// 100Hz 측정 루프의 현재 상태입니다.
enum class State : uint8_t {
  Idle,             // 아직 시작하지 않은 상태입니다.
  Buffering,        // 파일을 만들지 않고 RAM에만 쌓는 상태입니다.
  FileLogging,      // SD 파일에 기록 중인 상태입니다.
  Fault,            // 기록을 계속 시도하지만 오류가 발생한 상태입니다.
};

// 측정 루프의 누적 상태입니다.
struct Status {
  State state;                 // 현재 recorder 상태입니다.
  uint32_t started_ms;         // recorder 시작 시각입니다.
  uint32_t calibrated_ms;      // calibration 완료 시각입니다.
  uint32_t records_captured;   // 생성한 전체 레코드 수입니다.
  uint32_t records_written;    // SD에 기록한 전체 레코드 수입니다.
  uint16_t buffered_records;   // 파일 시작 전 RAM에 쌓인 레코드 수입니다.
  int16_t hv_voltage_zero_deci_v; // HV 전압 zero calibration 값입니다.
  int32_t hv_current_zero_uv;  // HV 전류 센서 zero calibration 전압입니다.
  bool file_started;           // 로그 파일을 만들었는지 나타냅니다.
  bool calibrated;             // calibration 완료 여부입니다.
  bool adc_error;              // ADS1115 읽기 실패 여부입니다.
  bool sd_error;               // SD 파일 쓰기 실패 여부입니다.
  bool range_error;            // 측정 범위 오류 여부입니다.
  bool power_lost;             // 입력 전원 차단을 감지했는지 나타냅니다.
};

// calibration 완료 후 100Hz 측정 상태를 초기화합니다.
void begin(const calibration::Result& calibration_result);

// loop에서 계속 호출하는 recorder 작업 함수입니다.
void tick();

// recorder 상태를 조회합니다.
const Status& status();

}  // namespace ft26::recorder
