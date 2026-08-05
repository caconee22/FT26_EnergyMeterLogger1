#include "boot.h"

#include <Arduino.h>
#include <RTClib.h>
#include <Wire.h>
#include <esp_mac.h>

#include "config.h"
#include "sensors.h"
#include "status_led.h"
#include "storage.h"

namespace ft26::boot {
namespace {

RTC_DS3231 rtc;
HardwareStatus hw = {};

void logLine(const char* level, const char* message) {
  Serial.printf("[%s] %s\n", level, message);
}

void logLinef(const char* level, const char* fmt, ...) {
  char message[160] = {};
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  logLine(level, message);
}

uint8_t probeI2cAddress(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission();
}

const char* i2cErrorText(uint8_t err) {
  switch (err) {
    case 0:
      return "ACK";
    case 1:
      return "data too long";
    case 2:
      return "address NACK";
    case 3:
      return "data NACK";
    case 4:
      return "other error";
    case 5:
      return "timeout";
    default:
      return "unknown";
  }
}

void fillUidFromMac() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  log_format::setUidFromMac(hw.uid, mac);

  logLinef("INFO", "MAC %02X:%02X:%02X:%02X:%02X:%02X -> uid %08lX-%08lX-%08lX",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           static_cast<unsigned long>(hw.uid[0]),
           static_cast<unsigned long>(hw.uid[1]),
           static_cast<unsigned long>(hw.uid[2]));
}

void fillBootTimeFromRtc() {
  if (!hw.rtc_ready) {
    hw.boot_time = {};
    return;
  }

  const DateTime now = rtc.now();
  hw.boot_time.year = static_cast<uint8_t>(now.year() >= 2000 ? now.year() - 2000 : 0);
  hw.boot_time.month = now.month();
  hw.boot_time.day = now.day();
  hw.boot_time.hour = now.hour();
  hw.boot_time.minute = now.minute();
  hw.boot_time.second = now.second();
  hw.boot_time.millisecond = static_cast<uint16_t>(millis() % 1000);

  logLinef("INFO", "RTC boot time 20%02u-%02u-%02u %02u:%02u:%02u.%03u",
           hw.boot_time.year, hw.boot_time.month, hw.boot_time.day,
           hw.boot_time.hour, hw.boot_time.minute, hw.boot_time.second,
           hw.boot_time.millisecond);
}

void initializeSerial() {
  Serial.begin(ft26::SERIAL_BAUD);
  delay(200);
  hw.serial_ready = true;
  logLine("INFO", "FT26 firmware hardware start");
  logLinef("INFO", "Serial ready baud=%lu", ft26::SERIAL_BAUD);
}

void initializeLed() {
  status_led::begin();
  status_led::setMode(status_led::Mode::SolidOn);
  hw.led_ready = true;
}

void initializePowerSense() {
  hw.power_sense_ready = sensors::beginPowerSense();
  const sensors::PowerSenseReading reading = sensors::readPowerSense();
  hw.power_raw = reading.raw;
  hw.power_mv = reading.millivolts;
  hw.power_present = reading.present;

  logLinef(hw.power_present ? "INFO" : "ERROR",
           "Power sense raw=%d approx=%lu mV",
           hw.power_raw, static_cast<unsigned long>(hw.power_mv));

  if (!hw.power_present) {
    status_led::setFault(status_led::FaultGroup::Power);
  }
}

void initializeI2c() {
  Wire.begin(ft26::PIN_I2C_SDA, ft26::PIN_I2C_SCL);
  Wire.setClock(ft26::I2C_CLOCK_HZ);
  Wire.setTimeOut(ft26::I2C_TIMEOUT_MS);
  hw.i2c_ready = true;

  logLinef("INFO", "I2C ready SDA=GPIO%u SCL=GPIO%u clock=%lu timeout=%u ms",
           ft26::PIN_I2C_SDA, ft26::PIN_I2C_SCL,
           static_cast<unsigned long>(ft26::I2C_CLOCK_HZ),
           ft26::I2C_TIMEOUT_MS);
}

void initializeRtc() {
  const uint8_t ack = probeI2cAddress(ft26::I2C_ADDR_DS3231);
  hw.rtc_found = ack == 0;
  if (!hw.rtc_found) {
    logLinef("ERROR", "DS3231 missing addr=0x%02X %s (%u)",
             ft26::I2C_ADDR_DS3231, i2cErrorText(ack), ack);
    status_led::setFault(status_led::FaultGroup::Rtc);
  }

  hw.rtc_ready = rtc.begin(&Wire);
  logLinef(hw.rtc_ready ? "INFO" : "ERROR", "DS3231 begin %s",
           hw.rtc_ready ? "OK" : "FAIL");

  if (!hw.rtc_ready) {
    status_led::setFault(status_led::FaultGroup::Rtc);
    return;
  }

  hw.rtc_lost_power = rtc.lostPower();
  if (hw.rtc_lost_power) {
    logLine("ERROR", "DS3231 lostPower flag is set");
    status_led::setFault(status_led::FaultGroup::Rtc);
  }

  fillBootTimeFromRtc();
}

void initializeAds() {
  const uint8_t ack = probeI2cAddress(ft26::I2C_ADDR_ADS1115);
  hw.ads_found = ack == 0;
  if (!hw.ads_found) {
    logLinef("ERROR", "ADS1115 missing addr=0x%02X %s (%u)",
             ft26::I2C_ADDR_ADS1115, i2cErrorText(ack), ack);
    status_led::setFault(status_led::FaultGroup::Adc);
  }

  hw.ads_ready = sensors::beginAds1115();
  logLinef(hw.ads_ready ? "INFO" : "ERROR", "ADS1115 begin %s",
           hw.ads_ready ? "OK" : "FAIL");

  if (!hw.ads_ready) {
    status_led::setFault(status_led::FaultGroup::Adc);
  }
}

void initializeSd() {
  hw.sd_mounted = storage::beginCard();
  hw.sd_card_size_bytes = storage::cardSizeBytes();

  if (hw.sd_mounted) {
    logLinef("INFO", "SD mounted size=%llu MB",
             hw.sd_card_size_bytes / (1024ULL * 1024ULL));
  } else {
    logLine("ERROR", "SD mount failed");
    status_led::setFault(status_led::FaultGroup::Sd);
  }
}

}  // namespace

const HardwareStatus& initializeHardware() {
  hw = {};
  hw.boot_millis = millis();

  initializeLed();
  initializeSerial();
  fillUidFromMac();
  initializePowerSense();
  initializeI2c();
  initializeRtc();
  initializeAds();
  initializeSd();

  logLinef("INFO", "Hardware start complete rtc=%u ads=%u sd=%u power=%u",
           hw.rtc_ready, hw.ads_ready, hw.sd_mounted, hw.power_present);

  if (hw.power_present) {
    status_led::setMode(status_led::Mode::SlowPulse);
  } else {
    status_led::powerFailOff();
  }

  return hw;
}

const HardwareStatus& status() {
  return hw;
}

}  // namespace ft26::boot
