#include "Telegram.h"
#include "RuntimeSettings.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Arduino.h>

namespace Telegram {

static const SensorData *_sensor = nullptr;

static int32_t       _lastUpdateId    = 0;
static bool          _rebootPending   = false;
static unsigned long _rebootPendingMs = 0;

static QueueHandle_t _sendQueue = nullptr;
static const int QUEUE_DEPTH = 6;
static const int MSG_MAX     = 512;

// ── HTTP send ─────────────────────────────────────────────────────────────────
static bool doSend(const char *text) {
    String token  = RuntimeSettings::tgToken();
    String chatId = RuntimeSettings::tgChatId();
    if (token.isEmpty() || chatId.isEmpty()) return false;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://api.telegram.org/bot" + token + "/sendMessage");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(8000);

    String body;
    body.reserve(strlen(text) + 80);
    body = "{\"chat_id\":"; body += chatId; body += ",\"text\":\"";
    for (const char *p = text; *p; p++) {
        if      (*p == '"')  body += "\\\"";
        else if (*p == '\\') body += "\\\\";
        else if (*p == '\n') body += "\\n";
        else                  body += *p;
    }
    body += "\",\"parse_mode\":\"HTML\"}";
    int code = http.POST(body);
    http.end();
    return code == 200;
}

// ── Command handler ───────────────────────────────────────────────────────────
static void handleCmd(const String &text) {
    String dev = RuntimeSettings::deviceName();

    if (text == "/status" || text.startsWith("/status ")) {
        String m = "\U0001F3E0 <b>" + dev + "</b>\n\n";
        if (_sensor && _sensor->aht_ok) {
            m += "\U0001F321 Температура: <b>" + String(_sensor->temperature, 1) + " °C</b>\n";
            m += "\U0001F4A7 Влажность: <b>"   + String(_sensor->humidity, 1)    + " %</b>\n";
        } else {
            m += "⚠️ AHT21: нет данных\n";
        }
        if (_sensor && _sensor->ens_ok && _sensor->ens_status == 0 && _sensor->aqi > 0) {
            static const char *aqiNames[] = {"--","Good","Moderate","Unhealthy","Poor","Bad"};
            m += "\U0001F4A8 AQI: <b>" + String(_sensor->aqi) + " – " +
                 aqiNames[min((int)_sensor->aqi, 5)] + "</b>\n";
            m += "   TVOC: " + String(_sensor->tvoc) + " ppb\n";
            m += "   eCO2: " + String(_sensor->eco2) + " ppm\n";
        } else if (_sensor && _sensor->ens_ok) {
            const char *statNames[] = {"OK","Warm-up","Initial start-up","Invalid"};
            m += "\U0001F4A8 ENS160: " + String(statNames[_sensor->ens_status & 3]) + "\n";
        }
        if (_sensor && _sensor->rtc_ok)
            m += "⏰ Время: " + String(_sensor->timeStr) + "\n";
        doSend(m.c_str());

    } else if (text == "/help" || text.startsWith("/help ")) {
        String m = "\U0001F3E0 <b>" + dev + "</b>\n\n"
                   "/status — показания датчиков\n"
                   "/reboot — перезагрузить устройство\n"
                   "/help — это сообщение";
        doSend(m.c_str());

    } else if (text == "/reboot") {
        _rebootPending   = true;
        _rebootPendingMs = millis();
        doSend(("\U0001F3E0 <b>" + dev + "</b>\n\nОтправьте <code>/reboot confirm</code> в течение 30 с для подтверждения.").c_str());

    } else if (text == "/reboot confirm") {
        if (_rebootPending && millis() - _rebootPendingMs < 30000UL) {
            doSend(("♻️ <b>" + dev + "</b> перезагружается…").c_str());
            vTaskDelay(pdMS_TO_TICKS(600));
            ESP.restart();
        } else {
            _rebootPending = false;
            doSend("❌ Нет активного запроса или время истекло.");
        }
    }
}

// ── Polling ───────────────────────────────────────────────────────────────────
static void doPoll() {
    String token = RuntimeSettings::tgToken();
    if (token.isEmpty()) return;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = "https://api.telegram.org/bot" + token + "/getUpdates?offset=" +
                 (_lastUpdateId + 1) + "&limit=10&timeout=0";
    http.begin(client, url);
    http.setTimeout(5000);
    int code = http.GET();
    if (code != 200) { http.end(); return; }
    String payload = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, payload) != DeserializationError::Ok) return;
    if (!doc["ok"].as<bool>()) return;

    String allowed = RuntimeSettings::tgChatId();
    for (JsonObject upd : doc["result"].as<JsonArray>()) {
        int32_t uid = upd["update_id"] | 0;
        if (uid > _lastUpdateId) _lastUpdateId = uid;
        JsonObject msg = upd["message"];
        if (!msg) continue;
        String text = msg["text"] | "";
        text.trim();
        if (text.isEmpty()) continue;
        char cidBuf[24];
        snprintf(cidBuf, sizeof(cidBuf), "%lld", (long long)(msg["chat"]["id"] | 0LL));
        if (!allowed.isEmpty() && String(cidBuf) != allowed) continue;
        handleCmd(text);
    }
}

