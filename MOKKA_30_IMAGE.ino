/**
 * MOKKA KASSENANZEIGE - 30.–
 * LILYGO Screen-4.7-S3 V2.4
 * Bilddaten aus src/pic30.h, 960x540, 4-bit grayscale
 */

#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM: Tools -> PSRAM -> OPI PSRAM"
#endif

#include <Arduino.h>
#include "epd_driver.h"
#include "utilities.h"
#include "src/pic30.h"

void setup()
{
    Serial.begin(115200);
    delay(1000);

    epd_init();

    Rect_t area = {
        .x = 0,
        .y = 0,
        .width = pic30_width,
        .height = pic30_height
    };

    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(area, (uint8_t *)pic30_data);
    epd_poweroff();
}

void loop()
{
    // ePaper bleibt stehen.
}
