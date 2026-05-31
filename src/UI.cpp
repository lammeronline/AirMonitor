#include "UI.h"
#include "Config.h"
#include "Logger.h"
#include "Version.h"
#include "RuntimeSettings.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <WiFi.h>

static Adafruit_SSD1306 _oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

static int           _page      = 0;
static bool          _dirty     = true;
static bool          _enabled   = true;
static unsigned long _lastFlip  = 0;

static const unsigned long OLED_SWITCH_INTERVAL = 6000;

static const char *DOW_NAMES[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static const char *AQI_LABEL[] = {"--","Good","Fair","Mod.","Poor","Bad"};

static void centerText(const char *text, int y, uint8_t size = 1) {
    _oled.setTextSize(size);
    int16_t x1, y1; uint16_t w, h;
    _oled.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    _oled.setCursor((OLED_WIDTH - (int)w) / 2, y);
    _oled.print(text);
}

static void _drawPageDots(int current, int total) {
    int startX = (OLED_WIDTH - total * 5) / 2;
    for (int i = 0; i < total; i++) {
        int cx = startX + i * 5 + 2;
        if (i == current)
            _oled.fillCircle(cx, OLED_HEIGHT - 2, 1, SSD1306_WHITE);
        else
            _oled.drawCircle(cx, OLED_HEIGHT - 2, 1, SSD1306_WHITE);
    }
}

// ── Page 0: Sensor data ────────────────────────────────────────────────────────
static void _drawPage0(const SensorData &s) {
    _oled.setTextColor(SSD1306_WHITE);

    // Column labels
    _oled.setTextSize(1);
    _oled.setCursor(0, 0);  _oled.print("TEMP");
    _oled.setCursor(74, 0); _oled.print("HUM");

    // Large values
    _oled.setTextSize(2);
    if (s.aht_ok) {
        char tbuf[8], hbuf[7];
        snprintf(tbuf, sizeof(tbuf), "%.1f", s.temperature);
        snprintf(hbuf, sizeof(hbuf), "%.1f", s.humidity);
        _oled.setCursor(0, 9);  _oled.print(tbuf);
        _oled.setCursor(70, 9); _oled.print(hbuf);
    } else {
        _oled.setCursor(0, 9);  _oled.print("--");
        _oled.setCursor(70, 9); _oled.print("--");
    }

    // Units
    _oled.setTextSize(1);
    _oled.setCursor(0, 27);  _oled.print("\xf8" "C");
    _oled.setCursor(70, 27); _oled.print("%");

    // Divider
    _oled.drawLine(0, 36, OLED_WIDTH - 1, 36, SSD1306_WHITE);

    // CO2 row
    _oled.setCursor(0, 38);
    if (s.ens_ok && s.ens_status == 0) {
        char buf[18];
        snprintf(buf, sizeof(buf), "CO2: %u ppm", s.eco2);
        _oled.print(buf);
    } else {
        _oled.print("CO2: -- ppm");
    }

    // AQI bar — "AQI" + 5 rects + label
    uint8_t aqi = (s.ens_ok && s.ens_status == 0 && s.aqi >= 1 && s.aqi <= 5) ? s.aqi : 0;
    _oled.setCursor(0, 48);
    _oled.print("AQI");
    for (uint8_t i = 0; i < 5; i++) {
        int rx = 22 + i * 12;
        if (i < aqi)
            _oled.fillRect(rx, 48, 10, 8, SSD1306_WHITE);
        else
            _oled.drawRect(rx, 48, 10, 8, SSD1306_WHITE);
    }
    _oled.setCursor(88, 48);
    _oled.print(aqi >= 1 ? AQI_LABEL[aqi] : "--");

    _drawPageDots(0, 2);
}

// ── Page 1: Network / status ───────────────────────────────────────────────────
static void _drawPage1(const SensorData &s, const UiStatus &status) {
    _oled.setTextColor(SSD1306_WHITE);
    _oled.setTextSize(1);

    bool wifiOk = (WiFi.status() == WL_CONNECTED);

    // Header
    centerText("NETWORK / STATUS", 0);

    // IP
    _oled.setCursor(0, 10);
    if (wifiOk)
        _oled.print(WiFi.localIP().toString());
    else
        _oled.print("Not connected");

    // RSSI
    _oled.setCursor(0, 19);
    if (wifiOk) {
        char buf[20];
        snprintf(buf, sizeof(buf), "WiFi: %d dBm", WiFi.RSSI());
        _oled.print(buf);
    } else {
        _oled.print("WiFi: offline");
    }

    // Time + weekday
    _oled.setCursor(0, 28);
    if (s.rtc_ok) {
        // timeStr = "DD.MM.YYYY  HH:MM:SS", time starts at offset 12
        char tbuf[9];
        strncpy(tbuf, s.timeStr + 12, 8);
        tbuf[8] = '\0';
        char line[20];
        snprintf(line, sizeof(line), "%s  %s", tbuf, DOW_NAMES[s.weekday % 7]);
        _oled.print(line);
    } else {
        _oled.print("--:--:--");
    }

    // Date
    _oled.setCursor(0, 37);
    if (s.rtc_ok) {
        char dbuf[11];
        strncpy(dbuf, s.timeStr, 10);
        dbuf[10] = '\0';
        _oled.print(dbuf);
    } else {
        _oled.print("--.--.----");
    }

    // Status icons
    _oled.setCursor(0, 47);
    char stline[22];
    snprintf(stline, sizeof(stline), "SD:%-2s RTC:%-2s ENS:%-2s",
             status.sdReady ? "OK" : "--",
             s.rtc_ok ? "OK" : "--",
             s.ens_ok ? (s.ens_status == 0 ? "OK" : "!") : "--");
    _oled.print(stline);

    _drawPageDots(1, 2);
}

// ── Public API ────────────────────────────────────────────────────────────────

void initUI() {
    Preferences p;
    p.begin("airmon", true);
    _enabled = p.getBool("oled_en", true);
    p.end();

    if (!_oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("OLED: FAILED");
        return;
    }
    _oled.clearDisplay();
    _oled.setTextColor(SSD1306_WHITE);
    _oled.display();
    Serial.println("OLED: OK");
}

bool uiEnabled() { return _enabled; }

void enableUI(bool on) {
    _enabled = on;
    Preferences p;
    p.begin("airmon", false);
    p.putBool("oled_en", on);
    p.end();
    if (!on) {
        _oled.clearDisplay();
        _oled.display();
    }
}

void drawBootTitle() {
    if (!_enabled) return;
    _oled.clearDisplay();
    _oled.setTextSize(2);
    _oled.setTextColor(SSD1306_WHITE);
    centerText(DEVICE_BRAND, 0, 2);
    _oled.setTextSize(1);
    char ver[12];
    snprintf(ver, sizeof(ver), "v%s", FW_VERSION);
    centerText(ver, 20);
    _oled.drawLine(0, 29, OLED_WIDTH - 1, 29, SSD1306_WHITE);
    _oled.display();
}

void drawBootLine(const char *label, const char *value, bool ok) {
    if (!_enabled) return;
    static int _bootY = 32;
    if (_bootY > 56) _bootY = 32;
    _oled.setTextSize(1);
    _oled.setTextColor(SSD1306_WHITE);
    _oled.fillRect(0, _bootY, OLED_WIDTH, 8, SSD1306_BLACK);
    _oled.setCursor(0, _bootY);
    _oled.printf("%-8s%s", label, value);
    _oled.display();
    _bootY += 9;
}

bool handleUI() {
    if (!_enabled) return false;
    if (millis() - _lastFlip >= OLED_SWITCH_INTERVAL) {
        _page     = (_page + 1) % 2;
        _lastFlip = millis();
        _dirty    = true;
    }
    if (_dirty) { _dirty = false; return true; }
    return false;
}

void invalidateUI() {
    _dirty = true;
}

void drawUI(const SensorData &sensor, const UiStatus &status) {
    if (!_enabled) return;
    _oled.clearDisplay();
    _oled.setTextColor(SSD1306_WHITE);

    if (_page == 0)
        _drawPage0(sensor);
    else
        _drawPage1(sensor, status);

    _oled.display();
}

void drawAPScreen(const String &ssid, const String &ip) {
    if (!_enabled) return;
    _oled.clearDisplay();
    _oled.setTextSize(1);
    _oled.setTextColor(SSD1306_WHITE);

    centerText(DEVICE_BRAND, 0, 2);
    _oled.setTextSize(1);
    centerText("AP Setup Mode", 18);
    _oled.drawLine(0, 27, OLED_WIDTH - 1, 27, SSD1306_WHITE);

    _oled.setCursor(0, 30); _oled.print("WiFi: "); _oled.print(ssid);
    _oled.setCursor(0, 42); _oled.print("Open: "); _oled.print(ip);
    _oled.setCursor(0, 54); _oled.print("Settings->WiFi->Save");
    _oled.display();
}

void drawConnectingScreen(const String &ssid) {
    if (!_enabled) return;
    _oled.clearDisplay();
    _oled.setTextSize(1);
    _oled.setTextColor(SSD1306_WHITE);

    centerText(DEVICE_BRAND, 0, 2);
    _oled.setTextSize(1);
    centerText("Connecting...", 20);
    _oled.setCursor(0, 34); _oled.print(ssid);
    _oled.display();
}
