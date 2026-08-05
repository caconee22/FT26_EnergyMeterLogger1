#include <Arduino.h>
#include <Adafruit_ADS1X15.h>
#include <RTClib.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <esp_mac.h>

#include "config.h"

namespace {

Adafruit_ADS1115 ads;
RTC_DS3231 rtc;

bool rtcOk = false;
bool adsOk = false;
bool sdOk = false;
bool i2cAdsFound = false;
bool i2cRtcFound = false;
uint32_t lastReportMs = 0;
uint32_t reportCount = 0;
uint8_t stepNumber = 0;
char testLog[ft26::ASSEMBLY_LOG_BUFFER_SIZE] = {};
size_t testLogLen = 0;

hw_timer_t* ledTimer = nullptr;
portMUX_TYPE ledMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint16_t ledToggleTicksRemaining = 0;
volatile bool ledLevel = false;

void IRAM_ATTR onLedTimer() {
  portENTER_CRITICAL_ISR(&ledMux);

  if (ledToggleTicksRemaining > 0) {
    ledLevel = !ledLevel;
    digitalWrite(ft26::PIN_LED, ledLevel ? HIGH : LOW);
    --ledToggleTicksRemaining;
  } else if (ledLevel) {
    ledLevel = false;
    digitalWrite(ft26::PIN_LED, LOW);
  }

  portEXIT_CRITICAL_ISR(&ledMux);
}

void requestLedPulses(uint8_t pulseCount) {
  portENTER_CRITICAL(&ledMux);
  ledToggleTicksRemaining = static_cast<uint16_t>(pulseCount) * 2;
  ledLevel = false;
  digitalWrite(ft26::PIN_LED, LOW);
  portEXIT_CRITICAL(&ledMux);
}

void startLedWorker() {
  pinMode(ft26::PIN_LED, OUTPUT);
  digitalWrite(ft26::PIN_LED, LOW);

  ledTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(ledTimer, &onLedTimer, true);
  timerAlarmWrite(ledTimer, ft26::LED_TIMER_US, true);
  timerAlarmEnable(ledTimer);
}

void appendLogLine(const char* level, const char* fmt, ...) {
  char message[192] = {};
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  Serial.printf("[%s] %s\n", level, message);

  if (testLogLen >= sizeof(testLog) - 1) {
    return;
  }

  const int written = snprintf(testLog + testLogLen, sizeof(testLog) - testLogLen,
                               "%010lu [%s] %s\n", millis(), level, message);
  if (written > 0) {
    const size_t used = static_cast<size_t>(written);
    testLogLen += min(used, sizeof(testLog) - testLogLen - 1);
  }
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

uint8_t probeI2cAddress(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission();
}

String macString() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  char buffer[18] = {};
  snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buffer);
}

bool printI2cScan() {
  appendLogLine("INFO", "I2C scan start");

  uint8_t found = 0;
  i2cAdsFound = false;
  i2cRtcFound = false;
  for (uint8_t addr = 1; addr < 127; ++addr) {
    const uint8_t err = probeI2cAddress(addr);
    if (err == 0) {
      const char* name = "";
      if (addr == ft26::I2C_ADDR_ADS1115) {
        name = " ADS1115";
        i2cAdsFound = true;
      } else if (addr == ft26::I2C_ADDR_DS3231) {
        name = " DS3231";
        i2cRtcFound = true;
      }
      appendLogLine("INFO", "I2C device found addr=0x%02X%s", addr, name);
      ++found;
    }
  }

  appendLogLine("INFO", "I2C scan done found=%u", found);

  if (!i2cAdsFound) {
    const uint8_t err = probeI2cAddress(ft26::I2C_ADDR_ADS1115);
    appendLogLine("ERROR", "ADS1115 address 0x%02X missing: %s (%u)",
                  ft26::I2C_ADDR_ADS1115, i2cErrorText(err), err);
  }
  if (!i2cRtcFound) {
    const uint8_t err = probeI2cAddress(ft26::I2C_ADDR_DS3231);
    appendLogLine("ERROR", "DS3231 address 0x%02X missing: %s (%u)",
                  ft26::I2C_ADDR_DS3231, i2cErrorText(err), err);
  }

  return i2cAdsFound && i2cRtcFound;
}

