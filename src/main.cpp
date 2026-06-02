#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include "Config.h"
#include "Sensors.h"
#include "SDCard.h"
#include "Led.h"
#include "Logger.h"
#include "API.h"
#include "UI.h"
#include "RuntimeSettings.h"
#include "Telegram.h"
#include "MQTT.h"
#include "Version.h"

SensorData currentData;

static const unsigned long SENSOR_INTERVAL_MS = 1000;
static const unsigned long DATA_LOG_INTERVAL_MS = DATA_LOG_INTERVAL_SEC * 1000UL;
static unsigned long lastLogWrite       = 0;
static unsigned long lastSensorUpdate   = 0;
static unsigned long lastNtpSync        = 0;
static unsigned long wifiDownSince      = 0;
static bool          sdReady            = false;
static uint64_t      sdSizeMb           = 0;
static bool          _apMode            = false;
static DNSServer     _dnsServer;

static UiStatus currentUiStatus() {
    UiStatus st;
    st.sdReady        = sdReady;
    st.sdSizeMb       = sdSizeMb;
    st.lastLogWriteMs = lastLogWrite;
    return st;
}

static bool connectWiFi() {
    String ssid = RuntimeSettings::wifiSsid();
    if (ssid.isEmpty()) return false;

    WiFi.persistent(false);
    WiFi.setHostname(RuntimeSettings::hostname().c_str());
    WiFi.mode(WIFI_STA);

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

void setup() {
    Serial.begin(115200);
    RuntimeSettings::begin();

    initLED();
    setLED(0, 0, 255);  // blue = booting

    initUI();
    drawBootTitle();

    // ── Sensors ───────────────────────────────────────────────────────────────
    initSensors();
    updateSensors(currentData);

    drawBootLine("RTC:",    currentData.rtc_ok ? "OK" : "FAIL", currentData.rtc_ok);
    drawBootLine("AHT21:",  currentData.aht_ok ? "OK" : "FAIL", currentData.aht_ok);
    drawBootLine("ENS160:", currentData.ens_ok ? "OK" : "FAIL", currentData.ens_ok);

    // ── SD card ───────────────────────────────────────────────────────────────
    sdSizeMb = initSDCard();
    sdReady  = sdSizeMb > 0;
    initLogger(sdReady);

    char sdBuf[16];
    if (sdSizeMb > 0) snprintf(sdBuf, sizeof(sdBuf), "%llu MB", sdSizeMb);
    else              snprintf(sdBuf, sizeof(sdBuf), "FAIL");
    drawBootLine("SD:", sdBuf, sdReady);

    // ── WiFi ──────────────────────────────────────────────────────────────────
    bool wifiOk = connectWiFi();

    if (!wifiOk) {
        // AP mode
        String apSsid = String(DEVICE_BRAND) + "-" + RuntimeSettings::hostname();
        String apIpStr = RuntimeSettings::apIp();
        IPAddress apIpAddr, apSubnet(255, 255, 255, 0);
        if (!apIpAddr.fromString(apIpStr)) apIpAddr = IPAddress(192, 168, 4, 1);

        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(apIpAddr, apIpAddr, apSubnet);
        WiFi.softAP(apSsid.c_str());
        _apMode = true;
        setApiApMode(true);
        _dnsServer.start(53, "*", apIpAddr);

        setLED(255, 128, 0);   // orange = AP mode

        initAPI(&currentData, sdReady);
        drawAPScreen(apSsid, apIpStr);
        return;
    }

    drawBootLine("WiFi:", WiFi.localIP().toString().c_str(), true);

    // ── NTP ───────────────────────────────────────────────────────────────────
    bool ntpOk = false;
    if (RuntimeSettings::ntpEnabled() && RuntimeSettings::ntpSyncOnBoot()) {
        ntpOk = syncRTCfromNTP();
    }
    drawBootLine("NTP:", ntpOk ? "OK" : (RuntimeSettings::ntpEnabled() ? "FAIL" : "off"), ntpOk || !RuntimeSettings::ntpEnabled());
    lastNtpSync = millis();

    // ── Final init ────────────────────────────────────────────────────────────
    updateSensors(currentData);
    initAPI(&currentData, sdReady);
    Telegram::begin(&currentData);
    MQTT::begin(&currentData);

    if (sdReady && currentData.rtc_ok) {
        preloadHistoryFromSD();
    }

    if (currentData.ens_status == 0) {
        logReading(currentData);
    }
    lastLogWrite     = millis();
    lastSensorUpdate = millis();

    bool allOk = currentData.rtc_ok && currentData.aht_ok && currentData.ens_ok;
    setLED(allOk ? 0 : 255, allOk ? 255 : 0, 0);

    delay(1500);
    invalidateUI();
    drawUI(currentData, currentUiStatus());
    offLED();
}

void loop() {
    // Pending WiFi restart (AP mode save)
    if (apiPendingRestart()) {
        drawConnectingScreen(RuntimeSettings::wifiSsid());
        delay(800);
        ESP.restart();
    }

    // AP mode: only DNS + web server
    if (_apMode) {
        _dnsServer.processNextRequest();
        handleAPI();
        delay(10);
        return;
    }

    unsigned long now = millis();

    handleAPI();
    MQTT::handle();

    // WiFi watchdog — reconnect if disconnected for over 60 s
    if (WiFi.status() != WL_CONNECTED) {
        if (wifiDownSince == 0) wifiDownSince = now;
        else if (now - wifiDownSince >= 60000UL) {
            Serial.println("WiFi: lost >60s, reconnecting");
            WiFi.reconnect();
            wifiDownSince = now;
        }
    } else {
        wifiDownSince = 0;
    }

    // Periodic NTP re-sync
    uint8_t ntpIntervalH = RuntimeSettings::ntpSyncIntervalH();
    if (ntpIntervalH > 0 && RuntimeSettings::ntpEnabled()) {
        unsigned long ntpIntervalMs = (unsigned long)ntpIntervalH * 3600000UL;
        if (now - lastNtpSync >= ntpIntervalMs) {
            syncRTCfromNTP();
            lastNtpSync = now;
        }
    }

    // Page rotation check
    if (handleUI()) {
        drawUI(currentData, currentUiStatus());
    }

    // Sensor polling @ 1 Hz
    if (now - lastSensorUpdate >= SENSOR_INTERVAL_MS) {
        updateSensors(currentData);
        updateLED(currentData.temperature, currentData.humidity,
                  currentData.aqi, currentData.aht_ok && currentData.ens_ok);
        drawUI(currentData, currentUiStatus());
        lastSensorUpdate = now;
    }

    // CSV logging — only when ENS160 reports normal operation
    if (now - lastLogWrite >= DATA_LOG_INTERVAL_MS) {
        if (currentData.ens_status == 0) {
            logReading(currentData);
        }
        lastLogWrite = now;
    }

    delay(10);
}
