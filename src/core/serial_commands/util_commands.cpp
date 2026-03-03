#include "util_commands.h"
#include "helpers.h"
#include <globals.h>
uint32_t restartCallback(cmd *c) { ESP.restart(); return true; }
void createUtilCommands(SimpleCLI *cli) { cli->addCommand("restart", restartCallback); }
