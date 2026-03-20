#include "nrf_spectrum.h"
#include "../../core/display.h"
#include "../../core/mykeyboard.h"
#include <stdio.h>

namespace {

constexpr uint8_t kChannelCount = 81; // 2400..2480 MHz
constexpr uint16_t kSpectrumSettleUs = 250;
constexpr uint16_t kSpectrumListenUs = 250;
constexpr uint32_t kYieldMask = 0x0F;

#define RGB565(r, g, b) ((((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)))
#define _BW (tftWidth / kChannelCount)

uint8_t gChannelLevels[kChannelCount] = {};

class SpectrumCleanupGuard {
public:
    ~SpectrumCleanupGuard() {
        NRFradio.stopListening();
        NRFradio.powerDown();
    }
};

void drawChannelLevel(uint8_t channel, uint8_t level) {
    const int x = channel * _BW;

    tft.drawFastVLine(
        x, tftHeight - (10 + level), level, (channel % 2 == 0) ? bruceConfig.priColor : TFT_DARKGREY
    );

    tft.drawFastVLine(
        x, 0, tftHeight - (9 + level), (channel % 8) ? TFT_BLACK : RGB565(25, 25, 25)
    );
    tft.drawFastVLine(x, 0, level, bruceConfig.secColor);

    if ((channel % 5 == 0) && (channel != 0)) {
        char label[4];
        snprintf(label, sizeof(label), "%u", channel);
        tft.drawCentreString(label, x, tftHeight / 2, 1);
    }
}

void sampleChannel(uint8_t channel) {
    NRFradio.stopListening();
    NRFradio.setChannel(channel);
    delayMicroseconds(kSpectrumSettleUs);
    NRFradio.startListening();
    delayMicroseconds(kSpectrumListenUs);
    NRFradio.stopListening();

    const uint8_t rpd = NRFradio.testRPD() ? 1U : 0U;
    gChannelLevels[channel] = static_cast<uint8_t>((gChannelLevels[channel] * 3U + rpd * 125U) / 4U);
}

} // namespace

String scanChannels(SPIClass *SSPI, bool web) {
    (void)SSPI;

    String result;
    if (web) {
        result.reserve(kChannelCount * 4U + 2U);
        result = "{";
    }

    uint32_t loopCounter = 0;
    for (uint8_t i = 0; i < kChannelCount; ++i) {
        sampleChannel(i);
        drawChannelLevel(i, gChannelLevels[i]);

        if (web) {
            if (i > 0) result += ',';
            result += String(gChannelLevels[i]);
        }

        ++loopCounter;
        if ((loopCounter & kYieldMask) == 0) { taskYIELD(); }
    }

    if (web) result += '}';
    return result;
}

void nrf_spectrum(SPIClass *SSPI) {
    (void)SSPI;

    tft.fillScreen(bruceConfig.bgColor);
    tft.setTextSize(FP);
    tft.drawString("2.40Ghz", 0, tftHeight - LH);
    tft.drawCentreString("2.44Ghz", tftWidth / 2, tftHeight - LH, 1);
    tft.drawRightString("2.48Ghz", tftWidth, tftHeight - LH, 1);

    if (!nrf_start(NRF_MODE_SPI)) {
        Serial.println("Fail Starting radio");
        displayError("NRF24 not found");
        delay(500);
        return;
    }

    SpectrumCleanupGuard cleanup;

    NRFradio.setAutoAck(false);
    NRFradio.disableCRC();
    NRFradio.setAddressWidth(2);

    const uint8_t noiseAddress[][2] = {
        {0x55, 0x55},
        {0xAA, 0xAA},
        {0xA0, 0xAA},
        {0xAB, 0xAA},
        {0xAC, 0xAA},
        {0xAD, 0xAA}
    };

    for (uint8_t i = 0; i < 6; ++i) { NRFradio.openReadingPipe(i, noiseAddress[i]); }
    NRFradio.setDataRate(RF24_1MBPS);
    NRFradio.stopListening();

    while (!check(EscPress)) {
        scanChannels(nullptr, false);
        vTaskDelay(1);
    }
}
