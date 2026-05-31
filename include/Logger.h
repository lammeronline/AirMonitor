#pragma once
#include <Arduino.h>
#include "Sensors.h"

bool        initLogger(bool sdReady);
bool        loggerReady();
void        logReading(const SensorData &sensor);
const char *readingsLogPath();
