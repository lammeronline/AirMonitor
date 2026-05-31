#include "API.h"
#include "Config.h"
#include "Version.h"
#include "Logger.h"
#include "RuntimeSettings.h"
#include "Telegram.h"
#include "MQTT.h"
#include "UI.h"
#include "WebUI.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SD.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <time.h>

static WebServer         server(80);
static const SensorData *_sensor    = nullptr;
static bool              _apiReady  = false;
static bool              _sdReady   = false;
static bool              _otaFailed = false;
static String            _otaMessage;
static bool              _pendingRestart = false;
static bool              _isApMode       = false;
static uint64_t          _sdTotalMb = 0;
static uint64_t          _sdUsedMb  = 0;

bool apiPendingRestart() { return _pendingRestart; }
void setApiApMode(bool v) { _isApMode = v; }
bool apiReady()           { return _apiReady; }

// ── History rings ─────────────────────────────────────────────────────────────

struct HistoryPoint {
    uint32_t ts          = 0;
    float    temperature = 0.0f;
    float    humidity    = 0.0f;
    float    aqi         = 0.0f;
    float    tvoc        = 0.0f;
    float    eco2        = 0.0f;
};

template <size_t N>
class HistoryRing {
public:
    void push(const HistoryPoint &p) {
        _buf[_head] = p;
        _head = (_head + 1) % N;
        if (_count < N) _count++;
    }
    size_t size() const { return _count; }
    HistoryPoint at(size_t i) const {
        return _buf[(_head + N - _count + i) % N];
    }
    void clear() { _head = 0; _count = 0; }
private:
    HistoryPoint _buf[N];
    size_t _head = 0, _count = 0;
};

struct HistoryAccumulator {
    bool     active      = false;
    uint32_t bucketStart = 0;
    uint16_t count       = 0;
    float    tempSum     = 0.0f;
    float    humSum      = 0.0f;
    float    aqiSum      = 0.0f;
    float    tvocSum     = 0.0f;
    float    eco2Sum     = 0.0f;
};

static HistoryRing<288> _hist24;
static HistoryRing<168> _hist7;
static HistoryRing<720> _hist30;
static HistoryAccumulator _acc24, _acc7, _acc30;
static uint32_t _hist24Rev = 0, _hist7Rev = 0, _hist30Rev = 0;

static HistoryPoint makePoint(uint32_t ts, const SensorData &s) {
    HistoryPoint p;
    p.ts          = ts;
    p.temperature = s.temperature;
    p.humidity    = s.humidity;
    p.aqi         = (float)s.aqi;
    p.tvoc        = (float)s.tvoc;
    p.eco2        = (float)s.eco2;
    return p;
}

template <size_t N>
static void flushBucket(HistoryAccumulator &acc, HistoryRing<N> &ring, uint32_t &rev) {
    if (!acc.active || acc.count == 0) return;
    HistoryPoint p;
    p.ts          = acc.bucketStart;
    p.temperature = acc.tempSum  / acc.count;
    p.humidity    = acc.humSum   / acc.count;
    p.aqi         = acc.aqiSum   / acc.count;
    p.tvoc        = acc.tvocSum  / acc.count;
    p.eco2        = acc.eco2Sum  / acc.count;
    ring.push(p);
    rev++;
}

template <size_t N>
static void updateBucket(HistoryAccumulator &acc, HistoryRing<N> &ring, uint32_t &rev,
                         uint32_t bucketSec, uint32_t nowSec, const SensorData &s) {
    // Only accumulate when ENS160 is in normal operation
    if (!s.aht_ok || s.ens_status != 0 || bucketSec == 0) return;

    uint32_t bucketStart = nowSec - (nowSec % bucketSec);
    if (!acc.active) {
        acc.active      = true;
        acc.bucketStart = bucketStart;
        acc.count       = 1;
        acc.tempSum = s.temperature; acc.humSum  = s.humidity;
        acc.aqiSum  = s.aqi;         acc.tvocSum = s.tvoc;
        acc.eco2Sum = s.eco2;
        ring.push(makePoint(nowSec, s)); rev++;
        return;
    }
    if (acc.bucketStart != bucketStart) {
        flushBucket(acc, ring, rev);
        acc.bucketStart = bucketStart;
        acc.count       = 1;
        acc.tempSum = s.temperature; acc.humSum  = s.humidity;
        acc.aqiSum  = s.aqi;         acc.tvocSum = s.tvoc;
        acc.eco2Sum = s.eco2;
        return;
    }
    acc.count++;
    acc.tempSum += s.temperature; acc.humSum  += s.humidity;
    acc.aqiSum  += s.aqi;         acc.tvocSum += s.tvoc;
    acc.eco2Sum += s.eco2;
}

