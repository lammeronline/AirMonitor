#include "Sensors.h"
#include "Config.h"
#include "RuntimeSettings.h"
#include <Wire.h>
#include <RTClib.h>
#include <SparkFun_ENS160.h>
#include <SparkFun_Qwiic_Humidity_AHT20.h>
#include <WiFi.h>
#include <time.h>

static RTC_DS3231      rtc;
static AHT20           _aht;
static SparkFun_ENS160 _ens;

static bool _rtc_found = false;
static bool _aht_found = false;
static bool _ens_found = false;

void initSensors() {
    Wire.begin(I2C_SDA, I2C_SCL);

    // RTC DS3231
    if (!rtc.begin(&Wire)) {
        Serial.println("RTC: FAILED");
    } else {
        _rtc_found = true;
        Serial.println("RTC: OK");
        if (rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    // AHT20/AHT21
    if (!_aht.begin(Wire)) {
        Serial.println("AHT21: FAILED");
    } else {
        _aht_found = true;
        Serial.println("AHT21: OK");
    }

    // ENS160 — try 0x53 first, then 0x52
    if (_ens.begin(Wire, ENS160_ADDR_HIGH)) {
        _ens_found = true;
        Serial.println("ENS160: OK addr=0x53");
    } else if (_ens.begin(Wire, ENS160_ADDR_LOW)) {
        _ens_found = true;
        Serial.println("ENS160: OK addr=0x52");
    } else {
        Serial.println("ENS160: FAILED both addresses");
    }

    if (_ens_found) {
        _ens.setOperatingMode(SFE_ENS160_STANDARD);
    }
}

void resetENS() {
    if (!_ens_found) return;
    _ens.setOperatingMode(SFE_ENS160_RESET);
    delay(100);
    _ens.setOperatingMode(SFE_ENS160_STANDARD);
    Serial.println("ENS160: reset");
}

bool connectWiFi() {
    String ssid = RuntimeSettings::wifiSsid();
    if (ssid.isEmpty()) return false;

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(RuntimeSettings::hostname().c_str());

    if (RuntimeSettings::staticIpEnabled()) {
        IPAddress ip, gw, sn, dns;
        if (ip.fromString(RuntimeSettings::staticIp()) &&
            gw.fromString(RuntimeSettings::staticGateway()) &&
            sn.fromString(RuntimeSettings::staticSubnet())) {
            dns.fromString(RuntimeSettings::staticDns());
            WiFi.config(ip, gw, sn, dns);
        }
    } else {
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }

    WiFi.begin(ssid.c_str(), RuntimeSettings::wifiPassword().c_str());
    WiFi.setAutoReconnect(true);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        attempts++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(true);
        Serial.println("WiFi: FAILED");
        return false;
    }
    Serial.printf("WiFi: OK %s\n", WiFi.localIP().toString().c_str());
    return true;
}

bool syncRTCfromNTP() {
    if (WiFi.status() != WL_CONNECTED) return false;

    String ntpServer = RuntimeSettings::ntpServer();
    configTime(RuntimeSettings::ntpOffsetSec(), 0, ntpServer.c_str());

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 2000)) {
        Serial.println("NTP: time FAILED");
        return false;
    }

    if (_rtc_found) {
        rtc.adjust(DateTime(
            timeinfo.tm_year + 1900,
            timeinfo.tm_mon + 1,
            timeinfo.tm_mday,
            timeinfo.tm_hour,
            timeinfo.tm_min,
            timeinfo.tm_sec
        ));
    }

    Serial.println("NTP: OK");
    return true;
}

uint32_t getRTCUnixTime() {
    if (!_rtc_found) return 0;
    return rtc.now().unixtime();
}

void updateSensors(SensorData &data) {
    data.rtc_ok = _rtc_found;
    data.aht_ok = false;
    data.ens_ok = false;
    data.ens_status = 3;

    // RTC — time string
    if (_rtc_found) {
        DateTime now = rtc.now();
        snprintf(data.timeStr, sizeof(data.timeStr), "%02d.%02d.%04d  %02d:%02d:%02d",
                 now.day(), now.month(), now.year(),
                 now.hour(), now.minute(), now.second());
        data.weekday = now.dayOfTheWeek();
    } else {
        strncpy(data.timeStr, "--.--.----  --:--:--", sizeof(data.timeStr));
        data.weekday = 0;
    }

    // AHT20/AHT21 — temperature + humidity with calibration offsets
    if (_aht_found) {
        data.aht_ok = true;
        data.temperature = _aht.getTemperature() + RuntimeSettings::ahtTempOffset();
        data.humidity    = _aht.getHumidity()    + RuntimeSettings::ahtHumOffset();
        data.humidity    = constrain(data.humidity, 0.0f, 100.0f);
    }

    // ENS160 — AQI / TVOC / eCO2
    if (_ens_found) {
        if (data.aht_ok) {
            _ens.setTempCompensationCelsius(data.temperature);
            _ens.setRHCompensationFloat(data.humidity);
        }

        data.ens_ok = true;
        // Validity: 0=Normal 1=Warmup 2=InitStartup 3=NoValidOutput
        data.ens_status = _ens.getFlags();

        if (data.ens_status == 0) {
            data.aqi  = _ens.getAQI();
            data.tvoc = _ens.getTVOC();
            data.eco2 = _ens.getECO2();
        }
    }
}
