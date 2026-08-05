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

constexpr uint32_t POWER_SENSE_PRESENT_MV_MIN = 1800;

constexpr uint8_t ADS_CH_HV_VOLTAGE = 0;
constexpr uint8_t ADS_CH_HV_CURRENT = 1;
constexpr uint8_t ADS_CH_LV_VOLTAGE = 2;
constexpr uint8_t ADS_CH_EXTERNAL_TEMP = 3;

constexpr int32_t ADS1115_GAIN_TWOTHIRDS_UV_PER_COUNT_X10 = 1875;

constexpr uint32_t RECORD_INTERVAL_MS = 10;
constexpr uint32_t SLOW_CHANNEL_INTERVAL_MS = 100;
constexpr uint32_t FILE_SYNC_INTERVAL_MS = 100;
constexpr uint32_t FILE_LOG_START_DELAY_MS = 20000;
constexpr size_t PRELOG_RECORD_CAPACITY = FILE_LOG_START_DELAY_MS / RECORD_INTERVAL_MS;
constexpr size_t WRITE_BATCH_RECORD_COUNT = FILE_SYNC_INTERVAL_MS / RECORD_INTERVAL_MS;

}  // namespace ft26
