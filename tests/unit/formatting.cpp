#include "core/formatting.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>

using namespace jamnntpd;
TEST_CASE("Soft CR removal respects source code page and UTF-8 continuation bytes") {
    CHECK(remove_soft_cr("\x8d\xc4\xb3", {"CP866"}, false) == "\x8d\xc4\xb3");
    CHECK(remove_soft_cr("\x8d", {"IBMPC,866"}, false) == "\x8d");
    CHECK(remove_soft_cr("\xc4\x8d", {"UTF-8"}, false) == "č");
    CHECK(remove_soft_cr("a\x8d"
                         "b",
                         {"ISO-8859-1"}, false) == "ab");
    CHECK(remove_soft_cr("a\x8d"
                         "b",
                         {"ISO-8859-1"}, true) == "a\x8d"
                                                  "b");
}
TEST_CASE("Post formatting replaces every Unicode NBSP including index zero") {
    const auto converted = format_post_text(" Н x \r\n", {.replace_nbsp = true, .quote_name = ""});
    REQUIRE(converted);
    CHECK(*converted == " Н x\r");
    CHECK(*format_post_text(" Н \n", {.quote_name = ""}) == " Н \r");
    CHECK(*format_post_text("\r\n\n\r", {.quote_name = ""}) == "\r\r\r");
    CHECK(format_post_text("", {.quote_name = ""})->empty());
    CHECK_FALSE(format_post_text("\xff", {.quote_name = ""}));
    CHECK_FALSE(format_post_text(std::string{"a\0b", 3}, {.quote_name = ""}));
    CHECK_FALSE(format_post_text("text", {.quote_name = "\xff"}));
}
TEST_CASE("Flowed post joins ordinary lines and preserves quote and signature semantics") {
    PostTextOptions options{.flowed = true, .quote_name = ""};
    const auto converted = format_post_text("Hello \r\nworld\r\n>  quoted\r\n> fixed \r\n"
                                            " From space \r\nnext\r\n-- \r\nsignature\r\n",
                                            options);
    REQUIRE(converted);
    CHECK(*converted == "Hello world\r> quoted\r> fixed\rFrom space next\r-- \rsignature\r");
    options.delete_stuffed_quote_space = false;
    CHECK(*format_post_text(">  quoted\n", options) == ">  quoted\r");
    CHECK(*format_post_text(" \n\n", options) == " \r");
}
TEST_CASE("Smart quotes retain Unicode initials and nested quote structure") {
    const PostTextOptions options{.quote_name = "Иван Петров"};
    CHECK(*format_post_text("> hello\r>> nested\rplain\r", options) ==
          " ИП> hello\r >> nested\rplain\r");
    CHECK(*format_post_text(">\r", options) == "\r");
    const std::string long_quote = "> " + std::string(200, 'x');
    CHECK(*format_post_text(long_quote, options) == long_quote + "\r");
    CHECK(*format_post_text("> AB> existing\r", options) == " AB>> existing\r");
}
TEST_CASE("Reading wraps complete code points and dot-stuffs framed output") {
    const auto converted = format_read_text(".line\rFrom here\r leading\r-- \r\r", {"UTF-8"}, true);
    REQUIRE(converted);
    CHECK(converted->bytes == "..line\r\n From here\r\n  leading\r\n-- \r\n\r\n");
    CHECK(format_read_text("x  \r", {"UTF-8"}, true)->bytes == "x\r\n");
    CHECK(format_read_text("x  \r", {"UTF-8"}, false)->bytes == "x  \r\n");
    const auto wrapped = format_read_text(std::string(70, 'a') + " слово next\r", {"UTF-8"}, true);
    REQUIRE(wrapped);
    CHECK(wrapped->bytes == std::string(70, 'a') + " \r\nслово next\r\n");
    CHECK(format_read_text(std::string(79, 'a'), {"UTF-8"}, false)->bytes ==
          std::string(79, 'a') + "\r\n");
    CHECK(format_read_text("", {"UTF-8"}, false)->bytes.empty());
    CHECK_FALSE(format_read_text("\xff", {"UTF-8"}, true));
    CHECK_FALSE(format_read_text("x", {"invalid-charset"}, false));
}
TEST_CASE("Long-word read limits are checked in final encoded bytes without splitting UTF-8") {
    std::string text{"."};
    for (int index = 0; index < 1200; ++index) {
        text += "😀";
    }
    const auto wire = format_read_text(text, {"UTF-8"}, false);
    REQUIRE(wire);
    std::string restored;
    std::string_view remaining = wire->bytes;
    while (!remaining.empty()) {
        const auto end = remaining.find("\r\n");
        REQUIRE(end != std::string_view::npos);
        auto line = remaining.substr(0, end);
        CHECK(line.size() <= 998);
        if (line.starts_with("..")) {
            line.remove_prefix(1);
        }
        CHECK(transcode(line, {"UTF-8"}, {"UTF-8"}));
        restored += line;
        remaining.remove_prefix(end + 2);
    }
    CHECK(restored == text);
    CHECK_FALSE(format_read_text(text, {"CP866"}, false));
    const auto replacement = format_read_text(text, {"CP866"}, false, ConversionPolicy::replace);
    REQUIRE(replacement);
    CHECK(replacement->replaced);
}
TEST_CASE("Tearline options and final encoded post limits never silently truncate") {
    TearlineOptions options{.newsreader = "Reader", .origin = "Дом", .address = "2:1/2"};
    const auto text = append_tearline("body\r", options);
    REQUIRE(text);
    CHECK(*text == "body\r\r--- Reader\r * Origin: Дом (2:1/2)\r");
    options.no_tearline = true;
    options.add_cr = false;
    CHECK(*append_tearline("body\r", options) == "body\r---\r * Origin: Дом (2:1/2)\r");
    options.origin = "line\ninjected";
    CHECK_FALSE(append_tearline("body", options));
    CHECK(encode_post_text("Н", {"CP866"}, 1)->bytes == "\x8d");
    CHECK_FALSE(encode_post_text("Н", {"UTF-8"}, 1));
    CHECK_FALSE(encode_post_text("😀", {"CP866"}, 8));
    CHECK(encode_post_text("😀", {"CP866"}, 1, ConversionPolicy::replace)->replaced);
    CHECK_FALSE(encode_post_text(std::string{"a\0", 2}, {"UTF-8"}, 2));
    CHECK_FALSE(format_read_text(std::string(1024 * 1024 + 1, 'x'), {"UTF-8"}, true));
}
