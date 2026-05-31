# AirMonitor

An ESP32-based indoor air quality monitor. Measures AQI, TVOC, eCO2, temperature, and humidity; shows data on an OLED display; and provides a web dashboard with live charts, MQTT publishing, Telegram alerts, SD card logging, and OTA firmware updates.

**Firmware version:** 2.0.0

---

## Hardware

| Component | Interface | Notes |
|---|---|---|
| ESP32 (38-pin) | — | Main MCU |
| ScioSense ENS160 | I²C (0x52 / 0x53) | AQI / TVOC / eCO2 |
| Sensirion AHT21 (SparkFun AHT20) | I²C | Temperature / humidity |
| DS3231 RTC | I²C | Battery-backed real-time clock |
| SSD1306 OLED 128×64 | I²C | 2-page status display |
| SD card module | SPI | Data logging (CSV) |
| Common-cathode RGB LED | GPIO (PWM) | Status indicator |

### Default pin assignment (`include/Config.h`)

| Signal | GPIO |
|---|---|
| I²C SDA | 21 |
| I²C SCL | 22 |
| SPI MOSI | 23 |
| SPI MISO | 19 |
| SPI SCK | 18 |
| SD CS | 5 |
| LED R | 25 |
| LED G | 26 |
| LED B | 27 |

---

## Features

- **Air quality** — ENS160 provides AQI (1–5 UBA scale), TVOC (ppb), and eCO2 (ppm) with automatic temperature/humidity compensation
- **Climate** — AHT21 temperature and humidity with configurable calibration offsets
- **Time** — DS3231 RTC with NTP sync; configurable server, UTC offset, and sync interval (6/12/24/48 h)
- **OLED display** — alternates every 6 s between sensor readings and network/status page
- **RGB LED** — configurable mode: off / status / temperature-based / humidity-based / AQI-based colour
- **SD logging** — appends a CSV row every 60 s; file created automatically if absent
- **Web dashboard** — SPA served from flash (PROGMEM); live sensor data, 24 h/7 d/30 d charts, full settings UI
- **MQTT** — publishes sensor readings on a configurable interval and topic prefix
- **Telegram alerts** — threshold-based notifications for temperature, humidity, and AQI with per-metric cooldown
- **OTA firmware update** — upload a new `.bin` directly from the web UI
- **mDNS** — device reachable at `http://<hostname>.local/`
- **AP mode with captive portal** — starts its own hotspot on first boot; Android, iOS, Windows, and Chrome captive-portal detection all supported
- **Static IP** — optional fixed IP address alongside DHCP
- **WiFi watchdog** — auto-reconnects after 60 s of connectivity loss
- **Factory reset** — single button in the web UI erases all NVS settings (SD data preserved)

---

## Building

