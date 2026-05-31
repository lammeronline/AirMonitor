#include "SDCard.h"
#include "Config.h"
#include <FS.h>
#include <SD.h>
#include <SPI.h>

SPIClass spiSD(HSPI); // Dedicated SD bus to avoid conflicts with the display.

uint64_t initSDCard() {
    spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

    if (!SD.begin(SD_CS, spiSD)) {
        Serial.println("SD Card: FAILED");
        return 0;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("SD Card: INVALID");
        return 0;
    }

    return SD.cardSize() / (1024 * 1024);
}
