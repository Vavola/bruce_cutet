#ifndef CLI_H
#define CLI_H
#include <SimpleCLI.h>
#include <WString.h>
class SerialCli {
public:
    SerialCli();
    void setup();
    bool parse(String input);
private:
    SimpleCLI _cli;
};
extern SerialCli serialCli;
#endif
