#include "nrf_jammer.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "nrf_common.h"
#include <globals.h>
#include <stdio.h>

namespace {

constexpr uint16_t kDefaultSettleUs = 250;
constexpr uint16_t kTurboSettleUs = 170;
constexpr uint32_t kUartProbeIntervalMs = 250;
constexpr uint32_t kSchedulerPauseMask = 0x7F;
static constexpr rf24_pa_dbm_e kPaCycle[] = {RF24_PA_MIN, RF24_PA_LOW, RF24_PA_HIGH, RF24_PA_MAX};
static constexpr const char *kPaLabels[] = {"MIN", "LOW", "HIGH", "MAX"};

template <typename T, size_t N> constexpr size_t arrayCount(const T (&)[N]) { return N; }

static constexpr uint8_t kTestChannels[] = {
    50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, 2,  4,  6,  8,
    10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48
};

static constexpr uint8_t kWifiChannels[] = {
    2, 7, 12, 17, 22, 27, 32, 37, 42, 47, 52, 57, 62, 67, 72, 77
};

// BLE data channels: 2404..2478 MHz, advertising channels excluded.
static constexpr uint8_t kBleDataChannels[] = {
    4,  6,  8,  10, 12, 14, 16, 18, 20, 22, 24, 28, 30, 32, 34, 36, 38, 40, 42,
    44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78
};

// BLE advertising channels: 2402, 2426, 2480 MHz.
static constexpr uint8_t kBleAdvertisingChannels[] = {2, 26, 80};

// Classic Bluetooth / proprietary 1 MHz hopping devices: 2402..2480 MHz.
static constexpr uint8_t kBluetoothChannels[] = {
    2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
    42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
    62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80
};

static constexpr uint8_t kUsbChannels[] = {40, 50, 60};
static constexpr uint8_t kVideoChannels[] = {70, 75, 80};
static constexpr uint8_t kRcChannels[] = {2, 26, 50, 74};

static constexpr uint8_t kFullChannels[] = {
    1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,
    17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,
    33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,
    49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,
    65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,
    81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,
    97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112,
    113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124
};

struct JamMode {
    const char *name;
    const char *uartCommand;
    const uint8_t *channels;
    uint8_t count;
    uint16_t settleUs;
    bool pseudoRandom;
};

static constexpr JamMode kJamModes[] = {
    {"Test",             "TEST",      kTestChannels,           arrayCount(kTestChannels),           kDefaultSettleUs, false},
    {"WiFi",             "WIFI",      kWifiChannels,           arrayCount(kWifiChannels),           kDefaultSettleUs, false},
    {"BLE Data",         "BLE_DATA",  kBleDataChannels,        arrayCount(kBleDataChannels),        kDefaultSettleUs, false},
    {"BLE Adv",          "BLE_ADV",   kBleAdvertisingChannels, arrayCount(kBleAdvertisingChannels), kDefaultSettleUs, false},
    {"Bluetooth",        "BT",        kBluetoothChannels,      arrayCount(kBluetoothChannels),      kDefaultSettleUs, false},
    {"USB",              "USB",       kUsbChannels,            arrayCount(kUsbChannels),            kDefaultSettleUs, false},
    {"Video Stream",     "VIDEO",     kVideoChannels,          arrayCount(kVideoChannels),          kDefaultSettleUs, false},
    {"RC",               "RC",        kRcChannels,             arrayCount(kRcChannels),             kDefaultSettleUs, false},
    {"Full",             "FULL",      kFullChannels,           arrayCount(kFullChannels),           kDefaultSettleUs, false},
    {"Turbo BT/Mouse",   "TURBO_BT",  kBluetoothChannels,      arrayCount(kBluetoothChannels),      kTurboSettleUs,   true }
};

struct UartPollState {
    char rxBuffer[16] = {};
    uint8_t rxLength = 0;
    uint8_t onlineCount = 0;
    uint8_t localRadios = 0;
    bool handshakeSeen = false;
    uint32_t lastProbeMs = 0;
};

int paLevelToIndex(rf24_pa_dbm_e level) {
    for (size_t i = 0; i < arrayCount(kPaCycle); ++i) {
        if (kPaCycle[i] == level) return static_cast<int>(i);
    }
    return static_cast<int>(arrayCount(kPaCycle) - 1);
}

const char *paLevelToText(rf24_pa_dbm_e level) {
    const int index = paLevelToIndex(level);
    return kPaLabels[index];
}

class CarrierCleanupGuard {
public:
    explicit CarrierCleanupGuard(NRF24_MODE mode) : mode_(mode) {}

    void markSpiActive() { spiActive_ = true; }

