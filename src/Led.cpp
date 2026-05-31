#include "Led.h"
#include "Config.h"
#include "RuntimeSettings.h"

void initLED() {
    digitalWrite(LED_R, LOW); pinMode(LED_R, OUTPUT);
    digitalWrite(LED_G, LOW); pinMode(LED_G, OUTPUT);
    digitalWrite(LED_B, LOW); pinMode(LED_B, OUTPUT);
}

void setLED(uint8_t r, uint8_t g, uint8_t b) {
    analogWrite(LED_R, r);
    analogWrite(LED_G, g);
    analogWrite(LED_B, b);
}

void offLED() {
    setLED(0, 0, 0);
}

void updateLED(float temp, float hum, uint8_t aqi, bool sensor_ok) {
    uint8_t mode = RuntimeSettings::ledMode();

    if (mode == 0 || !sensor_ok) {
        offLED();
        return;
    }

    // Цвета совпадают с веб-интерфейсом: blue/green/yellow/orange/red
    // blue=#38c7ff  green=#43e884  yellow=#ffbd2e  orange=#ff922e  red=#ff4f5f
    uint8_t r, g, b;

    // Зелёный диод ярче чем на PCHUB — G уменьшен для тёплых цветов
    if (mode == 1) {                            // Температура
        if      (temp < 18.0f) { r = 30;  g = 80;  b = 255; }  // blue   — Cold
        else if (temp < 26.0f) { r = 0;   g = 210; b = 0;   }  // green  — Comfort/Warm
        else if (temp < 30.0f) { r = 255; g = 30;  b = 0;   }  // orange — Hot
        else                   { r = 255; g = 0;   b = 0;   }  // red    — Very hot
    } else if (mode == 2) {                     // Влажность
        if      (hum < 30.0f)  { r = 255; g = 150; b = 0;   }  // yellow — Dry
        else if (hum < 60.0f)  { r = 0;   g = 210; b = 0;   }  // green  — Normal
        else if (hum < 70.0f)  { r = 255; g = 150; b = 0;   }  // yellow — High
        else                   { r = 255; g = 30;  b = 0;   }  // orange — Very high
    } else {                                    // Качество воздуха (AQI 1–5 UBA)
        switch (aqi) {
            case 1:  r = 0;   g = 210; b = 0;   break;  // green  — Excellent
            case 2:  r = 0;   g = 210; b = 0;   break;  // green  — Good
            case 3:  r = 255; g = 150; b = 0;   break;  // yellow — Moderate
            case 4:  r = 255; g = 30;  b = 0;   break;  // orange — Poor
            case 5:  r = 255; g = 0;   b = 0;   break;  // red    — Bad
            default: offLED(); return;
        }
    }

    setLED(r, g, b);
}
