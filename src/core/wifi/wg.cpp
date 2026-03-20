bool isConnectedWireguard = false;
#ifndef LITE_VERSION
#include "wg.h"
#include "core/display.h"
#include "core/sd_functions.h"
#include "core/wifi/wifi_common.h"
#include <WireGuard-ESP32.h>
#include <globals.h>

char private_key[45];
IPAddress local_ip;
char public_key[45];
char endpoint_address[16];
int endpoint_port = 31337;

static constexpr const uint32_t UPDATE_INTERVAL_MS = 5000;

static WireGuard wg;

/*********************************************************************
**  Function: parse_config_file
**  parses wireguard config file wg.conf
**********************************************************************/
void parse_config_file(File configFile) {
    String line;

    while (configFile.available()) {
        line = configFile.readStringUntil('\n');
        line.trim();

        if (line.startsWith("[Interface]") || line.isEmpty()) {
            // Skip [Interface] or empty lines
            continue;
        } else if (line.startsWith("PrivateKey")) {
            line.remove(0, line.indexOf('=') + 1);
            line.trim();
            strncpy(private_key, line.c_str(), sizeof(private_key) - 1);
            private_key[sizeof(private_key) - 1] = '\0'; // Ensure null-terminated
        } else if (line.startsWith("Address")) {
            line.remove(0, line.indexOf('=') + 1);
            line.trim();
            int slashIndex = line.indexOf('/');

            if (slashIndex != -1) { local_ip.fromString(line.substring(0, slashIndex)); }

        } else if (line.startsWith("[Peer]")) {
            // add [Peer] section
        } else if (line.startsWith("PublicKey")) {
            line.remove(0, line.indexOf('=') + 1);
            line.trim();
            strncpy(public_key, line.c_str(), sizeof(public_key) - 1);
            public_key[sizeof(public_key) - 1] = '\0'; // Ensure null-terminated
        } else if (line.startsWith("Endpoint")) {
            line.remove(0, line.indexOf('=') + 1);
            line.trim();
            int colonIndex = line.indexOf(':');

            if (colonIndex != -1) {
                // Serial.println("Endpoint Line: " + line);
                strncpy(
                    endpoint_address, line.substring(0, colonIndex).c_str(), sizeof(endpoint_address) - 1
                );
                endpoint_address[sizeof(endpoint_address) - 1] = '\0'; // Ensure null-terminated
                endpoint_port = line.substring(colonIndex + 1).toInt();
            }
        }
    }

    configFile.close();
}

/*********************************************************************
**  Function: read_and_parse_file
**  tries to open file wg.conf on local SD
**********************************************************************/
void read_and_parse_file() {
    if (!setupSdCard()) {
        sdcardMounted = false;
        Serial.println("Failed to initialize SD card");
        return;
    }

    File file = SD.open("/wg.conf");
    if (!file) {
        tft.fillScreen(bruceConfig.bgColor);
        tft.setCursor(0, 0);

        tft.setTextColor(TFT_RED, bruceConfig.bgColor);
        Serial.println("Failed to open wg.conf file");
        // tft.println("No wg.conf file\nfound on\nthe SD");
        displayRedStripe("No wg.conf file", TFT_RED, bruceConfig.priColor);
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        delay(6000);
        return;
    }

    parse_config_file(file);
}

/*********************************************************************
**  Function: wg_setup
**  connect to wireguard tunnel
**********************************************************************/
void wg_setup() {
    if (!wifiConnected) wifiConnectMenu();

    read_and_parse_file();

    Serial.println("Adjusting system time...");
    configTime(9 * 60 * 60, 0, "ntp.jst.mfeed.ad.jp", "ntp.nict.jp");
    tft.fillScreen(bruceConfig.bgColor);
    tft.setCursor(0, 0);

    Serial.println("Connected. Initializing WireGuard...");
    // tft.println("Connecting to\nwireguard...");
    wg.begin(local_ip, private_key, endpoint_address, public_key, endpoint_port);

    tft.fillScreen(bruceConfig.bgColor);
    tft.setCursor(0, 0);
    tft.setTextSize(FG);

    tft.setTextColor(TFT_GREEN, bruceConfig.bgColor);
    tft.println("Connected!");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.println("IP on tunnel:");
    tft.setTextColor(TFT_WHITE, bruceConfig.bgColor);
    tft.println(local_ip);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    delay(7000);
    isConnectedWireguard = true;
    tft.fillScreen(bruceConfig.bgColor);
}
#endif
