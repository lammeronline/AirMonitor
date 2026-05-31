#pragma once
#include "Sensors.h"

namespace Telegram {
    void begin(const SensorData *sensor);
    void handle();
    bool sendMessage(const String &text);
}
