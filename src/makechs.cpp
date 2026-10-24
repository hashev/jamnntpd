// Modernized makechs; original utility by Johan Billing, with Markus Kuhn's table.
#include "core/map_generator.hpp"
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

auto main(int argc, char **argv) -> int {
    constexpr int argument_count_with_target_map = 5;
    try {
        if (argc != 4 && argc != argument_count_with_target_map) {
            std::cerr << "Usage: makechs <fromchrs> <destchrs> <frommap> [<destmap>]\n";
            return 0;
        }
        std::vector<std::string_view> arguments;
        for (const auto *argument : std::span{argv, static_cast<std::size_t>(argc)}) {
            arguments.emplace_back(argument);
        }
        auto source = jamnntpd::read_unicode_map(std::filesystem::path{arguments.at(3)});
        if (!source) {
            std::cerr << source.error().message << '\n';
            return 1;
        }
        std::optional<jamnntpd::UnicodeMap> destination;
        if (argc == argument_count_with_target_map) {
            auto target = jamnntpd::read_unicode_map(std::filesystem::path{arguments.at(4)});
            if (!target) {
                std::cerr << target.error().message << '\n';
                return 1;
            }
            destination = *target;
        }
        auto generated =
            jamnntpd::generate_chs(*source, destination, {arguments.at(1)}, {arguments.at(2)});
        if (!generated) {
            std::cerr << generated.error().message << '\n';
            return 1;
        }
        for (const auto &warning : generated->warnings) {
            std::cerr << "Warning: " << warning << '\n';
        }
        std::cout << generated->text;
        return std::cout.good() ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