    ~CarrierCleanupGuard() {
        if (spiActive_) {
            NRFradio.stopConstCarrier();
            spiActive_ = false;
        }
        if (CHECK_NRF_UART(mode_)) { NRFSerial.println("OFF"); }
    }

private:
    NRF24_MODE mode_;
    bool spiActive_ = false;
};

uint32_t nextPseudoRandom(uint32_t &state) {
    if (state == 0) { state = 0xA5A55A5Au ^ micros(); }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void shuffleTurboOrder(uint8_t *order, const uint8_t *source, uint8_t count, uint32_t &rngState) {
    for (uint8_t i = 0; i < count; ++i) { order[i] = source[i]; }
    if (count < 2) return;

    for (int i = count - 1; i > 0; --i) {
        const uint32_t rnd = nextPseudoRandom(rngState);
        const uint8_t j = static_cast<uint8_t>(rnd % static_cast<uint32_t>(i + 1));
        const uint8_t tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
}

void configureCarrierRadio() {
    NRFradio.setAutoAck(false);
    NRFradio.disableCRC();
    NRFradio.setPALevel(current_pa_level);
    NRFradio.setAddressWidth(5);
    NRFradio.setPayloadSize(2);
    NRFradio.setDataRate(RF24_2MBPS);
}

void setChannelAndSettle(uint8_t channel, uint16_t settleUs) {
    NRFradio.setChannel(channel);
    delayMicroseconds(settleUs);
}

void startCarrierOnChannel(uint8_t channel, uint16_t settleUs) {
    setChannelAndSettle(channel, settleUs);
    NRFradio.startConstCarrier(current_pa_level, channel);
}

void maybePauseScheduler(uint32_t &loopCounter) {
    ++loopCounter;
    if ((loopCounter & kSchedulerPauseMask) == 0) { vTaskDelay(1); }
}

bool uartEnabled(NRF24_MODE mode) { return CHECK_NRF_UART(mode); }

void initUartState(UartPollState &state, NRF24_MODE mode) {
    state.localRadios = CHECK_NRF_SPI(mode) ? 1 : 0;
    state.onlineCount = state.localRadios;
    state.handshakeSeen = false;
    state.lastProbeMs = 0;
    state.rxLength = 0;
}

bool pollRemoteRadioCount(UartPollState &state, NRF24_MODE mode) {
    if (!uartEnabled(mode)) return false;

    const uint32_t now = millis();
    if (!state.handshakeSeen && (now - state.lastProbeMs) >= kUartProbeIntervalMs) {
        NRFSerial.println("RADIOS");
        state.lastProbeMs = now;
    }

    bool changed = false;
    while (NRFSerial.available() > 0) {
        const int raw = NRFSerial.read();
        if (raw < 0) break;

        const char ch = static_cast<char>(raw);
        if (ch == '\r') continue;

        if (ch == '\n') {
            if (state.rxLength == 1 && state.rxBuffer[0] >= '0' && state.rxBuffer[0] <= '9') {
                const uint8_t remoteCount = static_cast<uint8_t>(state.rxBuffer[0] - '0');
                state.onlineCount = state.localRadios + remoteCount;
                state.handshakeSeen = true;
                changed = true;
            }
            state.rxLength = 0;
            continue;
        }

        if (state.rxLength < sizeof(state.rxBuffer) - 1) {
            state.rxBuffer[state.rxLength++] = ch;
            state.rxBuffer[state.rxLength] = '\0';
        } else {
            state.rxLength = 0;
        }
    }

    return changed;
}

void sendUartCommand(NRF24_MODE mode, const char *command) {
    if (!uartEnabled(mode) || command == nullptr || command[0] == '\0') return;
    NRFSerial.println(command);
}

void drawStatusLine(int y, const char *text) {
    tft.fillRect(10, y, tftWidth - 20, FM * LH + 6, bruceConfig.bgColor);
    tft.setCursor(10, y);
    tft.print(text);
}

void drawPowerStatusLine(int y) {
    char line[24];
    snprintf(line, sizeof(line), "[PWR: %s]", paLevelToText(current_pa_level));
    drawStatusLine(y, line);
}

bool selectPaLevelUi(const char *modeTitle) {
    int paIndex = paLevelToIndex(current_pa_level);
    bool redraw = true;

    while (true) {
        if (redraw) {
            drawMainBorder();
            tft.setTextSize(FM);
            drawStatusLine(35, modeTitle);
            drawStatusLine(65, "TX Power Setup");
            drawPowerStatusLine(100);
            drawStatusLine(140, "NEXT/PREV: change");
            drawStatusLine(160, "SEL: start  ESC: back");
            redraw = false;
        }

        bool changed = false;
        if (check(NextPress)) {
            paIndex = (paIndex + 1) % static_cast<int>(arrayCount(kPaCycle));
            changed = true;
        }
        if (check(PrevPress)) {
            paIndex = (paIndex == 0) ? static_cast<int>(arrayCount(kPaCycle)) - 1 : paIndex - 1;
            changed = true;
        }
        if (changed) {
            current_pa_level = kPaCycle[paIndex];
            redraw = true;
        }

        if (check(SelPress)) {
            current_pa_level = kPaCycle[paIndex];
            return true;
        }
        if (check(EscPress)) return false;

        vTaskDelay(1);
    }
}

void drawJammerUi(uint8_t onlineCount, const JamMode &mode) {
    char line[48];

    drawMainBorder();
    tft.setTextSize(FM);

    drawStatusLine(35, "NRF X Jammer");

    snprintf(line, sizeof(line), "STATUS : %u ACTIVE", onlineCount);
    drawStatusLine(60, line);

    snprintf(line, sizeof(line), "MODE : %s", mode.name);
    drawStatusLine(100, line);
    drawPowerStatusLine(125);
}

void drawChannelJammerUi(uint8_t onlineCount, uint8_t channel) {
    char line[48];

    drawMainBorder();
    tft.setTextSize(FM);

    drawStatusLine(35, "NRF Channel Jammer");

    snprintf(line, sizeof(line), "STATUS : %u ACTIVE", onlineCount);
    drawStatusLine(60, line);

    snprintf(line, sizeof(line), "MODE : CH %u", channel);
    drawStatusLine(100, line);

    snprintf(line, sizeof(line), "Freq : %u MHz", static_cast<unsigned>(2400 + channel));
    drawStatusLine(125, line);
    drawPowerStatusLine(150);
}

void drawHopperConfigUi(uint8_t onlineCount, int startChannel, int stopChannel, int stepSize, int menuIndex, bool editMode) {
    char line[48];
    int highlightY = 70;

    drawMainBorder();
    tft.setTextSize(FM);
    drawStatusLine(35, "NRF Hopper Config");

    snprintf(line, sizeof(line), "STATUS : %u ACTIVE", onlineCount);
    drawStatusLine(55, line);

    snprintf(line, sizeof(line), "Start : CH %d", startChannel);
    drawStatusLine(70, line);

    snprintf(line, sizeof(line), "Stop  : CH %d", stopChannel);
    drawStatusLine(90, line);

    snprintf(line, sizeof(line), "Step  : %d MHz", stepSize);
    drawStatusLine(110, line);
    drawPowerStatusLine(130);

    drawStatusLine(150, "Start Jammer");
    drawStatusLine(170, "Exit");

    if (menuIndex == 1) highlightY = 90;
    else if (menuIndex == 2) highlightY = 110;
    else if (menuIndex == 3) highlightY = 150;
    else if (menuIndex == 4) highlightY = 170;

    tft.drawRect(5, highlightY - 2, tftWidth - 10, 18, bruceConfig.priColor);
    if (editMode && menuIndex < 3) { tft.drawRect(7, highlightY, tftWidth - 14, 14, bruceConfig.secColor); }
}

void drawHopperRunUi(uint8_t onlineCount, int startChannel, int stopChannel, int stepSize, int channel) {
    char line[48];

    drawMainBorder();
    tft.setTextSize(FM);
    drawStatusLine(35, "NRF Hopper Jammer");

    snprintf(line, sizeof(line), "STATUS : %u ACTIVE", onlineCount);
    drawStatusLine(60, line);

    snprintf(line, sizeof(line), "Range : %d - %d", startChannel, stopChannel);
    drawStatusLine(90, line);

    snprintf(line, sizeof(line), "Step  : %d", stepSize);
    drawStatusLine(110, line);

    snprintf(line, sizeof(line), "CH    : %d", channel);
    drawStatusLine(130, line);
    drawPowerStatusLine(150);
}

uint8_t modeFirstChannel(const JamMode &mode, uint8_t *turboOrder, uint32_t &rngState) {
    if (mode.pseudoRandom) {
        shuffleTurboOrder(turboOrder, mode.channels, mode.count, rngState);
        return turboOrder[0];
    }
    return mode.channels[0];
}

uint8_t modeChannelAt(const JamMode &mode, size_t index, uint8_t *turboOrder) {
    return mode.pseudoRandom ? turboOrder[index] : mode.channels[index];
}

} // namespace

void nrf_jammer() {
    const NRF24_MODE mode = nrf_setMode();
    if (mode == NRF_MODE_DISABLED) return;

    if (!nrf_start(mode)) {
        displayError("NRF24 not found");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        return;
    }

    CarrierCleanupGuard cleanup(mode);
    if (!selectPaLevelUi("NRF X Jammer")) return;

    UartPollState uartState;
    initUartState(uartState, mode);

    uint32_t schedulerCounter = 0;
    uint32_t turboRngState = micros();
    uint8_t turboOrder[arrayCount(kBluetoothChannels)] = {};

    int modeIndex = 0;
    size_t hopIndex = 0;
    bool redraw = true;

    if (CHECK_NRF_SPI(mode)) {
        configureCarrierRadio();
        const uint8_t firstChannel = modeFirstChannel(kJamModes[modeIndex], turboOrder, turboRngState);
        startCarrierOnChannel(firstChannel, kJamModes[modeIndex].settleUs);
        cleanup.markSpiActive();
        hopIndex = (kJamModes[modeIndex].count > 1) ? 1U : 0U;
    }

    sendUartCommand(mode, kJamModes[modeIndex].uartCommand);

    while (!check(SelPress)) {
        if (pollRemoteRadioCount(uartState, mode)) { redraw = true; }

        if (redraw) {
            drawJammerUi(uartState.onlineCount, kJamModes[modeIndex]);
            redraw = false;
        }

        if (check(NextPress)) {
            modeIndex = (modeIndex + 1) % static_cast<int>(arrayCount(kJamModes));
            hopIndex = 0;
            if (CHECK_NRF_SPI(mode)) {
                const uint8_t firstChannel = modeFirstChannel(kJamModes[modeIndex], turboOrder, turboRngState);
                setChannelAndSettle(firstChannel, kJamModes[modeIndex].settleUs);
                hopIndex = (kJamModes[modeIndex].count > 1) ? 1U : 0U;
            }
            sendUartCommand(mode, kJamModes[modeIndex].uartCommand);
            redraw = true;
        }

        if (check(PrevPress)) {
            modeIndex = (modeIndex == 0) ? static_cast<int>(arrayCount(kJamModes)) - 1 : modeIndex - 1;
            hopIndex = 0;
            if (CHECK_NRF_SPI(mode)) {
                const uint8_t firstChannel = modeFirstChannel(kJamModes[modeIndex], turboOrder, turboRngState);
                setChannelAndSettle(firstChannel, kJamModes[modeIndex].settleUs);
                hopIndex = (kJamModes[modeIndex].count > 1) ? 1U : 0U;
            }
            sendUartCommand(mode, kJamModes[modeIndex].uartCommand);
            redraw = true;
        }

        if (CHECK_NRF_SPI(mode)) {
            const JamMode &currentMode = kJamModes[modeIndex];
            const uint8_t nextChannel = modeChannelAt(currentMode, hopIndex, turboOrder);
            setChannelAndSettle(nextChannel, currentMode.settleUs);

            ++hopIndex;
            if (hopIndex >= currentMode.count) {
                hopIndex = 0;
                if (currentMode.pseudoRandom) {
                    shuffleTurboOrder(turboOrder, currentMode.channels, currentMode.count, turboRngState);
                }
            }
        }

        maybePauseScheduler(schedulerCounter);
    }
}

void nrf_channel_jammer() {
    const NRF24_MODE mode = nrf_setMode();
    if (mode == NRF_MODE_DISABLED) return;

    if (!nrf_start(mode)) {
        displayError("NRF24 not found");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        return;
    }

    CarrierCleanupGuard cleanup(mode);
    if (!selectPaLevelUi("NRF Channel Jammer")) return;

    UartPollState uartState;
    initUartState(uartState, mode);

    uint32_t schedulerCounter = 0;
    uint8_t channel = 50;
    bool redraw = true;

    if (CHECK_NRF_SPI(mode)) {
        configureCarrierRadio();
        startCarrierOnChannel(channel, kDefaultSettleUs);
        cleanup.markSpiActive();
    }

    char uartCommand[16];
    snprintf(uartCommand, sizeof(uartCommand), "CH_%u", channel);
    sendUartCommand(mode, uartCommand);

    while (!check(SelPress)) {
        if (pollRemoteRadioCount(uartState, mode)) { redraw = true; }

        if (redraw) {
            drawChannelJammerUi(uartState.onlineCount, channel);
            redraw = false;
        }

        bool channelChanged = false;
        if (check(NextPress)) {
            channel = (channel >= 125) ? 1 : static_cast<uint8_t>(channel + 1);
            channelChanged = true;
        }

        if (check(PrevPress)) {
            channel = (channel <= 1) ? 125 : static_cast<uint8_t>(channel - 1);
            channelChanged = true;
        }

        if (channelChanged) {
            if (CHECK_NRF_SPI(mode)) { setChannelAndSettle(channel, kDefaultSettleUs); }

            snprintf(uartCommand, sizeof(uartCommand), "CH_%u", channel);
            sendUartCommand(mode, uartCommand);
            redraw = true;
        }

        maybePauseScheduler(schedulerCounter);
    }
}

void nrf_channel_hopper() {
    const NRF24_MODE mode = nrf_setMode();
    if (mode == NRF_MODE_DISABLED) return;

    if (!nrf_start(mode)) {
        displayError("NRF24 not found");
        vTaskDelay(100 / portTICK_PERIOD_MS);
        return;
    }

    CarrierCleanupGuard cleanup(mode);
    if (!selectPaLevelUi("NRF Hopper")) return;

    UartPollState uartState;
    initUartState(uartState, mode);

    int startChannel = 2;
    int stopChannel = 80;
    int stepSize = 2;
    int menuIndex = 0;
    bool redraw = true;
    bool editMode = false;
    bool runJammer = false;
    uint32_t schedulerCounter = 0;

    if (CHECK_NRF_SPI(mode)) { configureCarrierRadio(); }

    while (true) {
        if (pollRemoteRadioCount(uartState, mode)) { redraw = true; }

        if (redraw) {
            char uartCommand[32];
            drawHopperConfigUi(uartState.onlineCount, startChannel, stopChannel, stepSize, menuIndex, editMode);
            snprintf(uartCommand, sizeof(uartCommand), "HOPPER_%d_%d_%d", startChannel, stopChannel, stepSize);
            sendUartCommand(mode, uartCommand);
            redraw = false;
        }

        if (check(EscPress)) { return; }

        if (check(NextPress)) {
            if (editMode) {
                if (menuIndex == 0) startChannel = (startChannel >= 125) ? 1 : startChannel + 1;
                else if (menuIndex == 1) stopChannel = (stopChannel >= 125) ? 1 : stopChannel + 1;
                else if (menuIndex == 2) stepSize = (stepSize >= 10) ? 1 : stepSize + 1;
            } else {
                menuIndex = (menuIndex + 1) % 5;
            }
            redraw = true;
        }

        if (check(PrevPress)) {
            if (editMode) {
                if (menuIndex == 0) startChannel = (startChannel <= 1) ? 125 : startChannel - 1;
                else if (menuIndex == 1) stopChannel = (stopChannel <= 1) ? 125 : stopChannel - 1;
                else if (menuIndex == 2) stepSize = (stepSize <= 1) ? 10 : stepSize - 1;
            } else {
                menuIndex = (menuIndex == 0) ? 4 : menuIndex - 1;
            }
            redraw = true;
        }

        if (check(SelPress)) {
            if (!editMode && menuIndex == 3) {
                runJammer = true;
                break;
            }
            if (!editMode && menuIndex == 4) { return; }
            if (menuIndex < 3) {
                editMode = !editMode;
                redraw = true;
            }
        }

        maybePauseScheduler(schedulerCounter);
    }

    if (startChannel > stopChannel) {
        const int tmp = startChannel;
        startChannel = stopChannel;
        stopChannel = tmp;
    }
    if (stepSize < 1) stepSize = 1;

    if (!runJammer) return;

    uint8_t channel = static_cast<uint8_t>(startChannel);
    if (CHECK_NRF_SPI(mode)) {
        startCarrierOnChannel(channel, kDefaultSettleUs);
        cleanup.markSpiActive();
    }

    char uartCommand[32];
    snprintf(uartCommand, sizeof(uartCommand), "HOPPER_%d_%d_%d", startChannel, stopChannel, stepSize);
    sendUartCommand(mode, uartCommand);

    bool redrawRunUi = true;
    while (!check(EscPress)) {
        if (pollRemoteRadioCount(uartState, mode)) { redrawRunUi = true; }

        if (redrawRunUi) {
            drawHopperRunUi(uartState.onlineCount, startChannel, stopChannel, stepSize, channel);
            redrawRunUi = false;
        }

        int nextChannel = static_cast<int>(channel) + stepSize;
        if (nextChannel > stopChannel) nextChannel = startChannel;
        channel = static_cast<uint8_t>(nextChannel);

        if (CHECK_NRF_SPI(mode)) { setChannelAndSettle(channel, kDefaultSettleUs); }

        redrawRunUi = true;
        maybePauseScheduler(schedulerCounter);
    }
}
