#include "Logger.h"
#include <SD.h>

static const char *LOG_PATH   = "/readings.csv";
static bool        _loggerReady = false;

const char *readingsLogPath() { return LOG_PATH; }
bool        loggerReady()     { return _loggerReady; }

bool initLogger(bool sdReady) {
    _loggerReady = false;
    if (!sdReady) {
        Serial.println("Logger: SD not ready");
        return false;
    }

    bool needsHeader = !SD.exists(LOG_PATH);
    File file = SD.open(LOG_PATH, FILE_APPEND);
    if (!file) {
        Serial.println("Logger: open FAILED");
        return false;
    }

    if (needsHeader || file.size() == 0) {
        file.println("time,rtc_ok,ens_status,aht_ok,temp_c,humidity_pct,aqi,tvoc_ppb,eco2_ppm");
    }
    file.close();

    _loggerReady = true;
    Serial.printf("Logger: OK %s\n", LOG_PATH);
    return true;
}

void logReading(const SensorData &sensor) {
    if (!_loggerReady) return;

    File file = SD.open(LOG_PATH, FILE_APPEND);
    if (!file) {
        Serial.println("Logger: write FAILED");
        _loggerReady = false;
        return;
    }

    file.printf("\"%s\",%d,%d,%d,%.2f,%.2f,%u,%u,%u\n",
                sensor.timeStr,
                sensor.rtc_ok    ? 1 : 0,
                sensor.ens_status,
                sensor.aht_ok    ? 1 : 0,
                sensor.temperature,
                sensor.humidity,
                sensor.aqi,
                sensor.tvoc,
                sensor.eco2);
    file.close();
}
