#include "core/codec.hpp"
#include "core/jam_format.hpp"
#include "core/mime.hpp"
#include "core/text.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace jamnntpd;
namespace {
auto environment(const char *name) -> std::optional<std::string> {
#ifdef _WIN32
    std::size_t size{};
    REQUIRE(::getenv_s(&size, nullptr, 0, name) == 0);
    if (size == 0) {
        return std::nullopt;
    }
    std::string value(size, '\0');
    REQUIRE(::getenv_s(&size, value.data(), value.size(), name) == 0);
    value.resize(size - 1);
    return value;
#else
    const auto *value = std::getenv(name);
    return value == nullptr ? std::nullopt : std::optional<std::string>{value};
#endif
}
auto parameter(const char *name, std::uint32_t fallback, std::uint32_t maximum) -> std::uint32_t {
    const auto value = environment(name);
    if (!value) {
        return fallback;
    }
    const auto parsed = parse_number(*value, maximum);
    REQUIRE(parsed);
    return *parsed;
}
auto invariant(std::string_view family, const std::string &input) -> bool {
    if (family == "codec") {
        const auto unicode = transcode(input, {"CP866"}, {"UTF-8"});
        if (!unicode) {
            return false;
        }
        const auto restored = transcode(unicode->bytes, {"UTF-8"}, {"CP866"});
        return restored && restored->bytes == input;
    }
    if (family == "jam") {
        const auto bytes = jam::bytes_from(input);
        if (auto value = jam::decode_base(bytes); value && jam::encode_base(*value) != bytes) {
            return false;
        }
        if (auto value = jam::decode_message(bytes);
            value && jam::encode_message(*value) != bytes) {
            return false;
        }
        if (auto value = jam::decode_subfields(bytes);
            value && jam::encode_subfields(*value) != bytes) {
            return false;
        }
        return true;
    }
    if (family == "parser") {
        if (auto article = parse_article(input, {"UTF-8"})) {
            const auto valid = transcode(article->body, {"UTF-8"}, {"UTF-8"});
            if (!valid || valid->bytes != article->body) {
                return false;
            }
        }
        if (auto table = parse_chs(input, "generated.chs")) {
            for (const auto &entry : table->entries) {
                if (entry.size() > 4 || entry.contains('\0')) {
                    return false;
                }
            }
        }
        if (auto words = config_words(input)) {
            std::string quoted;
            for (const auto &word : *words) {
                if (word.contains('"')) {
                    return true; // Embedded quotes have no escaping syntax in this grammar.
                }
                quoted += '"' + word + "\" ";
            }
            const auto again = config_words(quoted);
            return again && *again == *words;
        }
        return true;
    }
    FAIL("Unknown generated family");
    return false;
}
} // namespace
TEST_CASE("Generated parser JAM and codec families retain invariants with replayable seeds") {
    const auto count = parameter("JAM_TEST_CASES", 1000, 10000);
    REQUIRE(count >= 1000);
    const auto seed = parameter("JAM_TEST_SEED", 20261026, 0xffffffffU);
    for (const auto *family : {"parser", "jam", "codec"}) {
        std::mt19937 random{seed};
        std::vector<std::string> seeds;
        if (std::string_view{family} == "jam") {
            for (const auto *name : {"base-header.bin", "message-header.bin", "subfields.bin"}) {
                std::ifstream input{std::filesystem::path{JAM_FIXTURE_DIR} / "jam" / name,
                                    std::ios::binary};
                REQUIRE(input.good());
                seeds.emplace_back(std::istreambuf_iterator<char>{input},
                                   std::istreambuf_iterator<char>{});
            }
        } else if (std::string_view{family} == "parser") {
            seeds.emplace_back(
                "Subject: Test\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\nН─");
            seeds.emplace_back("groups \"path with spaces\" !CP866");
            std::ifstream input{std::filesystem::path{JAM_FIXTURE_DIR} / "chs/cp866-to-utf8.chs",
                                std::ios::binary};
            REQUIRE(input.good());
            seeds.emplace_back(std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{});
        }
        for (std::uint32_t index = 0; index < count; ++index) {
            std::string input(static_cast<std::size_t>(random() % 513), '\0');
            for (auto &byte : input) {
                byte = static_cast<char>(random() & 255U);
            }
            if (!seeds.empty() && index % 2 == 0) {
                input = seeds.at(static_cast<std::size_t>(index) % seeds.size());
                if (!input.empty()) {
                    const auto position = static_cast<std::size_t>(random()) % input.size();
                    input.at(position) = static_cast<char>(random() & 255U);
                    if (random() % 4 == 0) {
                        input.resize(position);
                    }
                }
            }
            INFO("family=" << family << " seed=" << seed << " index=" << index);
            if (!invariant(family, input)) {
                const auto configured = environment("JAM_TEST_ARTIFACTS");
                const auto directory = !configured ? std::filesystem::path{"failure-corpus"}
                                                   : std::filesystem::path{*configured};
                std::filesystem::create_directories(directory);
                const auto path = directory / (std::string{family} + "-" + std::to_string(seed) +
                                               "-" + std::to_string(index) + ".bin");
                std::ofstream output{path, std::ios::binary};
                output.write(input.data(), static_cast<std::streamsize>(input.size()));
                output.close();
                FAIL("Invariant failure; replay artifact: " << path.string());
            }
        }
    }
}
TEST_CASE("Replay a minimized generated case", "[.replay]") {
    const auto filename = environment("JAM_REPLAY_FILE");
    const auto family = environment("JAM_REPLAY_FAMILY");
    REQUIRE(filename);
    REQUIRE(family);
    REQUIRE(std::filesystem::file_size(*filename) <= 1024U * 1024U);
    std::ifstream input{*filename, std::ios::binary};
    REQUIRE(input.good());
    const std::string data{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    REQUIRE(invariant(*family, data));
}
