#include "command_registry.hpp"
#include <algorithm>
#include <iomanip>


void CommandRegistry::registerCommand(std::unique_ptr<BaseCommand> command) {

	if (!command) {

		std::cerr << "Warning: Attempt to register null command\n";
		return;
	}

	std::string name = command -> getName();

	//check for duplicate commands
	if (commands.find(name) != commands.end()) {

		std::cerr << "Warning: Command" << name << "is already registered. Overwriting, \n";
	}

	//store the command
	commands[name] = std::move(command);
}

BaseCommand* CommandRegistry::getCommand(const std::string& name) {
    auto it = commands.find(name);
    if (it != commands.end()) {
        return it->second.get(); // Return raw pointer to the command
    }
    return nullptr; // Command not found
}

bool CommandRegistry::hasCommand(const std::string& name) const {
    return commands.find(name) != commands.end();
}

void CommandRegistry::listCommands() const {
    if (commands.empty()) {
        std::cout << "No commands available.\n";
        return;
    }
    
    std::cout << "Available commands:\n";
    for (const auto& [name, command] : commands) {
        std::cout << "  " << std::left << std::setw(15) << name 
                  << " - " << command->getDescription() << "\n";
    }
}

void CommandRegistry::showGlobalHelp() const {
    std::cout << "Dataknot Financial Analysis Tool\n\n";
    std::cout << "Usage: dataknot <command> [options]\n\n";
    listCommands();
    std::cout << "\nUse 'dataknot <command> --help' for help on a specific command.\n";
}

std::vector<std::string> CommandRegistry::getCommandNames() const {
    std::vector<std::string> names;
    names.reserve(commands.size());
    
    for (const auto& [name, command] : commands) {
        names.push_back(name);
    }
    
    // Sort alphabetically for consistent output
    std::sort(names.begin(), names.end());
    return names;
}

size_t CommandRegistry::getCommandCount() const {
    return commands.size();
}