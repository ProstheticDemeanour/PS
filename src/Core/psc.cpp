#include "command_registry.hpp"
#include "vi_command.hpp"
#include <iostream>
#include <memory>

int main(int argc, char* argv[]) {
    CommandRegistry registry;

	registry.registerCommand(std::make_unique<vi_command>(nullptr)); //no config needed for vi

    // Handle no arguments
    if (argc < 2) {
        registry.showGlobalHelp();
        return 1;
    }

    // Look up the requested command
    std::string commandName = argv[1];
    BaseCommand* command = registry.getCommand(commandName);

    if (!command) {
        std::cerr << "Unknown command: " << commandName << "\n";
        registry.listCommands();
        return 1;
    }

    // Prepare arguments for the command (exclude program name and command name)
    std::vector<std::string> commandArgs;
    for (int i = 2; i < argc; ++i) {
        commandArgs.push_back(argv[i]);
    }

    try {
    	return command->execute(commandArgs);

	} catch (const std::exception& e) {

    	std::cerr << "Error: " << e.what() << "\n";
    	return 1;
	}
}