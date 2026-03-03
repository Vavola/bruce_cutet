#include "wifi_commands.h"
#include "core/wifi/wifi_common.h"
#include "helpers.h"
#include <globals.h>
uint32_t wifiConnectCallback(cmd *c) { wifiConnectMenu(); return true; }
void createWifiCommands(SimpleCLI *cli) {
    Command wifiCmd = cli->addCompositeCmd("wifi");
    wifiCmd.addCommand("connect", wifiConnectCallback);
}
