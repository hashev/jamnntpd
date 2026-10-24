#include "core/mime.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
using namespace jamnntpd;

TEST_CASE("MIME base64 matches known vectors and validates error boundaries") {
    const std::array pairs{
        std::pair{"", ""},        std::pair{"f", "Zg=="},        std::pair{"fo", "Zm8="},
        std::pair{"foo", "Zm9v"}, std::pair{"foob", "Zm9vYg=="}, std::pair{"foobar", "Zm9vYmFy"}};
    for (const auto &[plain, encoded] : pairs) {
        CHECK(encode_base64(plain) == encoded);
        CHECK(decode_base64(encoded) == plain);
    }
    std::string binary;
    for (unsigned int value = 0; value < 256; ++value) {
        binary.push_back(static_cast<char>(value));
    }
    CHECK(decode_base64(encode_base64(binary)) == binary);
    CHECK(decode_base64(" Z m9v\r\n\t") == "foo");
    for (auto invalid :
         std::array{"a", "a===", "====", "AA=A", "AA==AAAA", "AB==", "AAB=", "!!!!"}) {
        CHECK_FALSE(decode_base64(invalid));
    }
}
TEST_CASE("MIME quoted-printable distinguishes body and encoded-word rules") {
    CHECK(decode_quoted_printable("A_B=3D=20C", true) == "A B= C");
    CHECK(decode_quoted_printable("A_B=\r\nC=\nD=3d", false) == "A_BCD=");
    CHECK_FALSE(decode_quoted_printable("=", false));
    CHECK_FALSE(decode_quoted_printable("=G0", false));
    CHECK_FALSE(decode_quoted_printable("=\r\n", true));
}
TEST_CASE("Encoded headers decode adjacent independent words and reject injection") {
    const auto header =
        decode_header("Hello =?UTF-8?B?0J0=?= \r\n =?UTF-8?Q?=E2=94=80?=", {"UTF-8"});
    REQUIRE(header);
    CHECK(header->bytes == "Hello Н─");
    const auto legacy = decode_header("=?CP866?B?jQ==?=", {"UTF-8"});
    REQUIRE(legacy);
    CHECK(legacy->bytes == "Н");
    for (auto invalid : std::array{"=?", "=?UTF-8?B", "=?UTF-8?X?AA==?=", "=?UTF-8?B?AA==?=",
                                   "=?UTF-8?Q?=0AInjected?="}) {
        CHECK_FALSE(decode_header(invalid, {"UTF-8"}));
    }
}
TEST_CASE("Header folding never splits a UTF-8 code point or encoded word") {
    std::string subject;
    for (int index = 0; index < 100; ++index) {
        subject += "Н─😀";
    }
    auto encoded = encode_header_words(subject, {"UTF-8"});
    REQUIRE(encoded);
    const auto decoded = decode_header(*encoded, {"UTF-8"});
    REQUIRE(decoded);
    CHECK(decoded->bytes == subject);
    auto remaining = std::string_view{*encoded};
    while (!remaining.empty()) {
        const auto end = remaining.find("?=");
        REQUIRE(end != std::string_view::npos);
        CHECK(end + 2 <= 75);
        remaining.remove_prefix(end + 2);
        if (remaining.starts_with("\r\n ")) {
            remaining.remove_prefix(3);
        }
    }
    CHECK(encode_header_words("ASCII", {"UTF-8"}) == "ASCII");
    CHECK(encode_header_words("Н", {"UTF-8"}, true) == "Н");
    CHECK_FALSE(encode_header_words("😀", {"CP866"}));
    CHECK_FALSE(encode_header_words("a\r\nb", {"UTF-8"}));
}
TEST_CASE("Text articles decode transfer and charset before interpreting their text") {
    auto article = parse_article("From: Alice <a@b>\r\nSubject: =?CP866?B?jQ==?=\r\n"
                                 "Newsgroups: ECHO\r\nContent-Type: text/plain; charset=CP866\r\n"
                                 "Content-Transfer-Encoding: base64\r\n\r\njcSz\r\n",
                                 {"UTF-8"});
    REQUIRE(article);
    CHECK(article->headers.at("subject") == "Н");
    CHECK(article->body == "Н─│");
    CHECK(article->source_charset == "CP866");
    auto folded = parse_article("Subject: one\r\n two\r\n\r\ntext", {"UTF-8"});
    REQUIRE(folded);
    CHECK(folded->headers.at("subject") == "one two");
    for (auto invalid :
         std::array{"Subject: no separator", " Subject: invalid\r\n\r\nx",
                    "Subject: one\r\nSubject: two\r\n\r\nx", "Content-Type: text/html\r\n\r\nx",
                    "Content-Type: multipart/mixed\r\n\r\nx",
                    "Content-Transfer-Encoding: unknown\r\n\r\nx", "No colon\r\n\r\nx",
                    "Content-Type: text/plain; charset=\"\"\r\n\r\nx"}) {
        CHECK_FALSE(parse_article(invalid, {"UTF-8"}));
    }
    CHECK_FALSE(parse_article("\r\n\xff", {"UTF-8"}));
    auto replacement = parse_article("\r\n\xff", {"UTF-8"}, ConversionPolicy::replace);
    REQUIRE(replacement);
    CHECK(replacement->replaced);
    CHECK(replacement->body == "�");
}