// ── Alert engine ──────────────────────────────────────────────────────────────
struct AlertState {
    unsigned long lastSentMs = 0;
    bool active = false;
};

static AlertState _aTempHi, _aTempLo, _aHumHi, _aHumLo, _aAqiHi;

static void evalAlertF(AlertState &st, bool cond, unsigned long coolMs,
                       const char *fmt, float val, float thr) {
    if (cond) {
        unsigned long now = millis();
        if (!st.active || now - st.lastSentMs >= coolMs) {
            char body[160];
            snprintf(body, sizeof(body), fmt, val, thr);
            String m = "\U0001F3E0 <b>" + RuntimeSettings::deviceName() + "</b>\n" + body;
            doSend(m.c_str());
            st.lastSentMs = now;
        }
        st.active = true;
    } else {
        st.active = false;
    }
}

static void evalAlertAqi(AlertState &st, bool cond, unsigned long coolMs, uint8_t aqi, uint8_t thr) {
    static const char *aqiNames[] = {"--","Good","Moderate","Unhealthy","Poor","Bad"};
    if (cond) {
        unsigned long now = millis();
        if (!st.active || now - st.lastSentMs >= coolMs) {
            char body[160];
            snprintf(body, sizeof(body),
                     "\U0001F4A8 AQI %u (%s) ≥ порог %u",
                     aqi, aqiNames[min((int)aqi, 5)], (unsigned)thr);
            String m = "\U0001F3E0 <b>" + RuntimeSettings::deviceName() + "</b>\n" + body;
            doSend(m.c_str());
            st.lastSentMs = now;
        }
        st.active = true;
    } else {
        st.active = false;
    }
}

static void doCheckAlerts() {
    if (!_sensor) return;
    unsigned long coolMs = (unsigned long)RuntimeSettings::tgCooldownMin() * 60000UL;

    if (_sensor->aht_ok) {
        float t = _sensor->temperature, h = _sensor->humidity;
        if (RuntimeSettings::tgTempHiEn())
            evalAlertF(_aTempHi, t >= RuntimeSettings::tgTempHi(), coolMs,
                       "\U0001F321 Высокая температура: %.1f °C (порог %.1f °C)", t, RuntimeSettings::tgTempHi());
        if (RuntimeSettings::tgTempLoEn())
            evalAlertF(_aTempLo, t <= RuntimeSettings::tgTempLo(), coolMs,
                       "\U0001F321 Низкая температура: %.1f °C (порог %.1f °C)", t, RuntimeSettings::tgTempLo());
        if (RuntimeSettings::tgHumHiEn())
            evalAlertF(_aHumHi, h >= RuntimeSettings::tgHumHi(), coolMs,
                       "\U0001F4A7 Высокая влажность: %.1f%% (порог %.1f%%)", h, RuntimeSettings::tgHumHi());
        if (RuntimeSettings::tgHumLoEn())
            evalAlertF(_aHumLo, h <= RuntimeSettings::tgHumLo(), coolMs,
                       "\U0001F4A7 Низкая влажность: %.1f%% (порог %.1f%%)", h, RuntimeSettings::tgHumLo());
    }

    // AQI alert: only when ENS reports normal
    if (_sensor->ens_ok && _sensor->ens_status == 0 && _sensor->aqi > 0) {
        if (RuntimeSettings::tgAqiHiEn()) {
            evalAlertAqi(_aAqiHi, _sensor->aqi >= RuntimeSettings::tgAqiHi(),
                         coolMs, _sensor->aqi, RuntimeSettings::tgAqiHi());
        }
    }
}

// ── FreeRTOS task (core 0) ────────────────────────────────────────────────────
static void telegramTask(void *) {
    static unsigned long lastAlertMs  = 0;
    static unsigned long lastPollMs   = 0;
    char msgBuf[MSG_MAX];

    for (;;) {
        if (RuntimeSettings::tgEnabled()) {
            while (xQueueReceive(_sendQueue, msgBuf, 0) == pdTRUE)
                doSend(msgBuf);

            unsigned long now = millis();
            if (now - lastPollMs >= 5000UL) {
                lastPollMs = now;
                doPoll();
            }
            if (now - lastAlertMs >= 30000UL) {
                lastAlertMs = now;
                doCheckAlerts();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
void begin(const SensorData *sensor) {
    _sensor = sensor;
    _sendQueue = xQueueCreate(QUEUE_DEPTH, MSG_MAX);
    xTaskCreatePinnedToCore(telegramTask, "tg", 8192, nullptr, 1, nullptr, 0);
}

void handle() { /* alerts driven by task */ }

bool sendMessage(const String &text) {
    if (!_sendQueue) return false;
    char buf[MSG_MAX];
    text.toCharArray(buf, MSG_MAX);
    return xQueueSend(_sendQueue, buf, 0) == pdTRUE;
}

} // namespace Telegram