Requires [PlatformIO](https://platformio.org/).

```bash
cd AirMonitor
pio run                 # compile
pio run -t upload       # compile + flash
pio device monitor      # serial output at 115200
```

### Library dependencies (`platformio.ini`)

```ini
lib_deps =
    adafruit/Adafruit SSD1306@^2.5.7
    adafruit/Adafruit GFX Library@^1.11.9
    adafruit/RTClib@^2.1.1
    sparkfun/SparkFun Qwiic Humidity AHT20
    sparkfun/SparkFun Indoor Air Quality Sensor - ENS160
    bblanchon/ArduinoJson@^7.0.0
    knolleary/PubSubClient@^2.8
```

---

## First boot — AP mode

If no Wi-Fi credentials are saved the device starts an access point:

- **SSID**: `AirMonitor-<hostname>` (hostname configurable)
- **Password**: `12345678`
- **Config URL**: `http://192.168.4.1`

Open **Settings → Network**, enter your Wi-Fi credentials, save, and reboot.

---

## Web dashboard tabs

| Tab | Contents |
|---|---|
| Dashboard | Live AQI gauge, temperature, humidity, eCO2, TVOC; ENS160 status footer |
| History | 24 h / 7 d / 30 d charts for temperature, humidity, AQI, TVOC, eCO2 |
| Settings → Network | Wi-Fi SSID/password, static IP, hostname |
| Settings → NTP | NTP server, UTC offset, sync interval |
| Settings → Device | LED mode, OLED on/off, sensor calibration offsets, ENS160 reset |
| Settings → MQTT | Broker, port, credentials, topic prefix, publish interval |
| Settings → Telegram | Bot token, chat ID, per-metric alert thresholds and cooldown |
| Data | CSV download, history export |
| Info | Firmware version, uptime, heap, RSSI |
| System | OTA upload, factory reset, reboot |

---

## REST API

| Method | Path | Description |
|---|---|---|
| GET | `/api/status` | JSON snapshot of all sensor data and system state |
| GET | `/api/settings` | Current settings as JSON |
| POST | `/api/settings` | Update settings (partial JSON body accepted) |
| GET | `/api/scan` | Async Wi-Fi network scan results |
| POST | `/api/ens/reset` | Reinitialise ENS160 |
| GET | `/api/history?range=24h\|7d\|30d` | Averaged history as JSON |
| POST | `/api/history/clear` | Clear in-memory history buffers |
| GET | `/api/log` | Download `readings.csv` from the SD card |
| POST | `/api/sd/clear` | Erase all files on the SD card |
| GET | `/api/telegram` | Telegram settings as JSON |
| POST | `/api/telegram` | Update Telegram settings |
| POST | `/api/telegram/test` | Send a test message via the bot |
| GET | `/api/mqtt` | MQTT settings as JSON |
| POST | `/api/mqtt` | Update MQTT settings |
| GET | `/api/ip` | IP / static-IP settings as JSON |
| POST | `/api/ip` | Update IP settings |
| POST | `/api/ota` | Upload firmware `.bin` (multipart/form-data) |
| POST | `/api/reboot` | Reboot the device |
| POST | `/api/factory-reset` | Erase all NVS settings and reboot |

### `/api/status` response (excerpt)

```json
{
  "ok": true,
  "fw_version": "2.0.0",
  "device": "AirMonitor",
  "ip": "192.168.1.55",
  "hostname": "airmonitor.local",
  "sensor": {
    "temperature": 23.4,
    "humidity": 48.1,
    "aqi": 1,
    "tvoc": 120,
    "eco2": 650,
    "ens_ok": true,
    "ens_status": 0,
    "aht_ok": true,
    "rtc_ok": true,
    "time": "07.05.2026  14:30:00"
  },
  "system": {
    "heap_free": 180000,
    "uptime_sec": 3600,
    "sd_ready": true,
    "sd_total_mb": 7630,
    "sd_used_mb": 12
  },
  "mqtt": {
    "enabled": true,
    "connected": true,
    "broker": "192.168.1.10",
    "port": 1883
  },
  "telegram": {
    "enabled": false,
    "has_token": false
  }
}
```

---

## ENS160 status codes

| Code | Meaning | Dashboard colour |
|---|---|---|
| 0 | Operating normally | Green |
| 1 | Warm-up (~60 s after power-on) | Yellow |
| 2 | Initial start-up (~5 min on first ever run) | Yellow |
| 3 | No valid output | Red |

Only status 0 rows are written to the SD card log. Use **Settings → Device → ENS160 Reset** to force re-initialisation if the sensor is stuck.

---

## MQTT topics

All topics follow the pattern `<prefix>/<hostname>/<metric>`. Default prefix is `airmonitor`, default hostname is `airmonitor`.

| Topic | Value |
|---|---|
| `<prefix>/<hostname>/temperature` | °C |
| `<prefix>/<hostname>/humidity` | % RH |
| `<prefix>/<hostname>/aqi` | 1–5 |
| `<prefix>/<hostname>/tvoc` | ppb |
| `<prefix>/<hostname>/eco2` | ppm |
| `<prefix>/<hostname>/state` | JSON object with all sensor fields |

The device also subscribes to `<prefix>/<hostname>/cmd` and reboots on receiving `reboot`.

### Home Assistant auto-discovery

On every MQTT connection the device publishes Home Assistant MQTT Discovery config messages under `homeassistant/sensor/<uid>/config` for temperature, humidity, eCO2, TVOC, and AQI. No manual HA configuration is required — entities appear automatically when the integration is active.

---

## Telegram bot

1. Create a bot via [@BotFather](https://t.me/BotFather) and copy the token.
2. Get your chat ID — send any message to your bot, then open `https://api.telegram.org/bot<TOKEN>/getUpdates`.
3. Open **Settings → Telegram**, enter the token and chat ID, configure thresholds, and save.

### Commands

| Command | Description |
|---|---|
| `/status` | Current sensor readings |
| `/reboot` | Reboot the device |

### Alert thresholds (defaults)

| Alert | Default |
|---|---|
| Temperature HIGH | 30 °C |
| Temperature LOW | 15 °C |
| Humidity HIGH | 75 % |
| Humidity LOW | 30 % |
| AQI HIGH | 3 |

Cooldown between repeated alerts defaults to 10 minutes.

---

## SD card log format

`/readings.csv` — columns: `time,rtc_ok,ens_status,aht_ok,temp_c,humidity_pct,aqi,tvoc_ppb,eco2_ppm`

A new row is appended every 60 s when the SD card is present and ENS160 reports status 0. The file and header are created automatically on the first write. Download the file via **Data** tab or `GET /api/log`. Use **System → Clear SD** to erase all SD data.

---

## Project structure

```
AirMonitor/
├── include/
│   ├── Config.h              # Pin definitions and compile-time constants
│   ├── Sensors.h             # SensorData struct + public API
│   ├── UI.h                  # OLED display functions
│   ├── Led.h                 # RGB LED functions
│   ├── API.h                 # HTTP server + history buffers
│   ├── Logger.h              # SD card logging
│   ├── MQTT.h                # MQTT client
│   ├── Telegram.h            # Telegram bot interface
│   ├── RuntimeSettings.h     # NVS-backed runtime configuration
│   └── Version.h             # Firmware version string
├── src/
│   ├── main.cpp              # Setup / loop; all subsystem timers
│   ├── Sensors.cpp           # ENS160, AHT21, DS3231, Wi-Fi, NTP
│   ├── UI.cpp                # SSD1306 2-page display driver
│   ├── Led.cpp               # PWM RGB LED control
│   ├── API.cpp               # REST endpoints, history ringbuffers, WebSocket
│   ├── Logger.cpp            # CSV logging to SD card
│   ├── MQTT.cpp              # MQTT broker connection and publishing
│   ├── Telegram.cpp          # Bot polling, command handling, threshold alerts
│   ├── RuntimeSettings.cpp   # NVS getters/setters with defaults
│   ├── SDCard.cpp            # SD card initialisation
│   └── WebUI.html            # Embedded SPA dashboard (compiled → WebUI.h)
└── platformio.ini
```
