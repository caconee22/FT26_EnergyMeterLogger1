#pragma once

#include <Arduino.h>

namespace ft26 {

constexpr uint8_t PIN_LED = 3;

constexpr uint8_t PIN_SD_SCK = 4;
constexpr uint8_t PIN_SD_MISO = 5;
constexpr uint8_t PIN_SD_MOSI = 6;
constexpr uint8_t PIN_SD_CS = 7;

constexpr uint8_t PIN_I2C_SDA = 8;
constexpr uint8_t PIN_I2C_SCL = 9;

constexpr uint8_t PIN_POWER_SENSE_ADC = 0;

constexpr uint8_t I2C_ADDR_ADS1115 = 0x48;
constexpr uint8_t I2C_ADDR_DS3231 = 0x68;

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t I2C_CLOCK_HZ = 400000;
constexpr uint16_t I2C_TIMEOUT_MS = 5;

constexpr uint32_t LED_TIMER_US = 50000;
constexpr uint8_t LED_PULSE_STEP_START = 1;
constexpr uint8_t LED_PULSE_STEP_PASS = 2;
constexpr uint8_t LED_PULSE_STEP_FAIL = 6;
constexpr uint8_t LED_PULSE_REPORT = 1;

constexpr uint32_t ASSEMBLY_STEP_PAUSE_MS = 250;
constexpr uint32_t ASSEMBLY_TEST_INTERVAL_MS = 1000;
constexpr size_t ASSEMBLY_LOG_BUFFER_SIZE = 4096;
constexpr char ASSEMBLY_TEST_FILE[] = "/FT26_HW_TEST.TXT";

}  // namespace ft26
