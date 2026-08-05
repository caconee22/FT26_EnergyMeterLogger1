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

constexpr uint8_t PIN_UART_RX = 20;
constexpr uint8_t PIN_UART_TX = 21;

constexpr uint8_t I2C_ADDR_ADS1115 = 0x48;
constexpr uint8_t I2C_ADDR_DS3231 = 0x68;

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t I2C_CLOCK_HZ = 400000;
constexpr uint16_t I2C_TIMEOUT_MS = 5;

constexpr uint32_t RECORD_INTERVAL_MS = 10;
constexpr uint32_t SLOW_CHANNEL_INTERVAL_MS = 100;
constexpr uint32_t FILE_SYNC_INTERVAL_MS = 100;

}  // namespace ft26
