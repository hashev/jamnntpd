#include "core/server.hpp"
#include <algorithm>
#include <exception>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

auto main(int argc, char **argv) -> int {
    try {
        std::vector<std::string> arguments;
        const auto native_arguments = std::span{argv, static_cast<std::size_t>(argc)};
        for (const auto *argument :
             native_arguments.subspan(std::min<std::size_t>(1, native_arguments.size()))) {
            arguments.emplace_back(argument);
        }
        auto command = jamnntpd::parse_command_line(arguments);
        if (!command) {
            std::cerr << command.error().message << '\n';
            return 1;
        }
        if (command->action == jamnntpd::CommandAction::help) {
            std::cout << jamnntpd::command_help();
            return 0;
        }
        if (command->action == jamnntpd::CommandAction::create_config) {
            std::ofstream output{command->output_file};
            output << jamnntpd::config_text(command->config);
            output.flush();
            if (!output) {
                std::cerr << "Cannot write configuration file\n";
                return 1;
            }
            return 0;
        }
        const auto result = jamnntpd::run_server(command->config);
        if (!result) {
            std::cerr << result.error().message << '\n';
            return 1;
        }
        return 0;
    } catch (const std::exception &) {
        std::cerr << "JamNNTPd: fatal startup or control-thread failure\n";
        return 1;
    }
}
