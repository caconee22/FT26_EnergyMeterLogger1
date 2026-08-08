#pragma once

#include <stdint.h>

#include "log_format.h"

namespace ft26::boot {

enum class Mode : uint8_t {
  Record,
  Com,
};

// 부팅 중 각 하드웨어가 준비되었는지 저장하는 상태 묶음입니다.
struct HardwareStatus {
  bool serial_ready;       // 시리얼 포트 초기화 성공 여부입니다.
  bool led_ready;          // 상태 LED 초기화 성공 여부입니다.
  bool power_sense_ready;  // 전원 감시 ADC 초기화 성공 여부입니다.
  bool power_present;      // 5V 입력 전원이 유효 범위에 있는지 나타냅니다.
  bool i2c_ready;          // I2C 버스 초기화 성공 여부입니다.
  bool rtc_found;          // DS3231 주소 응답 여부입니다.
  bool rtc_ready;          // DS3231 드라이버 초기화 성공 여부입니다.
  bool rtc_lost_power;     // RTC 백업 전원 손실 플래그입니다.
  bool ads_found;          // ADS1115 주소 응답 여부입니다.
  bool ads_ready;          // ADS1115 드라이버 초기화 성공 여부입니다.
  bool sd_mounted;         // microSD 카드 마운트 성공 여부입니다.
  Mode mode;               // 부팅 뒤 선택된 동작 모드입니다.

  int power_raw;                     // 전원 감시 ADC 원시값입니다.
  uint32_t power_mv;                 // 전원 감시 ADC 전압 근사값입니다.
  uint64_t sd_card_size_bytes;       // 감지된 SD 카드 전체 용량입니다.
  uint32_t boot_millis;              // 초기화 시작 시점의 millis 값입니다.
  uint32_t uid[3];                   // 원본 로그 헤더와 호환되는 96비트 UID 영역입니다.
  log_format::BootTime boot_time;    // 로그 헤더에 넣을 부팅 시각입니다.
};

// 모든 하드웨어를 초기화하고 준비 상태를 반환합니다.
const HardwareStatus& initializeHardware();

// 마지막 초기화 결과를 조회합니다.
const HardwareStatus& status();

// 현재 부팅에서 선택된 동작 모드를 반환합니다.
Mode mode();

}  // namespace ft26::boot
