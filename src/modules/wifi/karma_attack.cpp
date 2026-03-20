/*
  Not perfect just improve it.
*/
#include "FS.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/wifi/wifi_common.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lwip/err.h"
#include "nvs_flash.h"
#include <set>
#include <vector>

#include "karma_attack.h"
#include "sniffer.h" // Channel list
#include <Arduino.h>
#include <TimeLib.h>
#include <globals.h>
#if defined(ESP32)
#include "FS.h"
#else
#include <SPI.h>
#include <SdFat.h>
#endif

//===== SETTINGS =====//
#define FILENAME "probe_capture_"
#define SAVE_INTERVAL 10     // save new file every 30s
#define CHANNEL_HOPPING true // if true it will scan on all channels
#define MAX_CHANNEL 11       //(only necessary if channelHopping is true)
#define FAST_HOP_INTERVAL 500
#define DEFAULT_HOP_INTERVAL 10000 // Normal mode (10s)

//===== Run-Time variables =====//
unsigned long last_time = 0;
unsigned long last_ChannelChange = 0;
uint8_t channl = 0;
bool flOpen = false;
bool is_LittleFS = true;
uint32_t pkt_counter = 0;
bool auto_hopping = true; // Enable/disable flag
unsigned long hop_interval = DEFAULT_HOP_INTERVAL;

File _probe_file;
std::set<String> uniqueProbes; // Stores unique probe requests (MAC+SSID)
std::vector<String> probeList; // Stores all probes in order
String filen = "";

std::vector<ProbeRequest> probeRequests;

namespace {
constexpr size_t PROBE_SSID_MAX_LEN = 32;
constexpr size_t PROBE_QUEUE_DEPTH = 32;

struct ProbeEvent {
    int8_t rssi = 0;
    uint8_t channel = 0;
    uint32_t timestamp = 0;
    uint8_t mac[6] = {0};
    char ssid[PROBE_SSID_MAX_LEN + 1] = {0};
};

QueueHandle_t probeQueue = nullptr;

void formatMac(const uint8_t mac[6], char out[18]) {
    snprintf(
        out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    );
}

bool extractProbeEvent(const wifi_promiscuous_pkt_t *packet, ProbeEvent &event) {
    if (!packet) return false;

    const uint8_t *frame = packet->payload;
    const int sigLen = packet->rx_ctrl.sig_len;
    if (sigLen <= 24) return false;

    const uint8_t frameType = (frame[0] & 0x0C) >> 2;
    const uint8_t frameSubType = (frame[0] & 0xF0) >> 4;
    if (frameType != 0x00 || frameSubType != 0x04) return false;

    int pos = 24;
    while (pos + 1 < sigLen) {
        const uint8_t tag = frame[pos++];
        const uint8_t len = frame[pos++];

        if (pos + len > sigLen) return false;
        if (tag == 0x00 && len > 0) {
            if (len > PROBE_SSID_MAX_LEN) return false;

            for (uint8_t i = 0; i < len; ++i) {
                if (!isPrintable(frame[pos + i])) return false;
            }

            memcpy(event.ssid, frame + pos, len);
            event.ssid[len] = '\0';
            memcpy(event.mac, frame + 10, sizeof(event.mac));
            event.rssi = packet->rx_ctrl.rssi;
            event.timestamp = packet->rx_ctrl.timestamp / 1000U;
            event.channel = all_wifi_channels[channl];
            return true;
        }

        pos += len;
    }

    return false;
}

void drainProbeQueue() {
    if (!probeQueue) return;

    ProbeEvent event;
    while (xQueueReceive(probeQueue, &event, 0) == pdTRUE) {
        char macBuf[18];
        formatMac(event.mac, macBuf);

        String mac(macBuf);
        String ssid(event.ssid);
        if (ssid.length() == 0) continue;

        String key;
        key.reserve(mac.length() + ssid.length());
        key += mac;
        key += ssid;

        if (uniqueProbes.insert(key).second) {
            ProbeRequest probe;
            probe.mac = mac;
            probe.ssid = ssid;
            probe.rssi = event.rssi;
            probe.timestamp = event.timestamp;
            probe.channel = event.channel;
            probeRequests.push_back(probe);
            pkt_counter++;
        }
    }
}
} // namespace