static void updateHistory() {
    if (!_sensor) return;
    static unsigned long lastSampleMs = 0;
    unsigned long nowMs = millis();
    if (nowMs - lastSampleMs < 1000UL) return;
    lastSampleMs = nowMs;

    uint32_t nowSec = getRTCUnixTime();
    if (nowSec == 0) nowSec = millis() / 1000UL;
    updateBucket(_acc24, _hist24, _hist24Rev, 5UL * 60,       nowSec, *_sensor);
    updateBucket(_acc7,  _hist7,  _hist7Rev,  60UL * 60,      nowSec, *_sensor);
    updateBucket(_acc30, _hist30, _hist30Rev, 6UL * 60 * 60,  nowSec, *_sensor);
}

void preloadHistoryFromSD() {
    const char *path = "/readings.csv";
    if (!SD.exists(path)) return;
    File f = SD.open(path, FILE_READ);
    if (!f) return;

    uint32_t rtcNow = getRTCUnixTime();
    if (rtcNow == 0) { f.close(); return; }

    const size_t READ_WINDOW = 3500UL * 1024UL;
    size_t fileSize = f.size();
    if (fileSize > READ_WINDOW + 256) {
        f.seek(fileSize - READ_WINDOW);
        f.readStringUntil('\n');
    } else {
        f.readStringUntil('\n');  // skip header
    }

    uint32_t next24 = 0, next7 = 0, next30 = 0;
    int loaded = 0;
    char buf[160];

    while (f.available()) {
        int len = f.readBytesUntil('\n', buf, sizeof(buf) - 1);
        if (len < 20) continue;
        buf[len] = '\0';

        // Format: "DD.MM.YYYY  HH:MM:SS",rtc_ok,ens_status,aht_ok,temp,hum,aqi,tvoc,eco2
        int dd, mm, yy, hh, mi, ss, rtc_ok, ens_status, aht_ok;
        float temp, hum;
        unsigned int aqi, tvoc, eco2;

        if (sscanf(buf, "\"%d.%d.%d  %d:%d:%d\",%d,%d,%d,%f,%f,%u,%u,%u",
                   &dd, &mm, &yy, &hh, &mi, &ss,
                   &rtc_ok, &ens_status, &aht_ok,
                   &temp, &hum, &aqi, &tvoc, &eco2) != 14) continue;

        if (!aht_ok || ens_status != 0 || yy < 2020 || mm < 1 || mm > 12) continue;

        int a   = (14 - mm) / 12;
        int y   = yy + 4800 - a;
        int m   = mm + 12 * a - 3;
        uint32_t jdn  = dd + (153 * m + 2) / 5 + 365UL * y + y / 4 - y / 100 + y / 400 - 32045;
        uint32_t rowUnix = (jdn - 2440588UL) * 86400UL + hh * 3600UL + mi * 60UL + ss;

        if (rowUnix > rtcNow) continue;
        uint32_t age = rtcNow - rowUnix;
        if (age > 30UL * 24 * 3600) continue;

        HistoryPoint p;
        p.ts = rowUnix; p.temperature = temp; p.humidity = hum;
        p.aqi = aqi;    p.tvoc = tvoc;        p.eco2 = eco2;

        if (age <= 24UL * 3600 && rowUnix >= next24) {
            _hist24.push(p); _hist24Rev++; next24 = rowUnix + 5 * 60;
        }
        if (age <= 7UL * 24 * 3600 && rowUnix >= next7) {
            _hist7.push(p); _hist7Rev++; next7 = rowUnix + 60 * 60;
        }
        if (rowUnix >= next30) {
            _hist30.push(p); _hist30Rev++; next30 = rowUnix + 6 * 60 * 60;
        }
        loaded++;
    }
    f.close();
    Serial.printf("History preload: %d points\n", loaded);
}

