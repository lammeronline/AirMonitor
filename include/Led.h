#pragma once
#include <Arduino.h>

void initLED();
void setLED(uint8_t r, uint8_t g, uint8_t b);
void offLED();
// sensor_ok = aht_ok && ens_ok && ens_status == 0
void updateLED(float temp, float hum, uint8_t aqi, bool sensor_ok);
