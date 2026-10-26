#include "core/codec.hpp"
#include "core/text.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace jamnntpd;
TEST_CASE("Standard encodings preserve representable bytes including box drawing") {
    for (auto encoding : std::array{"CP866", "CP437", "CP850", "CP865", "KOI8-R", "windows-1251",
                                    "windows-1252", "ISO-8859-1", "ISO-8859-15"}) {
        DYNAMIC_SECTION(encoding) {
            for (unsigned int value = 0; value < 256; ++value) {
                const std::string byte(1, static_cast<char>(value));
                const auto unicode = transcode(byte, {encoding}, {"UTF-8"});
                if (unicode) {
                    const auto restored = transcode(unicode->bytes, {"UTF-8"}, {encoding});
                    REQUIRE(restored);
                    CHECK(restored->bytes == byte);
                    CHECK_FALSE(restored->replaced);
                } else {
                    CHECK(unicode.error().kind == ErrorKind::encoding);
                }
            }
        }
    }
    CHECK(transcode("\x8d\xc4\xb3", {"CP866"}, {"UTF-8"})->bytes == "Н─│");
    CHECK(transcode("\xa0", {"LATIN-1"}, {"UTF-8"})->bytes == "\xc2\xa0");
}
TEST_CASE("Strict conversion rejects malformed and unrepresentable text") {
    for (const auto &bytes : {std::string{"\xc0\xaf"}, std::string{"\xed\xa0\x80"},
                              std::string{"\xe2\x82"}, std::string{"\xff"}}) {
        CHECK_FALSE(transcode(bytes, {"UTF-8"}, {"UTF-8"}));
        const auto replacement = transcode(bytes, {"UTF-8"}, {"UTF-8"}, ConversionPolicy::replace);
        REQUIRE(replacement);
        CHECK(replacement->replaced);
        CHECK_FALSE(replacement->bytes.empty());
    }
    CHECK_FALSE(transcode("😀", {"UTF-8"}, {"CP866"}));
    const auto replacement = transcode("😀", {"UTF-8"}, {"CP866"}, ConversionPolicy::replace);
    REQUIRE(replacement);
    CHECK(replacement->bytes == "?");
    CHECK(replacement->replaced);
    CHECK_FALSE(transcode("a", {"invalid-encoding"}, {"UTF-8"}));
    CHECK_FALSE(transcode("a", {"UTF-8"}, {"invalid-encoding"}));
    CHECK_FALSE(transcode("a", {""}, {"UTF-8"}));
    CHECK(transcode({}, {"UTF-8"}, {"UTF-8"})->bytes.empty());
    const std::string nul{"a\0b", 3};
    CHECK(transcode(nul, {"UTF-8"}, {"UTF-8"})->bytes == nul);
}
TEST_CASE("CHS supports shipped and explicitly directional custom mappings") {
    const auto directory = std::filesystem::path{JAM_FIXTURE_DIR} / "chs";
    const auto standard = load_chs(directory / "cp866-to-utf8.chs");
    REQUIRE(standard);
    CHECK(standard->apply("\x8d\xc4\xb3") == "Н─│");
    const auto custom = load_chs(directory / "custom-forward.chs");
    REQUIRE(custom);
    CHECK(custom->apply(std::string{"\0ABC\x80\xff\\", 7}) == "abC─?\\");
    const auto reverse = load_chs(directory / "custom-reverse.chs");
    REQUIRE(reverse);
    CHECK(reverse->apply("\x80") == "\x81");
    const auto level_one = load_chs(directory / "level-one.chs");
    REQUIRE(level_one);
    CHECK(level_one->apply("A\x80") == "Z\x80");
    CHECK_FALSE(load_chs(directory / "truncated.chs"));
    CHECK_FALSE(load_chs(directory / "invalid-escape.chs"));
    CHECK_FALSE(load_chs(directory / "does-not-exist.chs"));
    CHECK_FALSE(parse_chs("", "empty.chs"));
    CHECK_FALSE(parse_chs("1\n0\n3\na\nb\n", "bad-level.chs"));
}