// ── History JSON ──────────────────────────────────────────────────────────────
// Streams the response through a 256-byte stack buffer — no heap allocation for
// the JSON payload itself, avoiding fragmentation from ~1800 temporary Strings.
template <size_t N>
static void sendHistoryJson(const HistoryRing<N> &ring, const char *range) {
    const size_t total  = ring.size();
    const size_t maxPts = 360;
    const size_t step   = total > maxPts ? (total + maxPts - 1) / maxPts : 1;
    const size_t pts    = (total + step - 1) / step;

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");

    char   buf[256];
    size_t pos = 0;

    auto flush = [&]() {
        if (pos) { server.sendContent(buf, pos); pos = 0; }
    };
    auto cat = [&](const char *s, size_t n) {
        if (pos + n >= sizeof(buf)) flush();
        memcpy(buf + pos, s, n); pos += n;
    };
    auto catS = [&](const char *s) { cat(s, strlen(s)); };

    char tmp[32]; size_t n;

    n = (size_t)snprintf(tmp, sizeof(tmp), "{\"range\":\"%s\",\"n\":%u,\"ts\":[",
                         range, (unsigned)pts);
    cat(tmp, n);
    for (size_t i = 0; i < total; i += step) {
        n = (size_t)snprintf(tmp, sizeof(tmp), i ? ",%lu" : "%lu",
                             (unsigned long)ring.at(i).ts);
        cat(tmp, n);
    }
    catS("],\"temperature\":[");
    for (size_t i = 0; i < total; i += step) {
        n = (size_t)snprintf(tmp, sizeof(tmp), i ? ",%.1f" : "%.1f",
                             ring.at(i).temperature);
        cat(tmp, n);
    }
    catS("],\"humidity\":[");
    for (size_t i = 0; i < total; i += step) {
        n = (size_t)snprintf(tmp, sizeof(tmp), i ? ",%.1f" : "%.1f",
                             ring.at(i).humidity);
        cat(tmp, n);
    }
    catS("],\"aqi\":[");
    for (size_t i = 0; i < total; i += step) {
        n = (size_t)snprintf(tmp, sizeof(tmp), i ? ",%.1f" : "%.1f",
                             ring.at(i).aqi);
        cat(tmp, n);
    }
    catS("],\"tvoc\":[");
    for (size_t i = 0; i < total; i += step) {
        n = (size_t)snprintf(tmp, sizeof(tmp), i ? ",%.0f" : "%.0f",
                             ring.at(i).tvoc);
        cat(tmp, n);
    }
    catS("],\"eco2\":[");
    for (size_t i = 0; i < total; i += step) {
        n = (size_t)snprintf(tmp, sizeof(tmp), i ? ",%.0f" : "%.0f",
                             ring.at(i).eco2);
        cat(tmp, n);
    }
    catS("]}");
    flush();
}

