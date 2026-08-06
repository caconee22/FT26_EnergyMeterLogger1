#include "sensors.h"

#include <Adafruit_ADS1X15.h>
#include <Arduino.h>

#include "config.h"

namespace ft26::sensors {
namespace {

Adafruit_ADS1115 ads;
bool ads_ready = false;

}  // namespace

// ESP32 내장 ADC 전원 감시 핀을 설정합니다.
bool beginPowerSense() {
  analogReadResolution(12);
  analogSetPinAttenuation(ft26::PIN_POWER_SENSE_ADC, ADC_11db);
  return true;
}

// ESP32 내장 ADC로 입력 전원 존재 여부를 읽습니다.
PowerSenseReading readPowerSense() {
  PowerSenseReading reading = {};
  reading.raw = analogRead(ft26::PIN_POWER_SENSE_ADC);
  reading.millivolts = analogReadMilliVolts(ft26::PIN_POWER_SENSE_ADC);
  reading.present = reading.millivolts >= ft26::POWER_SENSE_PRESENT_MV_MIN;
  return reading;
}

// ADS1115를 시작하고 gain/data rate를 설정합니다.
bool beginAds1115() {
  ads_ready = ads.begin(ft26::I2C_ADDR_ADS1115, &Wire);
  if (!ads_ready) {
    return false;
  }

  ads.setGain(GAIN_TWOTHIRDS);
  ads.setDataRate(RATE_ADS1115_860SPS);
  return true;
}

// ADS1115 초기화 성공 상태를 반환합니다.
bool adsReady() {
  return ads_ready;
}

// ADS1115 4개 채널을 blocking 방식으로 한 번씩 읽습니다.
bool readAdsRaw(int16_t channels[4]) {
  if (!ads_ready || channels == nullptr) {
    return false;
  }

  for (uint8_t ch = 0; ch < 4; ++ch) {
    channels[ch] = ads.readADC_SingleEnded(ch);
  }

  return true;
}

// ADS1115 단일 채널을 blocking 방식으로 읽습니다.
bool readAdsChannel(uint8_t channel, int16_t& raw) {
  if (!ads_ready || channel > 3) {
    return false;
  }

  raw = ads.readADC_SingleEnded(channel);
  return true;
}

// ADS1115 단일 채널 변환을 non-blocking 방식으로 시작합니다.
bool startAdsChannel(uint8_t channel) {
  if (!ads_ready || channel > 3) {
    return false;
  }

  ads.startADCReading(MUX_BY_CHANNEL[channel], false);
  return true;
}

// ADS1115 non-blocking 변환 완료 여부를 확인합니다.
bool adsConversionReady() {
  if (!ads_ready) {
    return false;
  }

  return ads.conversionComplete();
}

// ADS1115 마지막 변환 결과를 읽습니다.
bool readAdsLastRaw(int16_t& raw) {
  if (!ads_ready) {
    return false;
  }

  raw = ads.getLastConversionResults();
  return true;
}

}  // namespace ft26::sensors
