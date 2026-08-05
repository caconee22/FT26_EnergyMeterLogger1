#pragma once

#include <stdint.h>

namespace ft26::status_led {

// 정상 동작 중 LED 표시 모드입니다.
enum class Mode : uint8_t {
  Off,            // LED를 끕니다.
  SolidOn,        // LED를 계속 켭니다.
  SlowPulse,      // 1초 주기로 0.2초 켜지는 느린 점멸입니다.
  SdWriteToggle,  // SD 쓰기 이벤트마다 LED 상태를 반전합니다.
};

// 현장에서 LED 펄스 개수로 구분할 큰 오류 분류입니다.
enum class FaultGroup : uint8_t {
  None = 0,     // 오류 없음입니다.
  Power = 1,    // 전원 관련 오류입니다.
  Rtc = 2,      // RTC 관련 오류입니다.
  Adc = 3,      // ADC 또는 ADS1115 관련 오류입니다.
  Sd = 4,       // SD 카드 또는 파일 관련 오류입니다.
  Range = 5,    // 측정값 범위 오류입니다.
  Unknown = 6,  // 분류되지 않은 오류입니다.
};

// LED 타이머와 GPIO를 초기화합니다.
void begin();

// 일반 LED 모드를 설정합니다. 오류가 latch된 뒤에는 무시됩니다.
void setMode(Mode mode);

// SD 쓰기 이벤트 1회에 대해 LED 상태를 반전합니다.
void notifySdWrite();

// 오류 표시를 latch합니다. 이후 일반 LED 명령은 무시됩니다.
void setFault(FaultGroup fault);

// 전원 차단 감지 시 오류 표시보다 우선해서 LED를 끕니다.
void powerFailOff();

// 오류 표시가 latch되어 있는지 확인합니다.
bool isFaultLatched();

// 현재 latch된 오류 분류를 반환합니다.
FaultGroup latchedFault();

// 명시적인 복구나 테스트에서만 오류 latch를 해제합니다.
void clearFaultForManualReset();

}  // namespace ft26::status_led