// ── Helpers ───────────────────────────────────────────────────────────────────
static void sendJson(JsonDocument &doc) {
    String out;
    out.reserve(measureJson(doc) + 1);  // exact size — no reallocation
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static String jsonEsc(const String &t) {
    String o; o.reserve(t.length() + 8);
    for (size_t i = 0; i < t.length(); i++) {
        char c = t[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o;
}

// ── Handlers ──────────────────────────────────────────────────────────────────
static void handleStatus() {
    if (!_sensor) {
        server.send(503, "application/json", "{\"ok\":false,\"error\":\"not ready\"}");
        return;
    }
    JsonDocument doc;
    doc["ok"]          = true;
    doc["fw_version"]  = FW_VERSION;
    doc["device"]      = RuntimeSettings::deviceName();
    doc["ip"]          = WiFi.localIP().toString();
    doc["hostname"]    = RuntimeSettings::hostname() + ".local";
    doc["log_path"]    = readingsLogPath();
    doc["logger_ready"]= loggerReady();

    JsonObject sys = doc["system"].to<JsonObject>();
    sys["sd_ready"]            = _sdReady;
    sys["api_ready"]           = _apiReady;
    sys["heap_free"]           = ESP.getFreeHeap();
    sys["uptime_sec"]          = millis() / 1000UL;
    sys["log_interval_sec"]    = DATA_LOG_INTERVAL_SEC;
    sys["hist24_rev"]          = _hist24Rev;
    sys["hist7_rev"]           = _hist7Rev;
    sys["hist30_rev"]          = _hist30Rev;
    sys["hist24_interval_min"] = 5;
    sys["hist7_interval_min"]  = 60;
    sys["hist30_interval_min"] = 360;
    sys["wifi_ssid"]           = RuntimeSettings::wifiSsid();
    sys["ntp_server"]          = RuntimeSettings::ntpServer();
    sys["ntp_offset_sec"]      = RuntimeSettings::ntpOffsetSec();
    sys["ntp_enabled"]         = RuntimeSettings::ntpEnabled();
    sys["ntp_sync_on_boot"]    = RuntimeSettings::ntpSyncOnBoot();
    sys["ntp_sync_interval_h"] = RuntimeSettings::ntpSyncIntervalH();
    sys["led_mode"]            = RuntimeSettings::ledMode();
    sys["sd_total_mb"]         = _sdTotalMb;
    sys["sd_used_mb"]          = _sdUsedMb;
    sys["aht_temp_offset"]     = RuntimeSettings::ahtTempOffset();
    sys["aht_hum_offset"]      = RuntimeSettings::ahtHumOffset();
    sys["oled_enabled"]        = uiEnabled();

    JsonObject sensor = doc["sensor"].to<JsonObject>();
    sensor["rtc_ok"]     = _sensor->rtc_ok;
    sensor["ens_ok"]     = _sensor->ens_ok;
    sensor["aht_ok"]     = _sensor->aht_ok;
    sensor["ens_status"] = _sensor->ens_status;
    sensor["time"]       = _sensor->timeStr;
    sensor["temperature"]= _sensor->temperature;
    sensor["humidity"]   = _sensor->humidity;
    sensor["aqi"]        = _sensor->aqi;
    sensor["tvoc"]       = _sensor->tvoc;
    sensor["eco2"]       = _sensor->eco2;

    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    mqtt["enabled"]      = RuntimeSettings::mqttEnabled();
    mqtt["connected"]    = MQTT::connected();
    mqtt["broker"]       = RuntimeSettings::mqttBroker();
    mqtt["port"]         = RuntimeSettings::mqttPort();
    mqtt["prefix"]       = RuntimeSettings::mqttPrefix();
    mqtt["interval_sec"] = RuntimeSettings::mqttIntervalSec();

    JsonObject tg = doc["telegram"].to<JsonObject>();
    tg["enabled"]      = RuntimeSettings::tgEnabled();
    tg["has_token"]    = RuntimeSettings::tgToken().length() > 0;
    tg["chat_id"]      = RuntimeSettings::tgChatId();
    tg["cooldown_min"] = RuntimeSettings::tgCooldownMin();
    tg["temp_hi_en"]   = RuntimeSettings::tgTempHiEn();
    tg["temp_hi"]      = RuntimeSettings::tgTempHi();
    tg["temp_lo_en"]   = RuntimeSettings::tgTempLoEn();
    tg["temp_lo"]      = RuntimeSettings::tgTempLo();
    tg["hum_hi_en"]    = RuntimeSettings::tgHumHiEn();
    tg["hum_hi"]       = RuntimeSettings::tgHumHi();
    tg["hum_lo_en"]    = RuntimeSettings::tgHumLoEn();
    tg["hum_lo"]       = RuntimeSettings::tgHumLo();
    tg["aqi_hi_en"]    = RuntimeSettings::tgAqiHiEn();
    tg["aqi_hi"]       = RuntimeSettings::tgAqiHi();

    JsonObject ipCfg = doc["ip_config"].to<JsonObject>();
    ipCfg["static_enabled"] = RuntimeSettings::staticIpEnabled();
    ipCfg["static_ip"]      = RuntimeSettings::staticIp();
    ipCfg["static_gw"]      = RuntimeSettings::staticGateway();
    ipCfg["static_sn"]      = RuntimeSettings::staticSubnet();
    ipCfg["static_dns"]     = RuntimeSettings::staticDns();
    ipCfg["ap_ip"]          = RuntimeSettings::apIp();

    sendJson(doc);
}

static void handleLog() {
    if (!_sdReady || !SD.exists(readingsLogPath())) {
        server.send(404, "text/plain", "Log not found");
        return;
    }
    File f = SD.open(readingsLogPath(), FILE_READ);
    if (!f) { server.send(500, "text/plain", "Open failed"); return; }
    server.streamFile(f, "text/csv");
    f.close();
}

static void handleHistory() {
    String range = server.arg("range");
    if (range == "7d")       sendHistoryJson(_hist7,  "7d");
    else if (range == "30d") sendHistoryJson(_hist30, "30d");
    else                     sendHistoryJson(_hist24, "24h");
}

static void handleSettingsGet() {
    JsonDocument doc;
    doc["ok"]             = true;
    doc["wifi_ssid"]      = RuntimeSettings::wifiSsid();
    doc["device"]         = RuntimeSettings::deviceName();
    doc["hostname"]       = RuntimeSettings::hostname();
    doc["ntp_server"]     = RuntimeSettings::ntpServer();
    doc["ntp_offset_sec"] = RuntimeSettings::ntpOffsetSec();
    doc["led_mode"]       = RuntimeSettings::ledMode();
    doc["aht_temp_offset"]= RuntimeSettings::ahtTempOffset();
    doc["aht_hum_offset"] = RuntimeSettings::ahtHumOffset();
    sendJson(doc);
}

static void handleSettingsPost() {
    String body = server.arg("plain");
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
        return;
    }

    bool wifiChanged = false;

    if (doc["wifi_ssid"].is<const char*>()) {
        String ssid = doc["wifi_ssid"].as<String>();
        String pass = doc["wifi_password"] | "";
        ssid.trim();
        if (!ssid.isEmpty()) { RuntimeSettings::saveWifi(ssid, pass); wifiChanged = true; }
    }

    if (doc["ntp_server"].is<const char*>() || doc["ntp_offset_sec"].is<long>()) {
        String srv = doc["ntp_server"] | RuntimeSettings::ntpServer();
        long   off = doc["ntp_offset_sec"] | RuntimeSettings::ntpOffsetSec();
        srv.trim();
        RuntimeSettings::saveNtp(srv, off);
        configTime(RuntimeSettings::ntpOffsetSec(), 0, RuntimeSettings::ntpServer().c_str());
    }

    if (doc["device"].is<const char*>() || doc["hostname"].is<const char*>()) {
        String name = doc["device"]   | RuntimeSettings::deviceName();
        String host = doc["hostname"] | RuntimeSettings::hostname();
        RuntimeSettings::saveDeviceIdentity(name, host);
        String h = RuntimeSettings::hostname();
        WiFi.setHostname(h.c_str());
        MDNS.end(); MDNS.begin(h.c_str()); MDNS.addService("http", "tcp", 80);
    }

    if (doc["led_mode"].is<int>())
        RuntimeSettings::saveLedMode((uint8_t)constrain(doc["led_mode"].as<int>(), 0, 3));

    if (doc["ntp_enabled"].is<bool>())          RuntimeSettings::saveNtpEnabled(doc["ntp_enabled"].as<bool>());
    if (doc["ntp_sync_on_boot"].is<bool>())     RuntimeSettings::saveNtpSyncOnBoot(doc["ntp_sync_on_boot"].as<bool>());
    if (doc["ntp_sync_interval_h"].is<int>())
        RuntimeSettings::saveNtpSyncIntervalH((uint8_t)constrain(doc["ntp_sync_interval_h"].as<int>(), 0, 255));

    if (doc["oled_enabled"].is<bool>())
        enableUI(doc["oled_enabled"].as<bool>());

    if (doc["aht_temp_offset"].is<float>() || doc["aht_hum_offset"].is<float>()) {
        float tOff = doc["aht_temp_offset"] | RuntimeSettings::ahtTempOffset();
        float hOff = doc["aht_hum_offset"]  | RuntimeSettings::ahtHumOffset();
        tOff = constrain(tOff, -10.0f, 10.0f);
        hOff = constrain(hOff, -20.0f, 20.0f);
        RuntimeSettings::saveAhtCalibration(tOff, hOff);
    }

    JsonDocument resp;
    resp["ok"]           = true;
    resp["wifi_changed"] = wifiChanged;
    if (wifiChanged && _isApMode) {
        resp["rebooting"] = true;
        resp["ap_mode"]   = true;
    }
    sendJson(resp);

    if (wifiChanged) {
        if (_isApMode) {
            _pendingRestart = true;
        } else {
            WiFi.disconnect(false, false);
            WiFi.mode(WIFI_STA);
            WiFi.begin(RuntimeSettings::wifiSsid().c_str(), RuntimeSettings::wifiPassword().c_str());
        }
    }
}

static void handleScan() {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) {
        // Not started or previous scan deleted — kick off an async scan
        WiFi.scanNetworks(true, true);
        server.send(200, "application/json", "{\"ok\":false,\"scanning\":true}");
        return;
    }
    if (n == WIFI_SCAN_RUNNING) {
        server.send(200, "application/json", "{\"ok\":false,\"scanning\":true}");
        return;
    }
    // Scan complete — return results then free memory
    String out; out.reserve(n * 64 + 16);
    out = "{\"ok\":true,\"networks\":[";
    for (int i = 0; i < n; i++) {
        if (i) out += ',';
        out += "{\"ssid\":\""; out += jsonEsc(WiFi.SSID(i));
        out += "\",\"rssi\":"; out += WiFi.RSSI(i);
        out += ",\"secure\":"; out += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true");
        out += "}";
    }
    out += "]}";
    WiFi.scanDelete();
    server.send(200, "application/json", out);
}

static void handleReboot() {
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    delay(300); ESP.restart();
}

static bool removePathRecursive(const String &path) {
    File e = SD.open(path);
    if (!e) return false;
    if (!e.isDirectory()) { e.close(); return SD.remove(path); }
    bool ok = true;
    File child = e.openNextFile();
    while (child) {
        String cp = child.path();
        bool isDir = child.isDirectory();
        child.close();
        if (isDir) { if (!removePathRecursive(cp)) ok = false; SD.rmdir(cp); }
        else if (!SD.remove(cp)) ok = false;
        child = e.openNextFile();
    }
    e.close();
    return ok;
}

static void handleHistoryClear() {
    _hist24.clear(); _hist7.clear(); _hist30.clear();
    _acc24 = {}; _acc7 = {}; _acc30 = {};
    _hist24Rev++; _hist7Rev++; _hist30Rev++;
    server.send(200, "application/json", "{\"ok\":true}");
}

void handleSdClear() {
    if (!_sdReady) { server.send(503, "application/json", "{\"ok\":false,\"error\":\"sd not ready\"}"); return; }
    File root = SD.open("/");
    if (!root || !root.isDirectory()) { if (root) root.close(); server.send(500, "application/json", "{\"ok\":false}"); return; }
    bool ok = true;
    File entry = root.openNextFile();
    while (entry) {
        String path = entry.path(); bool isDir = entry.isDirectory(); entry.close();
        if (isDir) { if (!removePathRecursive(path)) ok = false; SD.rmdir(path); }
        else if (!SD.remove(path)) ok = false;
        entry = root.openNextFile();
    }
    root.close();
    initLogger(_sdReady);
    server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"clear failed\"}");
}

