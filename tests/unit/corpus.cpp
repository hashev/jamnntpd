#include "core/codec.hpp"
#include "core/jam_format.hpp"
#include "core/mime.hpp"
#include "core/text.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

using namespace jamnntpd;
namespace {
auto seed_file(std::string_view name) -> std::string {
    std::ifstream file{std::filesystem::path{JAM_FIXTURE_DIR} / name, std::ios::binary};
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
auto mutate_seed(std::string value, std::mt19937 &random) -> std::string {
    if (value.empty()) {
        return value;
    }
    const auto position = static_cast<std::size_t>(random()) % value.size();
    value.at(position) = static_cast<char>(random() & 255U);
    if (random() % 4 == 0) {
        value.resize(position);
    }
    return value;
}
} // namespace
TEST_CASE("Seeded corrupt JAM corpus never escapes validated record boundaries") {
    std::mt19937 random{0x20261023U};
    for (auto name : std::array{"jam/base-header.bin", "jam/message-header.bin", "jam/index.bin",
                                "jam/lastread.bin", "jam/subfields.bin"}) {
        const auto original = seed_file(name);
        for (int iteration = 0; iteration < 200; ++iteration) {
            const auto bytes = jam::bytes_from(mutate_seed(original, random));
            if (auto base = jam::decode_base(bytes)) {
                CHECK(jam::encode_base(*base) == bytes);
            }
            if (auto message = jam::decode_message(bytes)) {
                CHECK(jam::encode_message(*message) == bytes);
            }
            if (auto index = jam::decode_index(bytes)) {
                CHECK(jam::encode_index(*index) == bytes);
            }
            if (auto lastread = jam::decode_lastread(bytes)) {
                CHECK(jam::encode_lastread(*lastread) == bytes);
            }
            if (auto fields = jam::decode_subfields(bytes)) {
                CHECK(jam::encode_subfields(*fields) == bytes);
            }
        }
    }
}
TEST_CASE("Seeded MIME CHS and config corpus retains its validated invariants") {
    std::mt19937 random{0x20261024U};
    const auto chs = seed_file("chs/cp866-to-utf8.chs");
    for (int iteration = 0; iteration < 200; ++iteration) {
        const auto table = parse_chs(mutate_seed(chs, random), "fuzz.chs");
        if (table) {
            for (const auto &entry : table->entries) {
                CHECK(entry.size() <= 4);
                CHECK_FALSE(entry.contains('\0'));
            }
        } else {
            CHECK(table.error().message.find("fuzz.chs:") != std::string::npos);
        }
        const auto header = mutate_seed(
            "Subject: =?UTF-8?B?0J0=?=\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\nН─",
            random);
        const auto article = parse_article(header, {"UTF-8"});
        if (article) {
            const auto validated = transcode(article->body, {"UTF-8"}, {"UTF-8"});
            REQUIRE(validated);
            CHECK(validated->bytes == article->body);
        }
        const auto config =
            config_words(mutate_seed("groups \"a path with spaces\" !CP866", random));
        if (config) {
            for (const auto &word : *config) {
                CHECK(word.size() <= 35);
            }
        }
    }
}
