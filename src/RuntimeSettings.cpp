#include "RuntimeSettings.h"
#include "Config.h"
#include <Preferences.h>

namespace RuntimeSettings {

// ── In-memory cache ──────────────────────────────────────────────────────────
static String   _wifiSsid          = WIFI_SSID;
static String   _wifiPassword      = WIFI_PASSWORD;
static String   _ntpServer         = NTP_SERVER;
static long     _ntpOffsetSec      = NTP_OFFSET;
static bool     _ntpEnabled        = true;
static bool     _ntpSyncOnBoot     = true;
static uint8_t  _ntpSyncIntervalH  = 24;
static String   _deviceName        = DEVICE_NAME;
static String   _hostname          = DEVICE_NAME;
static uint8_t  _ledMode           = 0;
static float    _ahtTempOffset     = 0.0f;
static float    _ahtHumOffset      = 0.0f;

static bool     _tgEnabled         = false;
static String   _tgToken;
static String   _tgChatId;
static bool     _tgTempHiEn        = false;
static float    _tgTempHi          = 30.0f;
static bool     _tgTempLoEn        = false;
static float    _tgTempLo          = 15.0f;
static bool     _tgHumHiEn         = false;
static float    _tgHumHi           = 75.0f;
static bool     _tgHumLoEn         = false;
static float    _tgHumLo           = 30.0f;
static bool     _tgAqiHiEn         = false;
static uint8_t  _tgAqiHi           = 3;
static uint16_t _tgCooldownMin     = 10;

static bool     _mqttEnabled       = false;
static String   _mqttBroker;
static uint16_t _mqttPort          = 1883;
static String   _mqttUser;
static String   _mqttPassword;
static String   _mqttPrefix        = "airmonitor";
static uint16_t _mqttIntervalSec   = 60;

static bool   _staticIpEnabled = false;
static String _staticIp;
static String _staticGateway;
static String _staticSubnet    = "255.255.255.0";
static String _staticDns       = "8.8.8.8";
static String _apIp            = "192.168.4.1";

// ── Validators ───────────────────────────────────────────────────────────────
static long clampOffset(long v) {
    if (v < -12L * 3600) return -12L * 3600;
    if (v >  14L * 3600) return  14L * 3600;
    return v;
}

static String cleanLabel(String v, const char *fallback) {
    v.trim();
    if (v.isEmpty()) return String(fallback);
    if (v.length() > 31) v = v.substring(0, 31);
    return v;
}

static String cleanHostname(String v, const char *fallback) {
    v.toLowerCase(); v.trim();
    String out;
    out.reserve(v.length());
    for (size_t i = 0; i < v.length() && out.length() < 31; i++) {
        char c = v[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') out += c;
        else if (c == '_' || c == ' ' || c == '.') out += '-';
    }
    while (out.startsWith("-")) out.remove(0, 1);
    while (out.endsWith("-"))   out.remove(out.length() - 1);
    return out.length() ? out : String(fallback);
}

// ── Load from NVS ────────────────────────────────────────────────────────────
void reload() {
    Preferences p;
    p.begin("airmon", true);

    _wifiSsid         = p.getString("wifi_ssid",       WIFI_SSID);
    _wifiPassword     = p.getString("wifi_pass",       WIFI_PASSWORD);
    _ntpServer        = p.getString("ntp_server",      NTP_SERVER);
    _ntpOffsetSec     = clampOffset(p.getLong("ntp_offset", NTP_OFFSET));
    _ntpEnabled       = p.getBool("ntp_en",            true);
    _ntpSyncOnBoot    = p.getBool("ntp_boot",          true);
    _ntpSyncIntervalH = (uint8_t)p.getUInt("ntp_intv", 24);
    _deviceName       = cleanLabel(p.getString("dev_name", DEVICE_NAME), DEVICE_NAME);
    _hostname         = cleanHostname(p.getString("hostname", DEVICE_NAME), DEVICE_NAME);
    _ledMode          = (uint8_t)p.getUInt("led_mode", 0);
    _ahtTempOffset    = p.getFloat("aht_t_off",  0.0f);
    _ahtHumOffset     = p.getFloat("aht_h_off",  0.0f);

    _tgEnabled        = p.getBool("tg_en",        false);
    _tgToken          = p.getString("tg_token",   "");
    _tgChatId         = p.getString("tg_chat",    "");
    _tgTempHiEn       = p.getBool("tg_thi_en",   false);
    _tgTempHi         = p.getFloat("tg_thi",      30.0f);
    _tgTempLoEn       = p.getBool("tg_tlo_en",   false);
    _tgTempLo         = p.getFloat("tg_tlo",      15.0f);
    _tgHumHiEn        = p.getBool("tg_hhi_en",   false);
    _tgHumHi          = p.getFloat("tg_hhi",      75.0f);
    _tgHumLoEn        = p.getBool("tg_hlo_en",   false);
    _tgHumLo          = p.getFloat("tg_hlo",      30.0f);
    _tgAqiHiEn        = p.getBool("tg_aqi_en",   false);
    _tgAqiHi          = (uint8_t)p.getUInt("tg_aqi_hi", 3);
    _tgCooldownMin    = (uint16_t)p.getUInt("tg_cool",  10);

    _mqttEnabled      = p.getBool("mqtt_en",      false);
    _mqttBroker       = p.getString("mqtt_broker","");
    _mqttPort         = (uint16_t)p.getUInt("mqtt_port", 1883);
    _mqttUser         = p.getString("mqtt_user",  "");
    _mqttPassword     = p.getString("mqtt_pass",  "");
    _mqttPrefix       = p.getString("mqtt_pfx",   "airmonitor");
    _mqttIntervalSec  = (uint16_t)p.getUInt("mqtt_intv", 60);

    _staticIpEnabled  = p.getBool("sip_en",        false);
    _staticIp         = p.getString("sip_ip",       "");
    _staticGateway    = p.getString("sip_gw",       "");
    _staticSubnet     = p.getString("sip_sn",       "255.255.255.0");
    _staticDns        = p.getString("sip_dns",      "8.8.8.8");
    _apIp             = p.getString("ap_ip",        "192.168.4.1");

    p.end();
}

void begin() { reload(); }

// ── Getters ──────────────────────────────────────────────────────────────────
String   wifiSsid()         { return _wifiSsid; }
String   wifiPassword()     { return _wifiPassword; }
String   ntpServer()        { return _ntpServer; }
long     ntpOffsetSec()     { return _ntpOffsetSec; }
bool     ntpEnabled()       { return _ntpEnabled; }
bool     ntpSyncOnBoot()    { return _ntpSyncOnBoot; }
uint8_t  ntpSyncIntervalH() { return _ntpSyncIntervalH; }
String   deviceName()       { return _deviceName; }
String   hostname()         { return _hostname; }
uint8_t  ledMode()          { return _ledMode; }
float    ahtTempOffset()    { return _ahtTempOffset; }
float    ahtHumOffset()     { return _ahtHumOffset; }

bool     tgEnabled()        { return _tgEnabled; }
String   tgToken()          { return _tgToken; }
String   tgChatId()         { return _tgChatId; }
bool     tgTempHiEn()       { return _tgTempHiEn; }
float    tgTempHi()         { return _tgTempHi; }
bool     tgTempLoEn()       { return _tgTempLoEn; }
float    tgTempLo()         { return _tgTempLo; }
bool     tgHumHiEn()        { return _tgHumHiEn; }
float    tgHumHi()          { return _tgHumHi; }
bool     tgHumLoEn()        { return _tgHumLoEn; }
float    tgHumLo()          { return _tgHumLo; }
bool     tgAqiHiEn()        { return _tgAqiHiEn; }
uint8_t  tgAqiHi()          { return _tgAqiHi; }
uint16_t tgCooldownMin()    { return _tgCooldownMin; }

bool     mqttEnabled()      { return _mqttEnabled; }
String   mqttBroker()       { return _mqttBroker; }
uint16_t mqttPort()         { return _mqttPort; }
String   mqttUser()         { return _mqttUser; }
String   mqttPassword()     { return _mqttPassword; }
String   mqttPrefix()       { return _mqttPrefix; }
uint16_t mqttIntervalSec()  { return _mqttIntervalSec; }

bool   staticIpEnabled() { return _staticIpEnabled; }
String staticIp()        { return _staticIp; }
String staticGateway()   { return _staticGateway; }
String staticSubnet()    { return _staticSubnet; }
String staticDns()       { return _staticDns; }
String apIp()            { return _apIp; }

// ── Setters ──────────────────────────────────────────────────────────────────
void saveWifi(const String &ssid, const String &password) {
    _wifiSsid     = ssid;
    _wifiPassword = password;
    Preferences p; p.begin("airmon", false);
    p.putString("wifi_ssid", ssid);
    p.putString("wifi_pass", password);
    p.end();
}

void saveNtp(const String &server, long offsetSec) {
    _ntpServer    = server;
    _ntpOffsetSec = clampOffset(offsetSec);
    Preferences p; p.begin("airmon", false);
    p.putString("ntp_server", server);
    p.putLong("ntp_offset",   _ntpOffsetSec);
    p.end();
}

void saveNtpEnabled(bool en)           { _ntpEnabled = en;       Preferences p; p.begin("airmon",false); p.putBool("ntp_en",en);       p.end(); }
void saveNtpSyncOnBoot(bool en)        { _ntpSyncOnBoot = en;    Preferences p; p.begin("airmon",false); p.putBool("ntp_boot",en);     p.end(); }
void saveNtpSyncIntervalH(uint8_t h)   { _ntpSyncIntervalH = h;  Preferences p; p.begin("airmon",false); p.putUInt("ntp_intv",h);      p.end(); }

void saveDeviceIdentity(const String &name, const String &host) {
    _deviceName = cleanLabel(name, DEVICE_NAME);
    _hostname   = cleanHostname(host, DEVICE_NAME);
    Preferences p; p.begin("airmon", false);
    p.putString("dev_name", _deviceName);
    p.putString("hostname", _hostname);
    p.end();
}

void saveLedMode(uint8_t mode) {
    _ledMode = mode;
    Preferences p; p.begin("airmon", false);
    p.putUInt("led_mode", mode);
    p.end();
}

void saveAhtCalibration(float tempOffset, float humOffset) {
    _ahtTempOffset = tempOffset;
    _ahtHumOffset  = humOffset;
    Preferences p; p.begin("airmon", false);
    p.putFloat("aht_t_off", tempOffset);
    p.putFloat("aht_h_off", humOffset);
    p.end();
}

void saveTgEnabled(bool en) {
    _tgEnabled = en;
    Preferences p; p.begin("airmon", false);
    p.putBool("tg_en", en);
    p.end();
}

void saveTgCredentials(const String &token, const String &chatId) {
    if (!token.isEmpty()) _tgToken = token;
    _tgChatId = chatId;
    Preferences p; p.begin("airmon", false);
    if (!token.isEmpty()) p.putString("tg_token", token);
    p.putString("tg_chat", chatId);
    p.end();
}

void saveTgThresholds(bool tempHiEn, float tempHi,
                      bool tempLoEn, float tempLo,
                      bool humHiEn,  float humHi,
                      bool humLoEn,  float humLo,
                      bool aqiHiEn,  uint8_t aqiHi,
                      uint16_t cooldownMin) {
    _tgTempHiEn = tempHiEn; _tgTempHi = tempHi;
    _tgTempLoEn = tempLoEn; _tgTempLo = tempLo;
    _tgHumHiEn  = humHiEn;  _tgHumHi  = humHi;
    _tgHumLoEn  = humLoEn;  _tgHumLo  = humLo;
    _tgAqiHiEn  = aqiHiEn;  _tgAqiHi  = aqiHi;
    _tgCooldownMin = cooldownMin;
    Preferences p; p.begin("airmon", false);
    p.putBool("tg_thi_en", tempHiEn); p.putFloat("tg_thi", tempHi);
    p.putBool("tg_tlo_en", tempLoEn); p.putFloat("tg_tlo", tempLo);
    p.putBool("tg_hhi_en", humHiEn);  p.putFloat("tg_hhi", humHi);
    p.putBool("tg_hlo_en", humLoEn);  p.putFloat("tg_hlo", humLo);
    p.putBool("tg_aqi_en", aqiHiEn);  p.putUInt("tg_aqi_hi", aqiHi);
    p.putUInt("tg_cool", cooldownMin);
    p.end();
}

void saveMqttEnabled(bool en) {
    _mqttEnabled = en;
    Preferences p; p.begin("airmon", false);
    p.putBool("mqtt_en", en);
    p.end();
}

void saveMqttSettings(const String &broker, uint16_t port,
                      const String &user, const String &pass,
                      const String &prefix, uint16_t intervalSec) {
    _mqttBroker      = broker;
    _mqttPort        = port;
    _mqttUser        = user;
    if (!pass.isEmpty()) _mqttPassword = pass;
    _mqttPrefix      = prefix;
    _mqttIntervalSec = intervalSec;
    Preferences p; p.begin("airmon", false);
    p.putString("mqtt_broker", broker);
    p.putUInt("mqtt_port",     port);
    p.putString("mqtt_user",   user);
    if (!pass.isEmpty()) p.putString("mqtt_pass", pass);
    p.putString("mqtt_pfx",    prefix);
    p.putUInt("mqtt_intv",     intervalSec);
    p.end();
}

void saveIpSettings(bool staticEnabled, const String &ip,
                    const String &gw, const String &sn, const String &dns) {
    _staticIpEnabled = staticEnabled;
    _staticIp        = ip;
    _staticGateway   = gw;
    _staticSubnet    = sn;
    _staticDns       = dns;
    Preferences p; p.begin("airmon", false);
    p.putBool("sip_en",  staticEnabled);
    p.putString("sip_ip",  ip);
    p.putString("sip_gw",  gw);
    p.putString("sip_sn",  sn);
    p.putString("sip_dns", dns);
    p.end();
}

void saveApIp(const String &ip) {
    _apIp = ip;
    Preferences p; p.begin("airmon", false);
    p.putString("ap_ip", ip);
    p.end();
}

} // namespace RuntimeSettings