static void otaFail(const char *msg) { _otaFailed = true; _otaMessage = msg; Update.abort(); }

static void handleOtaUpload() {
    HTTPUpload &u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
        _otaFailed = false; _otaMessage = "";
        Serial.printf("OTA: start %s\n", u.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) otaFail("ota begin failed");
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (!_otaFailed && Update.write(u.buf, u.currentSize) != u.currentSize) otaFail("ota write failed");
    } else if (u.status == UPLOAD_FILE_END) {
        if (!_otaFailed && !Update.end(true)) otaFail("ota end failed");
        if (!_otaFailed) Serial.printf("OTA: OK %u bytes\n", u.totalSize);
    } else if (u.status == UPLOAD_FILE_ABORTED) {
        otaFail("ota aborted");
    }
}

static void handleOtaDone() {
    if (_otaFailed) {
        server.send(500, "application/json", "{\"ok\":false,\"error\":\"" + jsonEsc(_otaMessage) + "\"}");
        return;
    }
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    delay(500); ESP.restart();
}

static void handleTelegramGet() {
    JsonDocument doc;
    doc["ok"]          = true;
    doc["enabled"]     = RuntimeSettings::tgEnabled();
    doc["has_token"]   = RuntimeSettings::tgToken().length() > 0;
    doc["chat_id"]     = RuntimeSettings::tgChatId();
    doc["cooldown_min"]= RuntimeSettings::tgCooldownMin();
    doc["temp_hi_en"]  = RuntimeSettings::tgTempHiEn();
    doc["temp_hi"]     = RuntimeSettings::tgTempHi();
    doc["temp_lo_en"]  = RuntimeSettings::tgTempLoEn();
    doc["temp_lo"]     = RuntimeSettings::tgTempLo();
    doc["hum_hi_en"]   = RuntimeSettings::tgHumHiEn();
    doc["hum_hi"]      = RuntimeSettings::tgHumHi();
    doc["hum_lo_en"]   = RuntimeSettings::tgHumLoEn();
    doc["hum_lo"]      = RuntimeSettings::tgHumLo();
    doc["aqi_hi_en"]   = RuntimeSettings::tgAqiHiEn();
    doc["aqi_hi"]      = RuntimeSettings::tgAqiHi();
    sendJson(doc);
}

