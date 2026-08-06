#pragma once

#include <stdint.h>

namespace ft26::sensors {

// 전원 감시용 ESP32 내장 ADC 측정 결과입니다.
struct PowerSenseReading {
  int raw;              // ADC 원시값입니다.
  uint32_t millivolts;  // Arduino 보정 기준의 전압 근사값입니다.
  bool present;         // 전원 입력이 유효하다고 판단되었는지 나타냅니다.
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

// ADS1115 단일 채널의 원시값을 읽습니다.
bool readAdsChannel(uint8_t channel, int16_t& raw);

// ADS1115 단일 채널 변환을 시작하고 기다리지 않습니다.
bool startAdsChannel(uint8_t channel);

// ADS1115 변환 완료 여부를 확인합니다.
bool adsConversionReady();

// ADS1115 마지막 변환 결과를 읽습니다.
bool readAdsLastRaw(int16_t& raw);

}  // namespace ft26::sensors
