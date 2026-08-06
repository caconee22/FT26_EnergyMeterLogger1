#include <Arduino.h>

#include "boot.h"
#include "calibration.h"
#include "recorder.h"
#include "status_led.h"

// 부팅 시 하드웨어 준비, zero calibration, recorder 시작을 순서대로 처리합니다.
void setup() {
  ft26::boot::initializeHardware();
  const ft26::calibration::Result calibration_result =
      ft26::calibration::runHvZeroCalibration();
  ft26::recorder::begin(calibration_result);
}

// 전원 감시, ADS 비동기 측정, 100Hz record 생성, SD 기록을 계속 처리합니다.
void loop() {
  ft26::recorder::tick();
}