static void handleTelegramPost() {
    String body = server.arg("plain");
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}"); return;
    }
    if (doc["enabled"].is<bool>()) RuntimeSettings::saveTgEnabled(doc["enabled"].as<bool>());
    if (doc["chat_id"].is<const char*>() || doc["token"].is<const char*>()) {
        String token  = doc["token"]   | "";
        String chatId = doc["chat_id"] | RuntimeSettings::tgChatId();
        chatId.trim();
        RuntimeSettings::saveTgCredentials(token, chatId);
    }
    bool threshChanged =
        doc["temp_hi_en"].is<bool>() || doc["temp_hi"].is<float>() ||
        doc["temp_lo_en"].is<bool>() || doc["temp_lo"].is<float>() ||
        doc["hum_hi_en"].is<bool>()  || doc["hum_hi"].is<float>()  ||
        doc["hum_lo_en"].is<bool>()  || doc["hum_lo"].is<float>()  ||
        doc["aqi_hi_en"].is<bool>()  || doc["aqi_hi"].is<int>()    ||
        doc["cooldown_min"].is<int>();
    if (threshChanged) {
        RuntimeSettings::saveTgThresholds(
            doc["temp_hi_en"] | RuntimeSettings::tgTempHiEn(),
            doc["temp_hi"]    | RuntimeSettings::tgTempHi(),
            doc["temp_lo_en"] | RuntimeSettings::tgTempLoEn(),
            doc["temp_lo"]    | RuntimeSettings::tgTempLo(),
            doc["hum_hi_en"]  | RuntimeSettings::tgHumHiEn(),
            doc["hum_hi"]     | RuntimeSettings::tgHumHi(),
            doc["hum_lo_en"]  | RuntimeSettings::tgHumLoEn(),
            doc["hum_lo"]     | RuntimeSettings::tgHumLo(),
            doc["aqi_hi_en"]  | RuntimeSettings::tgAqiHiEn(),
            (uint8_t)(doc["aqi_hi"] | (int)RuntimeSettings::tgAqiHi()),
            (uint16_t)(doc["cooldown_min"] | (int)RuntimeSettings::tgCooldownMin())
        );
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleTelegramTest() {
    if (RuntimeSettings::tgToken().isEmpty() || RuntimeSettings::tgChatId().isEmpty()) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"token or chat_id not set\"}"); return;
    }
    bool ok = Telegram::sendMessage("AirMonitor test message — bot is working.");
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"queue full\"}");
}