bool initRtc() {
  const uint8_t ack = probeI2cAddress(ft26::I2C_ADDR_DS3231);
  if (ack != 0) {
    appendLogLine("ERROR", "RTC communication failed before begin: addr=0x%02X %s (%u)",
                  ft26::I2C_ADDR_DS3231, i2cErrorText(ack), ack);
  }

  rtcOk = rtc.begin(&Wire);
  appendLogLine(rtcOk ? "INFO" : "ERROR", "DS3231 begin %s", rtcOk ? "OK" : "FAIL");

  if (!rtcOk) {
    return false;
  }

  const DateTime now = rtc.now();
  appendLogLine("INFO", "RTC now %04d-%02d-%02d %02d:%02d:%02d",
                now.year(), now.month(), now.day(), now.hour(), now.minute(),
                now.second());

  if (rtc.lostPower()) {
    appendLogLine("WARN", "RTC lostPower flag is set");
  }

  return true;
}

bool initAds() {
  const uint8_t ack = probeI2cAddress(ft26::I2C_ADDR_ADS1115);
  if (ack != 0) {
    appendLogLine("ERROR", "ADS1115 communication failed before begin: addr=0x%02X %s (%u)",
                  ft26::I2C_ADDR_ADS1115, i2cErrorText(ack), ack);
  }

  adsOk = ads.begin(ft26::I2C_ADDR_ADS1115, &Wire);
  appendLogLine(adsOk ? "INFO" : "ERROR", "ADS1115 begin %s", adsOk ? "OK" : "FAIL");

  if (!adsOk) {
    return false;
  }

  ads.setGain(GAIN_TWOTHIRDS);
  ads.setDataRate(RATE_ADS1115_860SPS);
  appendLogLine("INFO", "ADS1115 gain=GAIN_TWOTHIRDS dataRate=860SPS");
  return true;
}

bool initSd() {
  SPI.begin(ft26::PIN_SD_SCK, ft26::PIN_SD_MISO, ft26::PIN_SD_MOSI,
            ft26::PIN_SD_CS);
  appendLogLine("INFO", "SD SPI begin SCK=GPIO%u MISO=GPIO%u MOSI=GPIO%u CS=GPIO%u",
                ft26::PIN_SD_SCK, ft26::PIN_SD_MISO, ft26::PIN_SD_MOSI,
                ft26::PIN_SD_CS);

  sdOk = SD.begin(ft26::PIN_SD_CS, SPI);
  appendLogLine(sdOk ? "INFO" : "ERROR", "SD mount %s", sdOk ? "OK" : "FAIL");

  if (!sdOk) {
    appendLogLine("ERROR", "SD mount failed: check card, socket, SPI pins, CS, and 3.3V power");
    return false;
  }

  appendLogLine("INFO", "SD card size %llu MB", SD.cardSize() / (1024ULL * 1024ULL));

  File file = SD.open(ft26::ASSEMBLY_TEST_FILE, FILE_WRITE);
  if (!file) {
    appendLogLine("ERROR", "SD file create/open failed: %s", ft26::ASSEMBLY_TEST_FILE);
    return false;
  }

  appendLogLine("INFO", "SD file create/open OK: %s", ft26::ASSEMBLY_TEST_FILE);

  file.println();
  file.println("==== FT26 hardware assembly test ====");
  file.write(reinterpret_cast<const uint8_t*>(testLog), testLogLen);
  file.close();
  appendLogLine("INFO", "SD wrote diagnostic log: %s bytes=%u",
                ft26::ASSEMBLY_TEST_FILE, static_cast<unsigned>(testLogLen));
  return true;
}

