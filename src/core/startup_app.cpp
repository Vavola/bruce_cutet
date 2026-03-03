#include "startup_app.h"
#include "core/settings.h" 
#include "core/wifi/wifi_common.h"
#include "modules/gps/gps_tracker.h"
#include "modules/gps/wardriving.h"
#include "modules/rf/rf_send.h"
#include "modules/rfid/PN532KillerTools.h"
#include "modules/rfid/pn532ble.h"
#include "modules/wifi/sniffer.h"
#ifdef SOC_USB_OTG_SUPPORTED
#include "core/massStorage.h"
#endif

StartupApp::StartupApp() {
#ifndef LITE_VERSION
    _startupApps["Sniffer"] = []() { sniffer_setup(); };
    _startupApps["GPS Tracker"] = []() { GPSTracker(); };
    _startupApps["PN532 BLE"] = []() { Pn532ble(); };
    _startupApps["PN532 UART"] = []() { PN532KillerTools(); };
#endif
    _startupApps["Clock"] = []() { runClockLoop(); };
    _startupApps["Custom SubGHz"] = []() { sendCustomRF(); };
#if defined(SOC_USB_OTG_SUPPORTED)
    _startupApps["Mass Storage"] = []() { MassStorage(); };
#endif
    _startupApps["Wardriving"] = []() { Wardriving(); };
}

bool StartupApp::startApp(const String &appName) const {
    auto it = _startupApps.find(appName);
    if (it == _startupApps.end()) {
        Serial.println("Invalid startup app: " + appName);
        return false;
    }
    it->second();
    delay(200);
    tft.fillScreen(bruceConfig.bgColor);
    return true;
}

std::vector<String> StartupApp::getAppNames() const {
    std::vector<String> keys;
    for (const auto &pair : _startupApps) { keys.push_back(pair.first); }
    return keys;
}