static void handleMqttGet() {
    JsonDocument doc;
    doc["ok"]           = true;
    doc["enabled"]      = RuntimeSettings::mqttEnabled();
    doc["connected"]    = MQTT::connected();
    doc["broker"]       = RuntimeSettings::mqttBroker();
    doc["port"]         = RuntimeSettings::mqttPort();
    doc["user"]         = RuntimeSettings::mqttUser();
    doc["prefix"]       = RuntimeSettings::mqttPrefix();
    doc["interval_sec"] = RuntimeSettings::mqttIntervalSec();
    sendJson(doc);
}

static void handleMqttPost() {
    String body = server.arg("plain");
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}"); return;
    }
    if (doc["enabled"].is<bool>()) RuntimeSettings::saveMqttEnabled(doc["enabled"].as<bool>());
    if (doc["broker"].is<const char*>()) {
        RuntimeSettings::saveMqttSettings(
            doc["broker"]       | RuntimeSettings::mqttBroker(),
            (uint16_t)(doc["port"]   | (int)RuntimeSettings::mqttPort()),
            doc["user"]         | RuntimeSettings::mqttUser(),
            doc["password"]     | String(""),
            doc["prefix"]       | RuntimeSettings::mqttPrefix(),
            (uint16_t)(doc["interval_sec"] | (int)RuntimeSettings::mqttIntervalSec())
        );
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleEnsReset() {
    resetENS();
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleFactoryReset() {
    Preferences p; p.begin("airmon", false); p.clear(); p.end();
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    delay(300); ESP.restart();
}

static void handleIpGet() {
    JsonDocument doc;
    doc["ok"]             = true;
    doc["static_enabled"] = RuntimeSettings::staticIpEnabled();
    doc["static_ip"]      = RuntimeSettings::staticIp();
    doc["static_gw"]      = RuntimeSettings::staticGateway();
    doc["static_sn"]      = RuntimeSettings::staticSubnet();
    doc["static_dns"]     = RuntimeSettings::staticDns();
    doc["ap_ip"]          = RuntimeSettings::apIp();
    sendJson(doc);
}

static void handleIpPost() {
    String body = server.arg("plain");
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}"); return;
    }
    if (doc["static_enabled"].is<bool>() || doc["static_ip"].is<const char*>()) {
        RuntimeSettings::saveIpSettings(
            doc["static_enabled"] | RuntimeSettings::staticIpEnabled(),
            doc["static_ip"]      | RuntimeSettings::staticIp(),
            doc["static_gw"]      | RuntimeSettings::staticGateway(),
            doc["static_sn"]      | RuntimeSettings::staticSubnet(),
            doc["static_dns"]     | RuntimeSettings::staticDns()
        );
    }
    if (doc["ap_ip"].is<const char*>()) RuntimeSettings::saveApIp(doc["ap_ip"].as<String>());
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleRoot() {
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8",
                  (const char*)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
}

// ── initAPI ───────────────────────────────────────────────────────────────────
void initAPI(const SensorData *sensor, bool sdReady) {
    _sensor  = sensor;
    _sdReady = sdReady;
    if (sdReady) {
        _sdTotalMb = SD.cardSize()  / (1024ULL * 1024ULL);
        _sdUsedMb  = SD.usedBytes() / (1024ULL * 1024ULL);
    }

    auto cpRedirect = []() {
        String url = "http://" + RuntimeSettings::apIp() + "/";
        server.sendHeader("Location", url, true);
        server.send(302, "text/plain", "");
    };
    server.on("/generate_204",        HTTP_GET, cpRedirect);
    server.on("/hotspot-detect.html", HTTP_GET, cpRedirect);
    server.on("/connecttest.txt",     HTTP_GET, cpRedirect);
    server.on("/ncsi.txt",            HTTP_GET, cpRedirect);
    server.on("/redirect",            HTTP_GET, cpRedirect);
    server.on("/canonical.html",      HTTP_GET, cpRedirect);

    server.on("/",                HTTP_GET,  handleRoot);
    server.on("/api/status",      HTTP_GET,  handleStatus);
    server.on("/api/settings",    HTTP_GET,  handleSettingsGet);
    server.on("/api/settings",    HTTP_POST, handleSettingsPost);
    server.on("/api/scan",        HTTP_GET,  handleScan);
    server.on("/api/history",     HTTP_GET,  handleHistory);
    server.on("/api/log",         HTTP_GET,  handleLog);
    server.on("/api/ota",         HTTP_POST, handleOtaDone, handleOtaUpload);
    server.on("/api/sd/clear",      HTTP_POST, handleSdClear);
    server.on("/api/history/clear", HTTP_POST, handleHistoryClear);
    server.on("/api/reboot",      HTTP_POST, handleReboot);
    server.on("/api/telegram",    HTTP_GET,  handleTelegramGet);
    server.on("/api/telegram",    HTTP_POST, handleTelegramPost);
    server.on("/api/telegram/test", HTTP_POST, handleTelegramTest);
    server.on("/api/ens/reset",     HTTP_POST, handleEnsReset);
    server.on("/api/factory-reset", HTTP_POST, handleFactoryReset);
    server.on("/api/mqtt",        HTTP_GET,  handleMqttGet);
    server.on("/api/mqtt",        HTTP_POST, handleMqttPost);
    server.on("/api/ip",          HTTP_GET,  handleIpGet);
    server.on("/api/ip",          HTTP_POST, handleIpPost);
    server.onNotFound([]() {
        if (WiFi.getMode() == WIFI_MODE_AP) {
            String url = "http://" + RuntimeSettings::apIp() + "/";
            server.sendHeader("Location", url, true);
            server.send(302, "text/plain", "");
        } else {
            server.send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
        }
    });

    String hostname = RuntimeSettings::hostname();
    MDNS.begin(hostname.c_str());
    MDNS.addService("http", "tcp", 80);
    server.begin();
    _apiReady = true;
    Serial.printf("API: OK http://%s.local/ (%s)\n",
                  hostname.c_str(), WiFi.localIP().toString().c_str());
}

void handleAPI() {
    updateHistory();
    if (_apiReady) server.handleClient();
}
