#pragma once
#include <Arduino.h>
#include "Sensors.h"

struct UiStatus {
    bool sdReady = false;
    uint64_t sdSizeMb = 0;
    unsigned long lastLogWriteMs = 0;
};

void initUI();
void enableUI(bool on);
bool uiEnabled();
bool handleUI();          // returns true if a redraw was triggered by page rotation
void invalidateUI();
void drawUI(const SensorData &sensor, const UiStatus &status);
void drawBootLine(const char *label, const char *value, bool ok);
void drawBootTitle();
void drawAPScreen(const String &ssid, const String &ip);
void drawConnectingScreen(const String &ssid);
