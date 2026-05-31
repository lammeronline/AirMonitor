#pragma once
#include <Arduino.h>
#include "Sensors.h"

void initAPI(const SensorData *sensor, bool sdReady);
void handleAPI();
bool apiReady();
bool apiPendingRestart();
void setApiApMode(bool v);
void preloadHistoryFromSD();
