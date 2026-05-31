#include "MQTT.h"
#include "RuntimeSettings.h"
#include "Version.h"
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <Arduino.h>

namespace MQTT {

static const SensorData *_sensor = nullptr;
static WiFiClient        _wifiClient;
static PubSubClient      _client(_wifiClient);
static unsigned long     _lastPublish   = 0;
static unsigned long     _lastReconnect = 0;

static String topic(const char *suffix) {
    return RuntimeSettings::mqttPrefix() + "/" + RuntimeSettings::hostname() + "/" + suffix;
}

static void pub(const char *suffix, const String &value, bool retain = true) {
    _client.publish(topic(suffix).c_str(), value.c_str(), retain);
}

static void onMessage(const char * /*top*/, byte *payload, unsigned int len) {
    char buf[32];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, payload, n); buf[n] = '\0';
    if (strcmp(buf, "reboot") == 0) ESP.restart();
}

// ── HA MQTT Discovery ─────────────────────────────────────────────────────────

static void pubDiscovery(const char *sensor_id, const char *name,
                         const char *state_suffix, const char *unit,
                         const char *device_class, const char *icon = nullptr) {
    String uid       = RuntimeSettings::hostname() + "_" + sensor_id;
    String cfg_topic = "homeassistant/sensor/" + uid + "/config";

    JsonDocument doc;
    doc["name"]        = name;
    doc["unique_id"]   = uid;
    doc["state_topic"] = topic(state_suffix);
    doc["state_class"] = "measurement";
    if (unit && *unit)                 doc["unit_of_measurement"] = unit;
    if (device_class && *device_class) doc["device_class"]        = device_class;
    if (icon && *icon)                 doc["icon"]                = icon;

    JsonObject dev        = doc["device"].to<JsonObject>();
    dev["identifiers"][0] = RuntimeSettings::hostname();
    dev["name"]           = RuntimeSettings::deviceName();
    dev["model"]          = DEVICE_BRAND;
    dev["sw_version"]     = FW_VERSION;

    String payload;
    serializeJson(doc, payload);
    _client.publish(cfg_topic.c_str(), payload.c_str(), true);
}

static void publishDiscovery() {
    pubDiscovery("temperature", "Temperature",  "temperature", "\xc2\xb0""C", "temperature",                   nullptr);
    pubDiscovery("humidity",    "Humidity",     "humidity",    "%",            "humidity",                      nullptr);
    pubDiscovery("eco2",        "eCO2",         "eco2",        "ppm",          "carbon_dioxide",                nullptr);
    pubDiscovery("tvoc",        "TVOC",         "tvoc",        "ppb",          "volatile_organic_compounds_parts", nullptr);
    pubDiscovery("aqi",         "AQI",          "aqi",         nullptr,        nullptr,                         "mdi:air-filter");
}

static bool tryConnect() {
    String broker = RuntimeSettings::mqttBroker();
    uint16_t port = RuntimeSettings::mqttPort();
    _client.setServer(broker.c_str(), port);
    _client.setCallback(onMessage);

    String clientId = "airmonitor-" + RuntimeSettings::hostname();
    String user     = RuntimeSettings::mqttUser();
    String pass     = RuntimeSettings::mqttPassword();
    bool ok = user.isEmpty()
        ? _client.connect(clientId.c_str())
        : _client.connect(clientId.c_str(), user.c_str(), pass.c_str());

    if (ok) {
        _client.subscribe(topic("cmd").c_str());
        publishDiscovery();
        Serial.printf("MQTT: connected %s:%u\n", broker.c_str(), port);
    } else {
        Serial.printf("MQTT: failed rc=%d\n", _client.state());
    }
    return ok;
}

static void publishAll() {
    if (!_sensor) return;

    if (_sensor->aht_ok) {
        pub("temperature", String(_sensor->temperature, 1));
        pub("humidity",    String(_sensor->humidity, 1));
    }

    // Only publish ENS data when sensor is in normal operation
    if (_sensor->ens_ok && _sensor->ens_status == 0 && _sensor->aqi > 0) {
        pub("aqi",  String(_sensor->aqi));
        pub("tvoc", String(_sensor->tvoc));
        pub("eco2", String(_sensor->eco2));
    }

    // JSON state topic
    JsonDocument doc;
    doc["rtc_ok"]     = _sensor->rtc_ok;
    doc["aht_ok"]     = _sensor->aht_ok;
    doc["ens_ok"]     = _sensor->ens_ok;
    doc["ens_status"] = _sensor->ens_status;
    if (_sensor->aht_ok) {
        doc["temperature"] = serialized(String(_sensor->temperature, 1));
        doc["humidity"]    = serialized(String(_sensor->humidity, 1));
    }
    if (_sensor->ens_ok && _sensor->ens_status == 0 && _sensor->aqi > 0) {
        doc["aqi"]  = _sensor->aqi;
        doc["tvoc"] = _sensor->tvoc;
        doc["eco2"] = _sensor->eco2;
    }
    String state; serializeJson(doc, state);
    pub("state", state);
}

bool connected() { return _client.connected(); }

void begin(const SensorData *sensor) {
    _sensor = sensor;
    _client.setBufferSize(512);
}

void handle() {
    if (!RuntimeSettings::mqttEnabled()) return;
    if (RuntimeSettings::mqttBroker().isEmpty()) return;

    if (!_client.connected()) {
        unsigned long now = millis();
        if (now - _lastReconnect >= 15000UL) {
            _lastReconnect = now;
            tryConnect();
        }
        return;
    }

    _client.loop();

    unsigned long now = millis();
    unsigned long interval = (unsigned long)RuntimeSettings::mqttIntervalSec() * 1000UL;
    if (now - _lastPublish >= interval) {
        _lastPublish = now;
        publishAll();
    }
}

} // namespace MQTT
