#include "nrf_common.h"
#include "../../core/mykeyboard.h"

RF24 NRFradio(bruceConfigPins.NRF24_bus.io0, bruceConfigPins.NRF24_bus.cs);
HardwareSerial NRFSerial = HardwareSerial(2); // Uses UART2 for External NRF's
rf24_pa_dbm_e current_pa_level = RF24_PA_MAX;
static SPIClass *NRFSPI = &SPI;

namespace {

using SpiPins = BruceConfigPins::SPIPins;

constexpr uint32_t kNrfUartBaud = 115200;
constexpr uint32_t kNrfUartTimeoutMs = 20;

bool isValidGpio(gpio_num_t pin) { return pin != GPIO_NUM_NC; }

bool isSameSpiBus(const SpiPins &a, const SpiPins &b) {
    return a.sck == b.sck && a.mosi == b.mosi && a.miso == b.miso && isValidGpio(a.sck) &&
           isValidGpio(a.mosi) && isValidGpio(a.miso);
}

bool isValidNrfSpiPins(const SpiPins &pins) {
    return isValidGpio(pins.sck) && isValidGpio(pins.miso) && isValidGpio(pins.mosi) && isValidGpio(pins.cs) &&
           isValidGpio(pins.io0);
}

bool isTftSharedBus(const SpiPins &pins) {
#if TFT_MOSI > 0
    const bool mosiMatch = pins.mosi == static_cast<gpio_num_t>(TFT_MOSI) && isValidGpio(pins.mosi);
#if TFT_SCLK > 0
    const bool sckMatch = pins.sck == static_cast<gpio_num_t>(TFT_SCLK) && isValidGpio(pins.sck);
#else
    const bool sckMatch = true;
#endif
    return mosiMatch && sckMatch;
#else
    (void)pins;
    return false;
#endif
}

void forceCsHigh(const SpiPins &nrf, const SpiPins &other) {
    if (!isSameSpiBus(nrf, other)) return;
    if (!isValidGpio(other.cs) || other.cs == nrf.cs) return;
    pinMode(other.cs, OUTPUT);
    digitalWrite(other.cs, HIGH);
}

void releaseSharedDevices(const SpiPins &nrf) {
    forceCsHigh(nrf, bruceConfigPins.SDCARD_bus);
    forceCsHigh(nrf, bruceConfigPins.CC1101_bus);
#if !defined(LITE_VERSION)
    forceCsHigh(nrf, bruceConfigPins.W5500_bus);
#endif
}

SPIClass *selectNrfSpiBus(const SpiPins &pins) {
    if (isSameSpiBus(pins, bruceConfigPins.SDCARD_bus)) { return &sdcardSPI; }
    if (isTftSharedBus(pins)) {
#if TFT_MOSI > 0
        return &tft.getSPIinstance();
#else
        return &SPI;
#endif
    }
    return &SPI;
}

} // namespace

void nrf_info() {
    tft.fillScreen(bruceConfig.bgColor);
    tft.setTextSize(FM);
    tft.setTextColor(TFT_RED, bruceConfig.bgColor);
    tft.drawCentreString("_Disclaimer_", tftWidth / 2, 10, 1);
    tft.setTextColor(TFT_WHITE, bruceConfig.bgColor);
    tft.setTextSize(FP);
    tft.setCursor(15, 33);
    padprintln("These functions were made to be used in a controlled environment for STUDY only.");
    padprintln("");
    padprintln("DO NOT use these functions to harm people or companies, you can go to jail!");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    padprintln("");
    padprintln(
        "This device is VERY sensible to noise, so long wires or passing near VCC line can make "
        "things go wrong."
    );
    delay(1000);
    while (!check(AnyKeyPress));
}

bool nrf_start(NRF24_MODE mode) {
    bool result = false;
    if (mode == NRF_MODE_DISABLED) return false;

    if (CHECK_NRF_UART(mode)) {
        if (USBserial.getSerialOutput() == &Serial1) {
            displayError("(E) UART already in use", true);
            return false;
        }
        NRFSerial.begin(kNrfUartBaud, SERIAL_8N1, bruceConfigPins.uart_bus.rx, bruceConfigPins.uart_bus.tx);
        NRFSerial.setTimeout(kNrfUartTimeoutMs);
        Serial.println("NRF24 on Serial Started");
        result = true;
    }

    if (!CHECK_NRF_SPI(mode)) return result;

    const SpiPins &nrfPins = bruceConfigPins.NRF24_bus;
    if (!isValidNrfSpiPins(nrfPins)) {
        Serial.println("NRF24 SPI pins are invalid, aborting NRF SPI start");
        return false;
    }

    pinMode(nrfPins.cs, OUTPUT);
    digitalWrite(nrfPins.cs, HIGH);
    pinMode(nrfPins.io0, OUTPUT);
    digitalWrite(nrfPins.io0, LOW);

    releaseSharedDevices(nrfPins);

    NRFSPI = selectNrfSpiBus(nrfPins);
    NRFSPI->begin((int8_t)nrfPins.sck, (int8_t)nrfPins.miso, (int8_t)nrfPins.mosi);
    delay(10);

    if (NRFradio.begin(NRFSPI, rf24_gpio_pin_t(nrfPins.io0), rf24_gpio_pin_t(nrfPins.cs))) {
        result = true;
    } else {
        Serial.println("NRF24 SPI begin failed");
        return false;
    }

    NRFradio.setPALevel(current_pa_level);
    NRFradio.stopListening();
    return result;
}

NRF24_MODE nrf_setMode() {
    NRF24_MODE mode = NRF_MODE_DISABLED;
    options = {
        {"SPI Mode",  [&]() { mode = NRF_MODE_SPI; } },
        {"SPI UART",  [&]() { mode = NRF_MODE_UART; }},
        {"SPI BOTH",  [&]() { mode = NRF_MODE_BOTH; }},
        {"Main Menu", [=]() { returnToMenu = true; } }
    };
    loopOptions(options);
    return mode;
}
