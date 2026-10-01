#ifndef COMMAND_REGISTRY_HPP
#define COMMAND_REGISTRY_HPP

#include "base_command.hpp"
#include <map>
#include <memory>
#include <string>
#include <iostream>

class CommandRegistry {
private:
    std::map<std::string, std::unique_ptr<BaseCommand>> commands; //actual storage, maps command name to command object
    
public:
    void registerCommand(std::unique_ptr<BaseCommand> command);
    BaseCommand* getCommand(const std::string& name);
    void listCommands() const;
    bool hasCommand(const std::string& name) const;
    void showGlobalHelp() const;
    std::vector<std::string> getCommandNames() const;
    size_t getCommandCount() const;
};

#endif