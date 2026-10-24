#ifndef JAMNNTPD_TEST_TEMP_DIRECTORY_HPP
#define JAMNNTPD_TEST_TEMP_DIRECTORY_HPP
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

class TempDirectory {
  public:
    TempDirectory() {
        std::random_device random;
        for (int attempt = 0; attempt < 20; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("jamnntpd-test-" + std::to_string(random()));
            if (std::filesystem::create_directory(path_)) {
                return;
            }
        }
        FAIL("Cannot create isolated test directory");
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TempDirectory(const TempDirectory &) = delete;
    auto operator=(const TempDirectory &) -> TempDirectory & = delete;
    TempDirectory(TempDirectory &&) = delete;
    auto operator=(TempDirectory &&) -> TempDirectory & = delete;
    auto file(std::string_view name) const -> std::filesystem::path { return path_ / name; }
    auto write(std::string_view name, std::string_view contents) const -> std::filesystem::path {
        auto destination = file(name);
        std::ofstream output{destination, std::ios::binary};
        REQUIRE(output.good());
        output << contents;
        output.close();
        REQUIRE(output.good());
        return destination;
    }

  private:
    std::filesystem::path path_;
};
#endif
