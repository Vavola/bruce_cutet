#include "pc_sync.h"
#include "core/display.h"
#include "core/led_control.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include <vector>

// ================= НАСТРОЙКИ СИНХРОНИЗАЦИИ =================
const char *HOTSPOT_SSID = "Lox";      // <-- ВПИШИ ИМЯ ТОЧКИ ДОСТУПА ТЕЛЕФОНА
const char *HOTSPOT_PASS = "10101010"; // <-- ВПИШИ ПАРОЛЬ ОТ ТЕЛЕФОНА
const char *NGROK_URL = "https://unpolitical-johnson-feverish.ngrok-free.dev/upload";
// ===========================================================

void pc_sync_start() {
    Serial.println("\n[PC Sync] === STARTING PCAP SYNCHRONIZER ===\n");

    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_MODE_STA);
    WiFi.disconnect();
    setLedAttackMode(0, CRGB_Colors::Black);

    std::vector<String> files;
    File dir = SD.open("/BrucePCAP/handshakes");
    if (dir) {
        while (File file = dir.openNextFile()) {
            if (!file.isDirectory() && String(file.name()).endsWith(".pcap")) {
                files.push_back(String(file.name()));
            }
            file.close();
        }
        dir.close();
    }

    if (files.empty()) {
        tft.fillScreen(bruceConfig.bgColor);
        tft.setTextColor(bruceConfig.priColor);
        tft.setTextSize(2);
        tft.setCursor(10, tftHeight / 2 - 10);
        tft.print("No PCAP files found!");
        delay(2000);
        return;
    }

    int selected = 0;
    bool in_menu = true;

    while (in_menu) {
        tft.fillScreen(bruceConfig.bgColor);
        tft.setTextColor(bruceConfig.priColor);
        tft.setTextSize(2);
        tft.setCursor(10, 10);
        tft.print("Select PCAP to Sync:");

        int startIdx = (selected / 8) * 8;
        for (int i = 0; i < 8 && (startIdx + i) < files.size(); i++) {
            int idx = startIdx + i;
            tft.setCursor(10, 45 + i * 20);

            String display_name = files[idx];
            if (display_name.length() > 22) display_name = display_name.substring(0, 19) + "...";

            if (idx == selected) {
                tft.setTextColor(bruceConfig.bgColor, bruceConfig.priColor);
                tft.print("> " + display_name);
                tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            } else {
                tft.print("  " + display_name);
            }
        }

        tft.setTextSize(1);
        tft.setCursor(10, tftHeight - 15);
        tft.print("UP/DOWN: Move | SEL: Send | ESC: Exit");

        while (true) {
            if (check(UpPress)) {
                selected = (selected > 0) ? selected - 1 : files.size() - 1;
                break;
            }
            if (check(DownPress)) {
                selected = (selected < files.size() - 1) ? selected + 1 : 0;
                break;
            }
            if (check(EscPress)) {
                in_menu = false;
                break;
            }
            if (check(SelPress)) {

                tft.fillScreen(bruceConfig.bgColor);
                tft.setTextSize(2);
                tft.setCursor(10, 10);
                tft.print("Connecting to Hotspot...");
                tft.setCursor(10, 40);
                tft.print("SSID: ");
                tft.print(HOTSPOT_SSID);

                WiFi.disconnect(true);
                delay(500);
                WiFi.begin(HOTSPOT_SSID, HOTSPOT_PASS);
                WiFi.setSleep(false); // <--- ОТКЛЮЧАЕМ ЭНЕРГОСБЕРЕЖЕНИЕ

                int attempts = 0;
                tft.setCursor(10, 70);
                while (WiFi.status() != WL_CONNECTED && attempts < 20) {
                    delay(500);
                    tft.print(".");
                    attempts++;
                }

                if (WiFi.status() == WL_CONNECTED) {
                    tft.fillScreen(bruceConfig.bgColor);
                    tft.setCursor(10, 10);
                    tft.print("WiFi Connected!");
                    tft.setCursor(10, 40);
                    tft.print("Uploading file...");

                    WiFiClientSecure client;
                    client.setInsecure();
                    client.setHandshakeTimeout(20000); // <--- ТАЙМАУТ РУКОПОЖАТИЯ

                    HTTPClient http;
                    http.setTimeout(20000);
                    http.begin(client, NGROK_URL);
                    http.addHeader("ngrok-skip-browser-warning", "true");
                    http.addHeader("Connection", "close"); // <--- ЗАКРЫВАЕМ СОЕДИНЕНИЕ ПОСЛЕ

                    String boundary = "----BrucegotchiBoundary";
                    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

                    String head = "--" + boundary +
                                  "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" +
                                  files[selected] + "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
                    String tail = "\r\n--" + boundary + "--\r\n";

                    File f = SD.open("/BrucePCAP/handshakes/" + files[selected], FILE_READ);
                    if (f) {
                        size_t fSize = f.size();
                        size_t totLen = head.length() + fSize + tail.length();

                        uint8_t *payload = (uint8_t *)malloc(totLen);
                        if (payload) {
                            memcpy(payload, head.c_str(), head.length());
                            f.read(payload + head.length(), fSize);
                            memcpy(payload + head.length() + fSize, tail.c_str(), tail.length());
                            f.close();

                            delay(100); // <--- ПАУЗА ПЕРЕД ВЫСТРЕЛОМ
                            int httpResponseCode = http.POST(payload, totLen);

                            tft.fillScreen(bruceConfig.bgColor);
                            tft.setCursor(10, 10);

                            if (httpResponseCode > 0) {
                                String server_response = http.getString();

                                if (httpResponseCode == 200) {
                                    if (server_response.indexOf("OK") >= 0) {
                                        tft.setTextColor(TFT_GREEN, bruceConfig.bgColor);
                                    } else {
                                        tft.setTextColor(TFT_YELLOW, bruceConfig.bgColor);
                                    }
                                    tft.print(server_response);
                                } else {
                                    tft.setTextColor(TFT_RED, bruceConfig.bgColor);
                                    tft.print("HTTP: ");
                                    tft.print(httpResponseCode);
                                }
                            } else {
                                tft.setTextColor(TFT_RED, bruceConfig.bgColor);
                                tft.print("Net Err: ");
                                tft.print(httpResponseCode);
                                tft.setCursor(10, 40);
                                tft.setTextSize(1);
                                tft.print(http.errorToString(httpResponseCode).c_str());
                            }
                            free(payload);
                        } else {
                            tft.setCursor(10, 70);
                            tft.print("RAM Allocation Failed!");
                            f.close();
                        }
                    } else {
                        tft.setCursor(10, 70);
                        tft.print("Failed to open file!");
                    }
                    http.end();
                } else {
                    tft.setCursor(10, 70);
                    tft.setTextColor(TFT_RED, bruceConfig.bgColor);
                    tft.print("WiFi Connection Failed!");
                }

                WiFi.disconnect(true);
                tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
                tft.setTextSize(2);
                tft.setCursor(10, 110);
                tft.print("Press ANY KEY");
                while (!check(EscPress) && !check(SelPress) && !check(UpPress) && !check(DownPress)) {
                    delay(50);
                }
                break;
            }
            delay(50);
        }
    }

    tft.fillScreen(bruceConfig.bgColor);
}
