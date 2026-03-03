#include "WifiMenu.h"
#include "core/display.h"
#include <esp_netif.h>
#include "core/settings.h"
#include "core/utils.h"
#include "core/wifi/wg.h"
#include "core/wifi/wifi_common.h"
#include "core/wifi/wifi_mac.h"
#include "modules/ethernet/ARPScanner.h"
#include "modules/wifi/ap_info.h"
#include "modules/wifi/clients.h"
#include "modules/wifi/karma_attack.h"
#include "modules/wifi/responder.h"
#include "modules/wifi/scan_hosts.h"
#include "modules/wifi/sniffer.h"
#include "modules/wifi/wifi_atks.h"

#ifndef LITE_VERSION
#include "modules/wifi/wifi_recover.h"
#endif

#include "modules/wifi/tcp_utils.h"

bool showHiddenNetworks = false;

void WifiMenu::optionsMenu() {
    returnToMenu = false;
    options.clear();

    if (WiFi.status() != WL_CONNECTED) {
        options = {
            {"Connect to Wifi", lambdaHelper(wifiConnectMenu, WIFI_STA)},
            {"Start WiFi AP", [=]() {
                 wifiConnectMenu(WIFI_AP);
                 displayInfo("pwd: " + bruceConfig.wifiAp.pwd, true);
             }},
        };
    }
    if (WiFi.getMode() != WIFI_MODE_NULL) { options.push_back({"Turn Off WiFi", wifiDisconnect}); }
    if (WiFi.getMode() == WIFI_MODE_STA || WiFi.getMode() == WIFI_MODE_APSTA) {
        options.push_back({"AP info", displayAPInfo});
    }
    options.push_back({"Wifi Atks", wifi_atk_menu});
    
#ifndef LITE_VERSION
    options.push_back({"Listen TCP", listenTcpPort});
    options.push_back({"Client TCP", clientTCP});
    options.push_back({"TelNET", telnet_setup});
    options.push_back({"SSH", lambdaHelper(ssh_setup, String(""))});
    options.push_back({"Sniffers", [this]() {
                           std::vector<Option> snifferOptions;
                           snifferOptions.push_back({"Raw Sniffer", sniffer_setup});
                           snifferOptions.push_back({"Probe Sniffer", karma_setup});
                           snifferOptions.push_back({"Back", [this]() { optionsMenu(); }});
                           loopOptions(snifferOptions, MENU_TYPE_SUBMENU, "Sniffers");
                       }});
    options.push_back({"Scan Hosts", [=]() {
                           bool doScan = true;
                           if (!wifiConnected) doScan = wifiConnectMenu();

                           if (doScan) {
                               esp_netif_t *esp_netinterface = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                               if (esp_netinterface == nullptr) {
                                   Serial.println("Failed to get netif handle");
                                   return;
                               }
                               ARPScanner{esp_netinterface};
                           }
                       }});
    options.push_back({"Wireguard", wg_setup});
    options.push_back({"Responder", responder});
    options.push_back({"WiFi Pass Recovery", wifi_recover_menu});
#endif
    
    options.push_back({"Config", [this]() { configMenu(); }});
    addOptionToMainMenu();
    loopOptions(options, MENU_TYPE_SUBMENU, "WiFi");
    options.clear();
}

void WifiMenu::configMenu() {
    std::vector<Option> wifiOptions;
    wifiOptions.push_back({"Change MAC", wifiMACMenu});
    {
        String hidden__wifi_option = String("Hidden Networks:") + (showHiddenNetworks ? "ON" : "OFF");
        Option opt(hidden__wifi_option.c_str(), [this]() {
            showHiddenNetworks = !showHiddenNetworks;
            displayInfo(String("Hidden Networks:") + (showHiddenNetworks ? "ON" : "OFF"), true);
            configMenu();
        });
        wifiOptions.push_back(opt);
    }
    wifiOptions.push_back({"Back", [this]() { optionsMenu(); }});
    loopOptions(wifiOptions, MENU_TYPE_SUBMENU, "WiFi Config");
}

void WifiMenu::drawIcon(float scale) {
    clearIconArea();
    int deltaY = scale * 20;
    int radius = scale * 6;
    tft.fillCircle(iconCenterX, iconCenterY + deltaY, radius, bruceConfig.priColor);
    tft.drawArc(iconCenterX, iconCenterY + deltaY, deltaY + radius, deltaY, 130, 230, bruceConfig.priColor, bruceConfig.bgColor);
    tft.drawArc(iconCenterX, iconCenterY + deltaY, 2 * deltaY + radius, 2 * deltaY, 130, 230, bruceConfig.priColor, bruceConfig.bgColor);
}