TEST_CASE("Standard legacy mappings match an independent Unicode reference") {
    std::ifstream input{std::filesystem::path{JAM_FIXTURE_DIR} / "encodings.tsv"};
    REQUIRE(input.good());
    std::string line;
    while (std::getline(input, line)) {
        if (line.starts_with('#')) {
            continue;
        }
        std::istringstream record{line};
        std::string encoding;
        unsigned int value{};
        std::string unicode_hex;
        REQUIRE(static_cast<bool>(record >> encoding >> std::hex >> value >> unicode_hex));
        std::string expected;
        for (std::size_t index = 0; index < unicode_hex.size(); index += 2) {
            const auto byte = jamnntpd::parse_number(std::string_view{unicode_hex}.substr(index, 2),
                                                     255, jamnntpd::NumberBase::hexadecimal);
            REQUIRE(byte);
            expected.push_back(static_cast<char>(*byte));
        }
        const auto converted =
            transcode(std::string(1, static_cast<char>(value)), {encoding}, {"UTF-8"});
        INFO(encoding << " byte " << value);
        REQUIRE(converted);
        CHECK(converted->bytes == expected);
        const auto restored = transcode(expected, {"UTF-8"}, {encoding});
        REQUIRE(restored);
        CHECK(restored->bytes == std::string(1, static_cast<char>(value)));
        const auto embedded = transcode("A" + expected + "Z", {"UTF-8"}, {encoding});
        REQUIRE(embedded);
        CHECK(embedded->bytes == "A" + std::string(1, static_cast<char>(value)) + "Z");
    }
}

TEST_CASE("DOS micro sign and Greek mu remain distinct in both conversion directions") {
    for (auto encoding : std::array{"CP437", "CP865"}) {
        CHECK(transcode("\xe6", {encoding}, {"UTF-8"})->bytes == "µ");
        CHECK(transcode("µ", {"UTF-8"}, {encoding})->bytes == "\xe6");
        CHECK_FALSE(transcode("μ", {"UTF-8"}, {encoding}));
        const auto replaced = transcode("μ", {"UTF-8"}, {encoding}, ConversionPolicy::replace);
        REQUIRE(replaced);
        CHECK(replaced->bytes == "?");
        CHECK(replaced->replaced);
    }
}

TEST_CASE("Undefined Windows bytes fail strictly and require explicit replacement") {
    for (const auto &[encoding, value] :
         std::array{std::pair{"windows-1251", '\x98'}, std::pair{"windows-1252", '\x81'},
                    std::pair{"windows-1252", '\x8d'}, std::pair{"windows-1252", '\x8f'},
                    std::pair{"windows-1252", '\x90'}, std::pair{"windows-1252", '\x9d'}}) {
        const std::string byte(1, value);
        CHECK_FALSE(transcode(byte, {encoding}, {"UTF-8"}));
        const auto replacement = transcode(byte, {encoding}, {"UTF-8"}, ConversionPolicy::replace);
        REQUIRE(replacement);
        CHECK(replacement->bytes == "�");
        CHECK(replacement->replaced);
        const auto control = transcode(byte, {"ISO-8859-1"}, {"UTF-8"});
        REQUIRE(control);
        CHECK_FALSE(transcode(control->bytes, {"UTF-8"}, {encoding}));
    }
}

TEST_CASE("Extended CHS tables retain all byte rows and explicit expansion deletion semantics") {
    std::string text = "100000\n0\n2\nCUSTOM\nUTF-8\n";
    for (unsigned int byte = 0; byte < 256; ++byte) {
        if (byte == 'A') {
            text += "\\x78\t\\x79\n";
        } else if (byte == 'B') {
            text += "\\0\n";
        } else if (byte == 128) {
            text += "\\xF0\t\\x9F\t\\x98\t\\x80\n";
        } else {
            text += "\\d" + std::to_string(byte) + "\n";
        }
    }
    text += "END\n";
    const auto table = parse_chs(text, "extended.chs");
    REQUIRE(table);
    CHECK(table->apply(std::string{"\0ABC\x80\xff", 6}) == "xyC😀\xff");
    CHECK(table->entries.at(255) == "\xff");
    CHECK(table->entries.at(0).empty());
    auto invalid = text;
    invalid.replace(invalid.find("\\x78\t\\x79"), std::string{"\\x78\t\\x79"}.size(),
                    "\\x01 \\x02 \\x03 \\x04 \\x05");
    const auto bad_expansion = parse_chs(invalid, "bad.chs");
    REQUIRE_FALSE(bad_expansion);
    CHECK(bad_expansion.error().message.starts_with("bad.chs:"));
    CHECK_FALSE(parse_chs(text + "unexpected\n", "trailer.chs"));
    CHECK_FALSE(parse_chs(text.substr(0, text.rfind("\\d255")), "truncated.chs"));
    invalid = text;
    invalid.replace(invalid.find("\\d255"), 5, "\\d256");
    CHECK_FALSE(parse_chs(invalid, "range.chs"));
}
