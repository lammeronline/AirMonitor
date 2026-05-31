#pragma once

// ---------- I2C pins (OLED, ENS160, AHT21, DS3231) ----------
#define I2C_SDA 21
#define I2C_SCL 22

// ---------- I2C addresses -----------------------------------
#define OLED_ADDR           0x3C
#define ENS160_ADDR_LOW     0x52    // ADDR pin → GND
#define ENS160_ADDR_HIGH    0x53    // ADDR pin → VCC

// ---------- SD card (SPI) -----------------------------------
#define SD_CS    5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK  18

// ---------- OLED SSD1306 ------------------------------------
#define OLED_WIDTH  128
#define OLED_HEIGHT  64

// ---------- RGB LED (PWM, common-cathode) --------------------
#define LED_R 25
#define LED_G 26
#define LED_B 27

// ---------- WiFi / NTP --------------------------------------
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""
#define NTP_SERVER    "pool.ntp.org"
#define NTP_OFFSET    10800

// ---------- Device ------------------------------------------
#define DEVICE_NAME "airmonitor"

// ---------- Update intervals --------------------------------
#define DATA_LOG_INTERVAL_SEC 60
