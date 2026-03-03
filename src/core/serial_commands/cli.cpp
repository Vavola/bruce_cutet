#include "cli.h"
#include "crypto_commands.h"
#include "gpio_commands.h"
#include "ir_commands.h"
#include "power_commands.h"
#include "rf_commands.h"
#include "screen_commands.h"
#include "settings_commands.h"
#include "sound_commands.h"
#include "storage_commands.h"
#include "util_commands.h"
#include "wifi_commands.h"
#include <globals.h>

void cliErrorCallback(cmd_error *e) {
    CommandError cmdError(e);
    serialDevice->print("ERROR: ");
    serialDevice->println(cmdError.toString());
    if (cmdError.hasCommand()) {
        serialDevice->print("Did you mean \"");
        serialDevice->print(cmdError.getCommand().toString());
        serialDevice->println("\"?");
    }
}
SerialCli::SerialCli() { setup(); }
void SerialCli::setup() {
    _cli.setOnError(cliErrorCallback);
    createCryptoCommands(&_cli);
    createGpioCommands(&_cli);
    createIrCommands(&_cli);
    createPowerCommands(&_cli);
    createRfCommands(&_cli);
    createSettingsCommands(&_cli);
    createStorageCommands(&_cli);
    createUtilCommands(&_cli);
    createWifiCommands(&_cli);
#ifdef HAS_SCREEN
    createScreenCommands(&_cli);
#endif
#if defined(HAS_NS4168_SPKR)
    createSoundCommands(&_cli);
#endif
}
bool SerialCli::parse(String input) {
    input.trim();
    if (input.length() > 0) {
        serialDevice->println("Parsing: " + input);
        _cli.parse(input);
        return true;
    }
    return false;
}
