#pragma once
#include <Arduino.h>

namespace RuntimeSettings {
    void begin();
    void reload();

    // WiFi
    String wifiSsid();
    String wifiPassword();
    void   saveWifi(const String &ssid, const String &password);

    // NTP
    String  ntpServer();
    long    ntpOffsetSec();
    bool    ntpEnabled();
    bool    ntpSyncOnBoot();
    uint8_t ntpSyncIntervalH();
    void    saveNtp(const String &server, long offsetSec);
    void    saveNtpEnabled(bool enabled);
    void    saveNtpSyncOnBoot(bool enabled);
    void    saveNtpSyncIntervalH(uint8_t hours);

    // Device identity
    String deviceName();
    String hostname();
    void   saveDeviceIdentity(const String &deviceName, const String &hostname);

    // LED
    uint8_t ledMode();
    void    saveLedMode(uint8_t mode);

    // AHT21 calibration offsets
    float ahtTempOffset();
    float ahtHumOffset();
    void  saveAhtCalibration(float tempOffset, float humOffset);

    // Telegram
    bool     tgEnabled();
    String   tgToken();
    String   tgChatId();
    bool     tgTempHiEn();
    float    tgTempHi();
    bool     tgTempLoEn();
    float    tgTempLo();
    bool     tgHumHiEn();
    float    tgHumHi();
    bool     tgHumLoEn();
    float    tgHumLo();
    bool     tgAqiHiEn();
    uint8_t  tgAqiHi();
    uint16_t tgCooldownMin();
    void     saveTgEnabled(bool enabled);
    void     saveTgCredentials(const String &token, const String &chatId);
    void     saveTgThresholds(bool tempHiEn, float tempHi,
                              bool tempLoEn, float tempLo,
                              bool humHiEn,  float humHi,
                              bool humLoEn,  float humLo,
                              bool aqiHiEn,  uint8_t aqiHi,
                              uint16_t cooldownMin);

    // MQTT
    bool     mqttEnabled();
    String   mqttBroker();
    uint16_t mqttPort();
    String   mqttUser();
    String   mqttPassword();
    String   mqttPrefix();
    uint16_t mqttIntervalSec();
    void     saveMqttEnabled(bool enabled);
    void     saveMqttSettings(const String &broker, uint16_t port,
                              const String &user, const String &password,
                              const String &prefix, uint16_t intervalSec);

    // Static IP
    bool   staticIpEnabled();
    String staticIp();
    String staticGateway();
    String staticSubnet();
    String staticDns();
    String apIp();
    void   saveIpSettings(bool staticEnabled, const String &ip, const String &gw,
                          const String &sn, const String &dns);
    void   saveApIp(const String &ip);
}
