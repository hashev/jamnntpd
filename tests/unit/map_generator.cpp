#include "core/map_generator.hpp"
#include "temp_directory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>
using namespace jamnntpd;
namespace {
auto identity_map() -> UnicodeMap {
    UnicodeMap result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result.at(index) = static_cast<char32_t>(index);
    }
    return result;
}
} // namespace
TEST_CASE("makechs Unicode maps preserve defaults and support supplementary code points") {
    TempDirectory directory;
    auto map = read_unicode_map(directory.write(
        "source.map", "# test\n0x8D 0x041D # LETTER EN\n0xC4 0x2500\n0xFF 0x1F600\n"));
    REQUIRE(map);
    CHECK(map->at(0x8d) == U'Н');
    CHECK(map->at('A') == U'A');
    auto generated = generate_chs(*map, std::nullopt, {"CUSTOM"}, {"UTF-8"});
    REQUIRE(generated);
    CHECK(generated->warnings.empty());
    auto table = parse_chs(generated->text, "generated.chs");
    REQUIRE(table);
    CHECK(table->apply("\x8d\xc4\xff") == "Н─😀");
    for (auto invalid : {"x80 0x100\n", "0x100 0x100\n", "0x80 0xD800\n", "0x80 0x110000\n",
                         "0x80 junk\n", "0x80 0x100 extra\n"}) {
        CHECK_FALSE(read_unicode_map(directory.write("invalid.map", invalid)));
    }
    CHECK_FALSE(read_unicode_map(directory.file("missing.map")));
}
TEST_CASE("makechs identical maps generate a valid classic table") {
    const auto map = identity_map();
    auto generated = generate_chs(map, map, {"SOURCE"}, {"DEST"});
    REQUIRE(generated);
    auto table = parse_chs(generated->text, "identity.chs");
    REQUIRE(table);
    CHECK(table->apply("A\x80\xff") == "A\x80\xff");
}
TEST_CASE("makechs preserves explicit historical fallback warnings") {
    auto map = identity_map();
    map.at(0x80) = U'€';
    map.at(0x81) = U'😀';
    auto generated = generate_chs(map, identity_map(), {"SOURCE"}, {"DEST"});
    REQUIRE(generated);
    REQUIRE(generated->warnings.size() == 2);
    auto table = parse_chs(generated->text, "fallback.chs");
    REQUIRE(table);
    CHECK(table->apply("\x80\x81") == "EUR?");
    CHECK_FALSE(generate_chs(map, {}, {"\nSOURCE"}, {"UTF-8"}));
}

TEST_CASE("makechs respects explicit ASCII mappings in destination maps") {
    const auto source = identity_map();
    auto destination = source;
    destination.at('A') = U'B';
    destination.at('B') = U'A';
    const auto generated = generate_chs(source, destination, {"SOURCE"}, {"DEST"});
    REQUIRE(generated);
    const auto table = parse_chs(generated->text, "remapped.chs");
    REQUIRE(table);
    CHECK(table->apply("AB") == "BA");
}