//===== FUNCTIONS =====//

String generateUniqueFilename(FS &fs) {
    String basePath = "/ProbeData/";
    String baseName = FILENAME;
    String extension = ".txt";

    if (!fs.exists(basePath)) { fs.mkdir(basePath); }

    int counter = 1;
    String filename;
    do {
        filename = basePath + baseName + String(counter) + extension;
        counter++;
    } while (fs.exists(filename));

    return filename;
}

// Check if packet is a probe request with SSID
bool isProbeRequestWithSSID(const wifi_promiscuous_pkt_t *packet) {
    const uint8_t *frame = packet->payload;
    uint8_t frameType = (frame[0] & 0x0C) >> 2;
    uint8_t frameSubType = (frame[0] & 0xF0) >> 4;

    if (frameType == 0x00 && frameSubType == 0x04) { // probe request
        int pos = 24;
        while (pos < packet->rx_ctrl.sig_len) {
            uint8_t tag = frame[pos];
            uint8_t len = frame[pos + 1];

            if (tag == 0x00 && len > 0) return true; // SSID tag
            pos += len + 2;
        }
    }
    return false;
}

// Get all unique probe requests with SSID
std::vector<ProbeRequest> getUniqueProbes() {
    std::vector<ProbeRequest> unique;
    std::set<String> seenSSIDs;
    for (auto it = probeRequests.rbegin(); it != probeRequests.rend(); ++it) {
        const auto &probe = *it;
        if (seenSSIDs.find(probe.ssid) == seenSSIDs.end()) {
            seenSSIDs.insert(probe.ssid);
            unique.push_back(probe);
        }
    }

    std::reverse(unique.begin(), unique.end());

    return unique;
}

// Clear collected probe requests
void clearProbes() {
    probeRequests.clear();
    uniqueProbes.clear();
    pkt_counter = 0;
    if (probeQueue) xQueueReset(probeQueue);
}

// Packet callback function - ONLY PROCESSES PROBES WITH SSID
void probe_sniffer(void *buf, wifi_promiscuous_pkt_type_t type) {
    (void)type;
    if (!probeQueue) return;

    ProbeEvent event;
    if (!extractProbeEvent(static_cast<const wifi_promiscuous_pkt_t *>(buf), event)) return;

    BaseType_t taskWoken = pdFALSE;
    xQueueSendFromISR(probeQueue, &event, &taskWoken);
    if (taskWoken) portYIELD_FROM_ISR();
}

//===== SETUP =====//

void safe_wifi_deinit() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_deinit();
    vTaskDelay(100 / portTICK_PERIOD_MS); // Crucial delay
}

