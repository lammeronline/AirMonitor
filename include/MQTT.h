#pragma once
#include "Sensors.h"

namespace MQTT {
    void begin(const SensorData *sensor);
    void handle();
    bool connected();
}