bool printPowerSense() {
  const int raw = analogRead(ft26::PIN_POWER_SENSE_ADC);
  const uint32_t mv = analogReadMilliVolts(ft26::PIN_POWER_SENSE_ADC);
  appendLogLine(raw > 0 ? "INFO" : "ERROR", "power-sense GPIO%u raw=%d approx=%lu mV",
                ft26::PIN_POWER_SENSE_ADC, raw, mv);
  return raw > 0;
}

bool printAdsChannels() {
  if (!adsOk) {
    appendLogLine("ERROR", "ADS1115 channel read skipped: ADS1115 setup failed");
    return false;
  }

  int16_t adc[4] = {};
  for (uint8_t ch = 0; ch < 4; ++ch) {
    adc[ch] = ads.readADC_SingleEnded(ch);
  }

  appendLogLine("INFO", "ADS1115 raw A0=%d A1=%d A2=%d A3=%d",
                adc[0], adc[1], adc[2], adc[3]);
  return true;
}

bool printRtcNow() {
  if (!rtcOk) {
    appendLogLine("ERROR", "RTC read skipped: RTC setup failed");
    return false;
  }

  const DateTime now = rtc.now();
  appendLogLine("INFO", "RTC %04d-%02d-%02d %02d:%02d:%02d",
                now.year(), now.month(), now.day(), now.hour(), now.minute(),
                now.second());
  return true;
}

void printReport() {
  requestLedPulses(ft26::LED_PULSE_REPORT);
  appendLogLine("INFO", "REPORT %lu millis=%lu", ++reportCount, millis());
  printPowerSense();
  printRtcNow();
  printAdsChannels();
}

bool step(const char* name, bool (*action)()) {
  appendLogLine("INFO", "STEP %02u %s start", ++stepNumber, name);
  requestLedPulses(ft26::LED_PULSE_STEP_START);
  delay(ft26::ASSEMBLY_STEP_PAUSE_MS);

  const bool ok = action();
  appendLogLine(ok ? "PASS" : "FAIL", "STEP %02u %s %s", stepNumber, name,
                ok ? "PASS" : "FAIL");
  requestLedPulses(ok ? ft26::LED_PULSE_STEP_PASS : ft26::LED_PULSE_STEP_FAIL);
  delay(ft26::ASSEMBLY_STEP_PAUSE_MS);
  return ok;
}

bool setupAdc() {
  analogReadResolution(12);
  analogSetPinAttenuation(ft26::PIN_POWER_SENSE_ADC, ADC_11db);
  return printPowerSense();
}

bool setupI2c() {
  Wire.begin(ft26::PIN_I2C_SDA, ft26::PIN_I2C_SCL);
  Wire.setClock(ft26::I2C_CLOCK_HZ);
  Wire.setTimeOut(ft26::I2C_TIMEOUT_MS);
  appendLogLine("INFO", "I2C setup SDA=GPIO%u SCL=GPIO%u clock=%lu timeout=%u ms",
                ft26::PIN_I2C_SDA, ft26::PIN_I2C_SCL,
                ft26::I2C_CLOCK_HZ, ft26::I2C_TIMEOUT_MS);
  return true;
}

bool printBootIdentity() {
  appendLogLine("INFO", "BOOT MAC=%s", macString().c_str());
  appendLogLine("INFO", "BOOT millis=%lu", millis());
  return true;
}

}  // namespace

void setup() {
  startLedWorker();

  Serial.begin(ft26::SERIAL_BAUD);
  delay(300);

  Serial.println();
  Serial.println("FT26 EnergyMeter hardware assembly test");
  appendLogLine("INFO", "FT26 EnergyMeter hardware assembly test start");

  step("boot identity", printBootIdentity);
  step("power-sense ADC", setupAdc);
  step("I2C bus setup", setupI2c);
  step("I2C device scan", printI2cScan);
  step("DS3231 RTC", initRtc);
  step("ADS1115 setup", initAds);
  step("ADS1115 channel read", printAdsChannels);
  step("microSD write", initSd);

  printReport();
}

void loop() {
  const uint32_t now = millis();
  if (now - lastReportMs >= ft26::ASSEMBLY_TEST_INTERVAL_MS) {
    lastReportMs = now;
    printReport();
  }
}
