#include "led_control.h"
#include "core/display.h"
#include "core/main_menu.h"
#include "core/settings.h"
#include "core/utils.h"
#include <math.h>

TaskHandle_t ledTaskHandle = NULL;
volatile int attackLedMode = 0;
CRGB attackLedColor = CRGB_Colors::Black;

void setLedAttackMode(int mode, CRGB color) {
    attackLedMode = mode;
    attackLedColor = color;
    if (mode == 0) {
        ledSetup(); // Сброс атаки - возвращаем цвет из настроек
    }
}

// Фоновая задача
void cydLedTask(void *pvParameters) {
    float breathPhase = 0.0;
    int blinkState = 0;
    while (1) {
        if (attackLedMode == 1) {
            // 1: Режим снайпера. Плавное дыхание 20% - 100%
            float breathMult = 0.2 + 0.8 * ((sin(breathPhase) + 1.0) / 2.0);
            breathPhase += 0.05;
            if (breathPhase > 2 * PI) breathPhase -= 2 * PI;

            float maxBright = bruceConfig.ledBright / 100.0;
            float currentBright = maxBright * breathMult;

            analogWrite(CYD_LED_R, 255 - (attackLedColor.r * currentBright));
            analogWrite(CYD_LED_G, 255 - (attackLedColor.g * currentBright));
            analogWrite(CYD_LED_B, 255 - (attackLedColor.b * currentBright));
            vTaskDelay(30 / portTICK_PERIOD_MS);

        } else if (attackLedMode == 2) {
            // 2: Режим успеха. Очереди: та-та-та ... пауза ... та-та-та
            // Шаг 80мс. 0, 2, 4 = горим. 1, 3 = тухнем. 5-14 = длинная пауза.
            bool isOn = (blinkState == 0 || blinkState == 2 || blinkState == 4);
            float currentBright = isOn ? (bruceConfig.ledBright / 100.0) : 0.0;

            analogWrite(CYD_LED_R, 255 - (attackLedColor.r * currentBright));
            analogWrite(CYD_LED_G, 255 - (attackLedColor.g * currentBright));
            analogWrite(CYD_LED_B, 255 - (attackLedColor.b * currentBright));

            blinkState = (blinkState + 1) % 15;
            vTaskDelay(80 / portTICK_PERIOD_MS); // Увеличили задержку до 80мс

        } else if (attackLedMode == 3) {
            // 3: Режим Деаутентификации. Временный солидный цвет
            float currentBright = bruceConfig.ledBright / 100.0;
            analogWrite(CYD_LED_R, 255 - (attackLedColor.r * currentBright));
            analogWrite(CYD_LED_G, 255 - (attackLedColor.g * currentBright));
            analogWrite(CYD_LED_B, 255 - (attackLedColor.b * currentBright));
            vTaskDelay(100 / portTICK_PERIOD_MS);

        } else if (bruceConfig.ledEffect == 1) {
            // Обычный мирный режим: дыхание 0-100%
            float breathMult = (sin(breathPhase) + 1.0) / 2.0;
            breathPhase += 0.05;
            if (breathPhase > 2 * PI) breathPhase -= 2 * PI;

            float maxBright = bruceConfig.ledBright / 100.0;
            float currentBright = maxBright * breathMult;

            CRGB color = CRGB(bruceConfig.ledColor);
            analogWrite(CYD_LED_R, 255 - (color.r * currentBright));
            analogWrite(CYD_LED_G, 255 - (color.g * currentBright));
            analogWrite(CYD_LED_B, 255 - (color.b * currentBright));

            vTaskDelay(30 / portTICK_PERIOD_MS);
        } else {
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
    }
}

void beginLed() {
    pinMode(CYD_LED_R, OUTPUT);
    pinMode(CYD_LED_G, OUTPUT);
    pinMode(CYD_LED_B, OUTPUT);
    ledSetup();

    if (ledTaskHandle == NULL) {
        xTaskCreatePinnedToCore(cydLedTask, "CYD_LED_Task", 2048, NULL, 1, &ledTaskHandle, 1);
    }
}

void setLedColor(CRGB color) {
    if (attackLedMode != 0) return; // Игнорируем мирные команды во время атаки

    uint32_t c = (color.r << 16) | (color.g << 8) | color.b;
    if (bruceConfig.ledEffect == 1 && c == bruceConfig.ledColor) return;

    float bright = bruceConfig.ledBright / 100.0;
    int r = 255 - (color.r * bright);
    int g = 255 - (color.g * bright);
    int b = 255 - (color.b * bright);

    analogWrite(CYD_LED_R, r);
    analogWrite(CYD_LED_G, g);
    analogWrite(CYD_LED_B, b);
}

void ledSetup() {
    if (bruceConfig.ledEffect == 0 && attackLedMode == 0) { setLedColor(CRGB(bruceConfig.ledColor)); }
}

void blinkLed(int blinkTime) {
    if (!bruceConfig.ledBlinkEnabled) return;

    int oldEffect = bruceConfig.ledEffect;
    bruceConfig.ledEffect = 0;

    float bright = bruceConfig.ledBright / 100.0;
    analogWrite(CYD_LED_R, 255 - (255 * bright));
    analogWrite(CYD_LED_G, 255 - (255 * bright));
    analogWrite(CYD_LED_B, 255 - (255 * bright));

    delay(blinkTime);
    bruceConfig.ledEffect = oldEffect;
    ledSetup();
}

void setLedColorConfig() {
    struct ColorMapping {
        const char *name;
        CRGB color;
    };
    const ColorMapping colorMappings[] = {
        {"OFF",    CRGB_Colors::Black },
        {"Red",    CRGB_Colors::Red   },
        {"Green",  CRGB_Colors::Green },
        {"Blue",   CRGB_Colors::Blue  },
        {"Purple", CRGB_Colors::Purple},
        {"White",  CRGB_Colors::White }
    };

    while (true) {
        options.clear();
        for (const auto &mapping : colorMappings) {
            options.emplace_back(mapping.name, [=]() {
                uint32_t c = (mapping.color.r << 16) | (mapping.color.g << 8) | mapping.color.b;
                bruceConfig.setLedColor(c);
                ledSetup();
            });
        }
        options.push_back({"Back", []() {}});

        addOptionToMainMenu();
        int selectedOption = loopOptions(options, MENU_TYPE_SUBMENU, "Select Color");
        if (selectedOption == -1 || selectedOption == options.size() - 1) return;
    }
}

void setLedBrightnessConfig() {
    std::vector<int> brightLevels = {0, 10, 25, 50, 75, 100};
    while (true) {
        options.clear();
        for (int b : brightLevels) {
            options.emplace_back(String(b) + "%", [=]() {
                bruceConfig.setLedBright(b);
                ledSetup();
            });
        }
        options.push_back({"Back", []() {}});

        addOptionToMainMenu();
        int selectedOption = loopOptions(options, MENU_TYPE_SUBMENU, "Brightness");
        if (selectedOption == -1 || selectedOption == options.size() - 1) return;
    }
}

void setLedEffectConfig() {
    while (true) {
        options = {
            {"Solid Color",
             [=]() {
                 bruceConfig.setLedEffect(0);
                 ledSetup();
             }, bruceConfig.ledEffect == 0},
            {"Breathe", [=]() { bruceConfig.setLedEffect(1); }, bruceConfig.ledEffect == 1},
            {"Back", []() {}}
        };
        addOptionToMainMenu();
        int selected = loopOptions(options, MENU_TYPE_SUBMENU, "LED Effect");
        if (selected == -1 || selected == options.size() - 1) return;
    }
}
