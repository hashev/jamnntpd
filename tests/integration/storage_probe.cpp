#include "core/storage.hpp"
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>

int main(int argc, char **argv) {
    try {
        if (argc != 2) {
            return 2;
        }
        const auto arguments = std::span{argv, static_cast<std::size_t>(argc)};
        const auto storage = jamnntpd::jam::Storage::open(
            std::filesystem::path{arguments[1]},
            {.lock_timeout = std::chrono::milliseconds{100}, .fail_io = {}});
        if (!storage) {
            std::cerr << storage.error().message << '\n';
            return storage.error().kind == jamnntpd::ErrorKind::timeout ? 3 : 1;
        }
        const auto info = storage->info();
        if (!info) {
            return 1;
        }
        std::cout << info->slots << ' ' << info->header.active << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
