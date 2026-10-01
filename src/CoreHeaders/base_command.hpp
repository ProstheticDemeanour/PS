#ifndef BASE_COMMAND_HPP
#define BASE_COMMAND_HPP

#include <vector>
#include <string>
#include "config.h"

class BaseCommand {
protected:
    const SoftwareConfig* config; //store reference to config


public:
    BaseCommand(const SoftwareConfig* cfg) : config(cfg) {}
    virtual ~BaseCommand() = default;
    
    // Pure virtual functions that every command must implement
    virtual std::string getName() const = 0;
    virtual std::string getDescription() const = 0;
    virtual int execute(const std::vector<std::string>& args) = 0;
    virtual void showHelp() const = 0;
};

#endif // BASE_COMMAND_HPP