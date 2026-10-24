#include "core/charset_rules.hpp"
#include "temp_directory.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>

using namespace jamnntpd;
namespace {
auto fixture_config(std::string_view rules) -> Result<CharsetConfig> {
    const auto directory = std::filesystem::path{JAM_FIXTURE_DIR} / "chs";
    return parse_charset_config("chsdir \"" + directory.string() + "\"\n" + std::string{rules},
                                "test.xlat");
}
} // namespace
TEST_CASE("Explicit label-only charset rules preserve bytes even for known pairs") {
    const auto config = fixture_config("read CP866 UTF-8\npost UTF-8 CP866\n");
    REQUIRE(config);
    auto reading = resolve_read(*config, {.metadata = {.chrs = "CP866", .codepage = ""},
                                          .group_default = "",
                                          .destination_pattern = std::nullopt});
    REQUIRE(reading);
    CHECK(reading->method == ConversionMethod::pass_through);
    CHECK(reading->apply("\x8d\xc4")->bytes == "\x8d\xc4");
    auto posting = resolve_post(*config, {.source = "UTF-8", .group_default = "!CP866"});
    REQUIRE(posting);
    CHECK(posting->apply("Н─")->bytes == "Н─");
    CHECK(posting->destination.chrs == "CP866");
    CHECK(reading->apply(std::string{"a\0b", 3})->bytes == std::string{"a\0b", 3});
    const auto disabled = fixture_config("read * unknown-8bit\npost * \"\"\n");
    REQUIRE(disabled);
    auto opaque = resolve_post(*disabled, {.source = "unknown-8bit", .group_default = ""});
    REQUIRE(opaque);
    CHECK(opaque->destination.chrs.empty());
    CHECK(opaque->apply("\xff")->bytes == "\xff");
}
TEST_CASE("Builtin rules require explicit opt-in and preserve strict and replacement policy") {
    const auto config = fixture_config("READ CP866 UTF-8 -BuIlTiN -keepsoftcr\n"
                                       "post UTF-8 IBMPC,866 -builtin\n");
    REQUIRE(config);
    auto read = resolve_read(*config, {.metadata = {.chrs = "CP866", .codepage = ""},
                                       .group_default = "",
                                       .destination_pattern = std::nullopt});
    REQUIRE(read);
    CHECK(read->keep_soft_cr);
    CHECK(read->method == ConversionMethod::builtin);
    CHECK(read->apply("\x8d\xc4\xb3")->bytes == "Н─│");
    auto post = resolve_post(*config, {.source = "UTF-8", .group_default = "!IBMPC,866"});
    REQUIRE(post);
    CHECK(post->apply("Н─│")->bytes == "\x8d\xc4\xb3");
    CHECK_FALSE(post->apply("😀"));
    const auto replaced = post->apply("😀", ConversionPolicy::replace);
    REQUIRE(replaced);
    CHECK(replaced->bytes == "?");
    CHECK(replaced->replaced);
}
TEST_CASE("Read charset force default and alias precedence is first match only") {
    auto config = fixture_config("defaultread !CP850\nreadalias IBMPC,866 CP866\n"
                                 "readalias CP866 CP437\nread * UTF-8 -builtin\n");
    REQUIRE(config);
    auto request = ReadCharsetRequest{.metadata = {.chrs = "CP437", .codepage = ""},
                                      .group_default = "!CP866",
                                      .destination_pattern = std::nullopt};
    CHECK(resolve_read(*config, request)->source.chrs == "CP437"); // group forced then alias
    request.group_default = "CP865";
    CHECK(resolve_read(*config, request)->source.chrs == "CP850");
    config->default_read = "CP850";
    CHECK(resolve_read(*config, request)->source.chrs == "CP437");
    request.metadata = {};
    CHECK(resolve_read(*config, request)->source.chrs == "CP865");
    request.group_default = "";
    CHECK(resolve_read(*config, request)->source.chrs == "CP850");
    request.metadata = {.chrs = "IBMPC", .codepage = "866"};
    const auto aliased = resolve_read(*config, request);
    REQUIRE(aliased);
    CHECK(aliased->source.chrs == "CP866");
    CHECK(aliased->source.codepage.empty());
}
TEST_CASE("Rule source destination filtering and post defaults retain legacy order") {
    const auto config = fixture_config("defaultpost old\npostalias old UTF-8\n"
                                       "postalias UTF-8 CP437\ndefaultread !CP850\n"
                                       "read CP866 CP437\nread CP866 CP850\n"
                                       "post UTF-8 CP437\npost UTF-8 CP850\npost UTF-8 CP866\n");
    REQUIRE(config);
    auto request = ReadCharsetRequest{.metadata = {.chrs = "CP866", .codepage = ""},
                                      .group_default = "!CP866",
                                      .destination_pattern = "CP850"};
    CHECK(resolve_read(*config, request)->destination.chrs == "CP850");
    request.destination_pattern = std::nullopt;
    CHECK(resolve_read(*config, request)->destination.chrs == "CP437");
    CHECK(resolve_post(*config, {.source = "", .group_default = "!CP866"})->destination.chrs ==
          "CP866");
    CHECK(resolve_post(*config, {.source = "", .group_default = "CP866"})->destination.chrs ==
          "CP850");
    auto unforced = *config;
    unforced.default_read = "CP850";
    CHECK(resolve_post(unforced, {.source = "", .group_default = "CP866"})->destination.chrs ==
          "CP437");
}
TEST_CASE("Custom tables take priority and cannot be silently inverted") {
    auto config = fixture_config("read CP866 UTF-8 custom-forward.chs -keepsoftcr\n");
    REQUIRE(config);
    auto read = resolve_read(*config, {.metadata = {.chrs = "CP866", .codepage = ""},
                                       .group_default = "",
                                       .destination_pattern = std::nullopt});
    REQUIRE(read);
    CHECK(read->method == ConversionMethod::table);
    CHECK(read->apply("AB\x80")->bytes == "ab─");
    CHECK_FALSE(resolve_post(*config, {.source = "UTF-8", .group_default = "CP866"}));
    auto reverse = fixture_config("read CP866 UTF-8 custom-forward.chs\n"
                                  "post UTF-8 CP866 custom-reverse.chs\n");
    REQUIRE(reverse);
    const auto post = resolve_post(*reverse, {.source = "UTF-8", .group_default = "CP866"});
    REQUIRE(post);
    CHECK(post->apply("\x80")->bytes == "\x81");
    // Plans own mappings, independent of configuration lifetime and mutation.
    config->read_rules.clear();
    CHECK(read->apply("AB")->bytes == "ab");
}
TEST_CASE("Automatic conversion is restricted to known standard pairs without matching rules") {
    CharsetConfig config;
    auto read = resolve_read(config, {.metadata = {.chrs = "IBMPC", .codepage = "866"},
                                      .group_default = "",
                                      .destination_pattern = std::nullopt});
    REQUIRE(read);
    CHECK(read->apply("\x8d")->bytes == "Н");
    CHECK(resolve_post(config, {.source = "UTF8", .group_default = "CP866"})->apply("Н")->bytes ==
          "\x8d");
    CHECK(resolve_post(config, {.source = "UTF-8", .group_default = ""})->apply("Н")->bytes == "Н");
    config.default_read = "LATIN-1";
    CHECK(resolve_read(config,
                       {.metadata = {}, .group_default = "", .destination_pattern = std::nullopt})
              ->apply("\xa3")
              ->bytes == "£");
    CHECK_FALSE(resolve_post(config, {.source = "unknown-8bit", .group_default = "CP866"}));
    CHECK_FALSE(resolve_post(config, {.source = "UTF-8", .group_default = "unknown"}));
    CHECK_FALSE(
        resolve_read(config, {.metadata = {}, .group_default = "", .destination_pattern = "*"}));
}
TEST_CASE("Charset configuration errors include filename and line without partial configuration") {
    for (const auto text :
         std::array{"read", "read CP866 UTF-8 -unknown", "read CP866 UTF-8 -builtin -builtin",
                    "read CP866 UTF-8 custom-forward.chs -builtin", "read CP866 UTF-8 missing.chs",
                    "read CP866 UTF-8 truncated.chs",
                    "read CP866 UTF-8 custom-forward.chs custom-reverse.chs",
                    "read CP866 unknown -builtin", "readalias only", "chsdir", "unknown value",
                    "defaultpost a b", "read \"\" UTF-8", "read CP866 \"UTF-8"}) {
        DYNAMIC_SECTION(text) {
            const auto config = fixture_config(text);
            REQUIRE_FALSE(config);
            CHECK(config.error().message.find("test.xlat:2:") != std::string::npos);
        }
    }
    CHECK_FALSE(parse_charset_config(std::string(65537, 'x'), "long.xlat"));
    CHECK_FALSE(parse_charset_config(std::string(16 * 1024 * 1024 + 1, 'x'), "large.xlat"));
    TempDirectory directory;
    const auto path =
        directory.write("valid.xlat", "# comment\ndefaultread CP866\nread CP866 UTF-8 -builtin\n");
    const auto loaded = load_charset_config(path);
    REQUIRE(loaded);
    CHECK(loaded->default_read == "CP866");
    CHECK_FALSE(load_charset_config(directory.file("absent.xlat")));
    CHECK_FALSE(load_charset_config(directory.file("")));
    ConversionPlan missing{.source = {},
                           .destination = {},
                           .method = ConversionMethod::table,
                           .table = std::nullopt,
                           .keep_soft_cr = false};
    CHECK_FALSE(missing.apply("text"));
}
