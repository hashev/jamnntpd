#include "core/ftn.hpp"
#include "core/post_builder.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace jamnntpd;
namespace {
auto sample_message() -> jam::Message {
    jam::Message message;
    message.header.number = 1;
    message.header.written = 1700000000;
    message.header.attributes = attribute_local | attribute_type_echo;
    add_jam_field(message, field_sender, "Alice");
    add_jam_field(message, field_recipient, "All");
    add_jam_field(message, field_subject, "Topic");
    add_jam_field(message, field_origin, "2:1/2");
    add_jam_field(message, field_message_id, "2:1/2 12345678");
    add_jam_field(message, field_kludge, "CHRS: UTF-8 4");
    message.text = "Body\r";
    return message;
}
} // namespace
TEST_CASE("FTN metadata is rendered safely and preserves mailbox and attribute meaning") {
    Group group{
        .name = "test", .access = 'a', .address = "2:1/2", .base = {}, .default_charset = "UTF-8"};
    const auto rules = parse_charset_config("read UTF-8 UTF-8 -builtin\n", "rules");
    REQUIRE(rules);
    auto message = sample_message();
    add_jam_field(message, field_kludge, "REPLYADDR Alice <a@example.org>");
    add_jam_field(message, field_kludge, "TZUTCINFO: +0300");
    auto text = read_message_text(message, group, *rules, ConversionPolicy::strict);
    REQUIRE(text);
    CHECK(text->timezone == "+0300");
    auto rendered = render_message(message, *text, group, {}, {});
    REQUIRE(rendered);
    CHECK(rendered->headers.find("<a@example.org>") != std::string::npos);
    CHECK(rendered->headers.find("<<") == std::string::npos);
    CHECK(rendered->headers.find("X-JAM-Attributes: Local TypeEcho") != std::string::npos);
    for (const auto number : {field_sender, field_recipient, field_subject, field_origin,
                              field_destination, field_message_id, field_reply_id}) {
        auto corrupt = sample_message();
        add_jam_field(corrupt, number, "bad\nInjected: true");
        for (auto &field : corrupt.fields) {
            if (field.low == number) {
                field.data = jam::bytes_from("bad\nInjected: true");
            }
        }
        CHECK_FALSE(read_message_text(corrupt, group, *rules, ConversionPolicy::strict));
    }
    message = sample_message();
    for (auto &field : message.fields) {
        if (field.low == field_subject) {
            field.data = jam::bytes_from("A\tB");
        }
    }
    CHECK(read_message_text(message, group, *rules, ConversionPolicy::strict));
    message.text = "\xff";
    CHECK_FALSE(read_message_text(message, group, *rules, ConversionPolicy::strict));
    const auto replacement = read_message_text(message, group, *rules, ConversionPolicy::replace);
    REQUIRE(replacement);
    CHECK(replacement->replaced);
}
TEST_CASE("FTN charset metadata and privacy handle opaque and standard names") {
    jam::Message message;
    add_jam_field(message, field_kludge, "CHARSET: IBMPC");
    add_jam_field(message, field_kludge, "CODEPAGE: 866");
    CHECK(encoding_name(message_charset(message)) == "CP866");
    CHECK(encoding_name({.chrs = "IBMPC", .codepage = ""}) == "CP437");
    MessageText text;
    text.opaque = true;
    text.sender = "Alice";
    text.recipient = "Bob";
    Group group{.name = "private",
                .access = 'b',
                .address = {},
                .base = {},
                .default_charset = {},
                .netmail = true};
    Identity identity{
        .username = "alice", .permissions = {.read = "b", .post = "b"}, .realnames = "Alice"};
    CHECK(visible_message(group, identity, text));
    text.sender = "Other";
    CHECK_FALSE(visible_message(group, identity, text));
    identity.realnames = "Bob";
    CHECK(visible_message(group, identity, text));
    identity.permissions.read = "a";
    CHECK_FALSE(visible_message(group, identity, text));
    text.opaque = false;
    text.charset = {.chrs = "CP866", .codepage = ""};
    CHECK_FALSE(identity_matches(identity, "😀", text));
}