void karma_setup() {
    // Clean shutdown if previous WiFi was active
    if (esp_wifi_stop() == ESP_OK) { safe_wifi_deinit(); }

    vTaskDelay(200 / portTICK_PERIOD_MS);

    FS *Fs;
    int redraw = true;
    String FileSys = "LittleFS";
    drawMainBorderWithTitle("PROBE SNIFFER");

    if (setupSdCard()) {
        Fs = &SD;
        FileSys = "SD";
        is_LittleFS = false;
        filen = generateUniqueFilename(SD);
    } else {
        Fs = &LittleFS;
        filen = generateUniqueFilename(LittleFS);
    }

    // Create directories if they don't exist
    if (!Fs->exists("/ProbeData")) Fs->mkdir("/ProbeData");

    displayTextLine("Sniffing Started");
    tft.setTextSize(FP);
    tft.setCursor(80, 100);

    // Clear previous data
    clearProbes();
    if (!probeQueue) probeQueue = xQueueCreate(PROBE_QUEUE_DEPTH, sizeof(ProbeEvent));
    if (!probeQueue) {
        displayError("Probe queue alloc failed", true);
        return;
    }

    nvs_flash_init();
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    cfg.rx_ba_win = 16;     // Increased from default 6
    cfg.nvs_enable = false; // Disable NVS for monitor mode

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // Set to monitor mode (no AP mode needed for probe sniffing)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(probe_sniffer);
    wifi_second_chan_t secondCh = (wifi_second_chan_t)NULL;
    esp_wifi_set_channel(all_wifi_channels[channl], secondCh);

    Serial.println("Probe sniffer started!");
    vTaskDelay(1000 / portTICK_RATE_MS);

    if (is_LittleFS && !checkLittleFsSize()) goto Exit;

    for (;;) {
        drainProbeQueue();

        if (returnToMenu) { // if it happened, LittleFS is full or user exited
            if (!checkLittleFsSize()) {
                Serial.println("Not enough space on LittleFS");
                displayError("LittleFS Full", true);
            }
            break;
        }

        unsigned long currentTime = millis();

        if (auto_hopping && (currentTime - last_ChannelChange >= hop_interval)) {
            esp_wifi_set_promiscuous(false);
            esp_wifi_set_promiscuous_rx_cb(nullptr);

            channl++;
            if (channl >= sizeof(all_wifi_channels)) channl = 0;

            wifi_second_chan_t secondCh = (wifi_second_chan_t)NULL;
            esp_wifi_set_channel(all_wifi_channels[channl], secondCh);
            last_ChannelChange = currentTime;
            redraw = true;

            esp_wifi_set_promiscuous(true);
            esp_wifi_set_promiscuous_rx_cb(probe_sniffer);
        }

        /* Channel Hopping */
        if (check(NextPress)) {
            esp_wifi_set_promiscuous(false);
            esp_wifi_set_promiscuous_rx_cb(nullptr);
            channl++; // increase channel
            if (channl >= sizeof(all_wifi_channels)) channl = 0;
            wifi_second_chan_t secondCh = (wifi_second_chan_t)NULL;
            esp_wifi_set_channel(all_wifi_channels[channl], secondCh);
            redraw = true;
            vTaskDelay(50 / portTICK_RATE_MS);
            esp_wifi_set_promiscuous(true);
            esp_wifi_set_promiscuous_rx_cb(probe_sniffer);
        }

        if (PrevPress) {
#if !defined(HAS_KEYBOARD) && !defined(HAS_ENCODER)
            LongPress = true;
            long _tmp = millis();
            while (PrevPress) {
                if (millis() - _tmp > 150)
                    tft.drawArc(
                        tftWidth / 2,
                        tftHeight / 2,
                        25,
                        15,
                        0,
                        360 * (millis() - _tmp) / 700,
                        getColorVariation(bruceConfig.priColor),
                        bruceConfig.bgColor
                    );
                vTaskDelay(10 / portTICK_RATE_MS);
            }
            LongPress = false;
            if (millis() - _tmp > 700) { // longpress detected to exit
                returnToMenu = true;
                break;
            }
#endif
            check(PrevPress);
            esp_wifi_set_promiscuous(false);
            esp_wifi_set_promiscuous_rx_cb(nullptr);
            if (channl > 0) channl--; // decrease channel
            else channl = sizeof(all_wifi_channels) - 1;
            wifi_second_chan_t secondCh = (wifi_second_chan_t)NULL;
            esp_wifi_set_channel(all_wifi_channels[channl], secondCh);
            redraw = true;
            vTaskDelay(50 / portTICK_PERIOD_MS);
            esp_wifi_set_promiscuous(true);
            esp_wifi_set_promiscuous_rx_cb(probe_sniffer);
        }

#if defined(HAS_KEYBOARD) || defined(T_EMBED)
        if (check(EscPress)) { // Power or Esc button
            returnToMenu = true;
            break;
        }
#endif

        if (check(SelPress) || redraw) {
            vTaskDelay(200 / portTICK_PERIOD_MS);
            if (!redraw) {
                options = {
                    {"Karma atk",
                     [=]() {
                         // Get unique probe requests
                         std::vector<ProbeRequest> uniqueProbes = getUniqueProbes();
                         std::vector<Option> karmaOptions;
                         for (const auto &probe : uniqueProbes) {
                             String itemText =
                                 probe.ssid + " (" + probe.rssi + "|ch " + String(probe.channel) + ")";
                             karmaOptions.push_back({itemText.c_str(), [=]() {
                                                         esp_wifi_set_promiscuous(false);
                                                         esp_wifi_stop();
                                                         esp_wifi_deinit(); // Critical for clean restart
                                                         vTaskDelay(500 / portTICK_PERIOD_MS);
                                                         // EvilPortal(probe.ssid, probe.channel, false,
                                                         // false);
                                                         wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
                                                         esp_wifi_init(&cfg);
                                                         esp_wifi_set_mode(WIFI_MODE_NULL);
                                                         esp_wifi_start();
                                                         esp_wifi_set_promiscuous(true);
                                                         esp_wifi_set_promiscuous_rx_cb(probe_sniffer);
                                                     }});
                         }

                         karmaOptions.push_back({"Back", [=]() {}});
                         loopOptions(karmaOptions);
                     }                                                                                              },

                    {"Save Probes",
                     [=]() {
                         if (is_LittleFS) saveProbesToFile(LittleFS);
                         else saveProbesToFile(SD);
                         displayTextLine("Probes saved!");
                     }                                                                                              },

                    {"Clear Probes",
                     [=]() {
                         clearProbes();
                         displayTextLine("Probes cleared!");
                     }                                                                                              },
                    {auto_hopping ? "* Auto Hop" : "- Auto Hop",
                     [=]() {
                         auto_hopping = !auto_hopping;
                         displayTextLine(auto_hopping ? "Auto Hop: ON" : "Auto Hop: OFF");
                     }                                                                                              },
                    {hop_interval == FAST_HOP_INTERVAL ? "* Fast Hop" : "- Fast Hop",
                     [=]() {
                         hop_interval =
                             (hop_interval == FAST_HOP_INTERVAL) ? DEFAULT_HOP_INTERVAL : FAST_HOP_INTERVAL;
                         displayTextLine(
                             hop_interval == FAST_HOP_INTERVAL ? "Fast Hop: ON" : "Fast Hop: OFF"
                         );
                     }                                                                                              },

                    {"Exit Sniffer",                                                  [=]() { returnToMenu = true; }},
                };
                loopOptions(options);
            }

            if (returnToMenu) goto Exit;
            redraw = false;
            tft.drawPixel(0, 0, 0);
            drawMainBorderWithTitle("PROBE SNIFFER");
            tft.setTextSize(FP);
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            padprintln("Saved to " + FileSys);
            padprintln("Sniffing ssids from probes...");
            padprintln(String(BTN_ALIAS) + ": Options Menu");
            tft.drawRightString(
                "Ch." +
                    String(
                        all_wifi_channels[channl] < 10    ? "  "
                        : all_wifi_channels[channl] < 100 ? " "
                                                          : ""
                    ) +
                    String(all_wifi_channels[channl]) + "(Next)",
                tftWidth - 10,
                tftHeight - 18,
                1
            );
        }

        vTaskDelay(5 / portTICK_PERIOD_MS);

        if (currentTime - last_time > 100) tft.drawPixel(0, 0, 0);

        if (currentTime - last_time > 1000) {
            last_time = currentTime;
            tft.drawString("Unique: " + String(uniqueProbes.size()), 10, tftHeight - 18);
            tft.drawCentreString("Packets " + String(pkt_counter), tftWidth / 2, tftHeight - 26, 1);
            String hopStatus = String(auto_hopping ? "A:" : "M:") + String(hop_interval) + "ms";
            tft.drawRightString(hopStatus, tftWidth - 10, tftHeight - 34);
            if (!probeRequests.empty()) {
                ProbeRequest latest = probeRequests.back();
                String displayText = latest.ssid + " -> " + latest.mac;
                if (displayText.length() > 55) { displayText = displayText.substring(0, 52) + "..."; }
                padprint(displayText);
            }
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

Exit:
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_deinit();
    vTaskDelay(1 / portTICK_RATE_MS);
}

void saveProbesToFile(FS &fs) {
    if (!fs.exists("/ProbeData")) fs.mkdir("/ProbeData");

    File file = fs.open(filen, FILE_WRITE);
    if (file) {
        file.println("Timestamp,MAC,RSSI,SSID");
        for (const auto &probe : probeRequests) {
            // Double check we only save probes with SSID (shouldn't be needed but just in case)
            if (probe.ssid.length() > 0) {
                file.printf(
                    "%lu,%s,%d,\"%s\"\n", probe.timestamp, probe.mac.c_str(), probe.rssi, probe.ssid.c_str()
                );
            }
        }
        file.close();
    }
}
