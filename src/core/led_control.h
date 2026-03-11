#ifndef __LED_CONTROL_H__
#define __LED_CONTROL_H__

#include <Arduino.h>

#define CYD_LED_R 4
#define CYD_LED_G 16
#define CYD_LED_B 17

struct CRGB {
    uint8_t r, g, b;
    CRGB() : r(0), g(0), b(0) {}
    CRGB(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) {}
    CRGB(uint32_t c) : r((c >> 16) & 0xFF), g((c >> 8) & 0xFF), b(c & 0xFF) {}
};

namespace CRGB_Colors {
const CRGB Black(0, 0, 0);
const CRGB Red(255, 0, 0);
const CRGB Green(0, 255, 0);
const CRGB Blue(0, 0, 255);
const CRGB White(255, 255, 255);
const CRGB Purple(128, 0, 128);
} // namespace CRGB_Colors

void beginLed();
void setLedColor(CRGB color);
void setLedAttackMode(int mode, CRGB color); // <-- Новая функция для атак
void ledSetup();
void setLedColorConfig();
void setLedBrightnessConfig();
void setLedEffectConfig();
void blinkLed(int blinkTime = 50);

#endif
