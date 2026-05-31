#pragma once
#include <Arduino.h>

// ENS160 validity status flags
// 0 = Normal operation   — data valid, logging allowed
// 1 = Warm-up phase      — ~1 min after power-on, show but don't log
// 2 = Initial start-up   — ~1 hour after first use, show but don't log
// 3 = No valid output    — sensor error
struct SensorData {
    bool     rtc_ok     = false;
    bool     ens_ok     = false;
    bool     aht_ok     = false;
    uint8_t  ens_status = 3;   // 0=Normal, 1=Warmup, 2=Initial, 3=Invalid
    char     timeStr[21] = "--.--.----  --:--:--";
    uint8_t  weekday    = 0;   // 0=Sun … 6=Sat
    float    temperature = 0.0f;  // AHT21 + offset
    float    humidity    = 0.0f;  // AHT21 + offset
    uint8_t  aqi        = 0;     // 1–5 UBA index (0 = not ready)
    uint16_t tvoc       = 0;     // ppb
    uint16_t eco2       = 0;     // ppm equivalent CO2
};

void     initSensors();
void     resetENS();
void     updateSensors(SensorData &data);
bool     connectWiFi();
bool     syncRTCfromNTP();
uint32_t getRTCUnixTime();
