#include "core/text.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>

using namespace jamnntpd;

TEST_CASE("ASCII matching is length safe and preserves legacy wildcard semantics") {
    CHECK(equal_ascii("ALICE", "alice"));
    CHECK_FALSE(equal_ascii("a", "ab"));
    CHECK_FALSE(match_pattern("long", ""));
    CHECK_FALSE(match_pattern("???", "ab"));
    CHECK(match_pattern("127.*.suffix", "127.0.0.1"));
    CHECK(match_pattern("a?", "Ab"));
    CHECK_FALSE(match_pattern("a", "ab"));
    CHECK(match_names({"Alice,Bob"}, "bOB"));
    CHECK_FALSE(match_names({"Alice,Bob"}, "Eve"));
    CHECK(match_group("Ax", 'a'));
    CHECK(match_group("*", 'Z'));
    CHECK_FALSE(match_group("-", 'a'));
    CHECK_FALSE(match_group("A", 'b'));
    CHECK(lower_ascii("Az09\xff") == "az09\xff");
}

TEST_CASE("Whitespace and configuration tokenization do not truncate or overread") {
    for (auto input : std::array{"", " ", "\r\n", "\t \r\n"}) {
        CHECK(trim(input).empty());
        const auto words = config_words(input);
        REQUIRE(words);
        CHECK(words->empty());
    }
    CHECK(trim("  hi\r\n") == "hi");
    auto words = config_words("groups \"path with spaces\" \"\" C:\\fido");
    REQUIRE(words);
    REQUIRE(words->size() == 4);
    CHECK((*words)[1] == "path with spaces");
    CHECK((*words)[2].empty());
    CHECK((*words)[3] == "C:\\fido");
    CHECK(config_words(" # comment")->empty());
    CHECK_FALSE(config_words("\"unterminated"));
    CHECK_FALSE(config_words("\"quoted\"suffix"));
    const std::string long_name(10000, 'a');
    REQUIRE(config_words(long_name));
    CHECK(config_words(long_name)->front() == long_name);
}

TEST_CASE("Numbers reject signs, overflow, junk and invalid bases") {
    CHECK(parse_number("0", 0) == 0);
    CHECK(parse_number("65535", 65535) == 65535);
    CHECK(parse_number("ffffffff", std::numeric_limits<std::uint32_t>::max(),
                       NumberBase::hexadecimal) == 0xffffffffU);
    for (auto input : std::array{"", "-1", "+1", "4294967296", "1x", " 1", "2"}) {
        CHECK_FALSE(parse_number(input, 1));
    }
    CHECK_FALSE(parse_number("10", 100, static_cast<NumberBase>(2)));
}
