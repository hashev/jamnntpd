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
TEST_CASE("Post builder preserves routing and applies options with encoded field limits") {
    Group group{
        .name = "test", .access = 'a', .address = "2:1/2", .base = {}, .default_charset = "!CP866"};
    const auto rules = parse_charset_config("post UTF-8 CP866 -builtin\n", "rules");
    REQUIRE(rules);
    const auto plan =
        resolve_post(*rules, {.source = "UTF-8", .group_default = group.default_charset});
    REQUIRE(plan);
    const Identity identity{
        .username = "alice", .permissions = {.read = "a", .post = "a"}, .realnames = "Alice"};
    RawMimeArticle raw;
    raw.headers = {{"from", "Sender <a@example.org>"},
                   {"subject", "Re: Topic"},
                   {"organization", "Origin"},
                   {"user-agent", "Reader"}};
    raw.source_charset = "UTF-8";
    raw.body = "To: Bob, 2:1/3\r\n\r\nBody\r\n";
    const auto build = [&](const Config &config, const UserOptions &options,
                           const ReplyTarget &parent) -> Result<BuiltPost> {
        return build_post(raw, group, identity, options, *plan, *rules, config, parent);
    };
    Config config;
    config.note = true;
    auto built = build(config, {}, {});
    REQUIRE(built);
    CHECK(jam_field(built->message, field_recipient) == "Bob");
    CHECK(jam_field(built->message, field_subject) == "Topic");
    CHECK(built->message.text.find("Origin") != std::string::npos);
    config.no_tearline = true;
    config.no_strip_re = true;
    config.no_reply_address = true;
    config.no_tzutc = true;
    built = build(config, {}, {});
    REQUIRE(built);
    CHECK(jam_field(built->message, field_subject) == "Re: Topic");
    CHECK(built->message.text.find("--- Reader") == std::string::npos);
    CHECK(built->message.text.find("---\r") != std::string::npos);
    group.netmail = true;
    built = build({}, {}, {});
    REQUIRE(built);
    CHECK(jam_field(built->message, field_destination) == "2:1/3");
    CHECK(built->message.header.attributes ==
          (attribute_local | attribute_private | attribute_type_net));
    raw.body = "Body";
    CHECK_FALSE(build({}, {}, {}));
    raw.body = "To: Bob, invalid\r\nBody";
    CHECK_FALSE(build({}, {}, {}));
    raw.body = "Body";
    raw.headers["references"] = "<1$test@JamNNTPd>";
    const ReplyTarget parent{
        .name = "Bob", .address = "2:1/3", .message_id = "old id", .number = 1};
    UserOptions options;
    options.squote = true;
    raw.body = "> quoted\r\n";
    built = build({}, options, parent);
    REQUIRE(built);
    CHECK(built->message.header.reply_to == 1);
    CHECK(jam_field(built->message, field_reply_id) == "old id");
    group.netmail = false;
    group.local = true;
    group.no_chrs = true;
    raw.headers.erase("references");
    built = build({}, {}, {});
    REQUIRE(built);
    CHECK(built->message.header.attributes == (attribute_local | attribute_type_local));
    CHECK(jam_field(built->message, field_origin).empty());
    raw.headers["subject"] = std::string(72, 'S');
    CHECK_FALSE(build({}, {}, {}));
    raw.headers["subject"] = "A\tB";
    CHECK(build({}, {}, {}));
    raw.headers["subject"] = "Bad\x01";
    CHECK_FALSE(build({}, {}, {}));
    raw.headers["subject"] = "Topic";
    raw.headers["from"] = "a@example.org";
    CHECK_FALSE(build({}, {}, {}));
    raw.headers["from"] = "Sender <a@example.org>";
    raw.headers["reply-to"] = std::string(101, 'x');
    CHECK_FALSE(build({}, {}, {}));
    raw.headers.erase("reply-to");
    raw.body = "\xff";
    CHECK_FALSE(build({}, {}, {}));
    config = Config{};
    config.conversion_policy = ConversionPolicy::replace;
    built = build(config, {}, {});
    REQUIRE(built);
    CHECK(built->replaced);
}
TEST_CASE("Opaque custom charset articles retain bytes and reject unsafe MIME headers") {
    Group group{.name = "test",
                .access = 'a',
                .address = "2:1/2",
                .base = {},
                .default_charset = "!CUSTOM"};
    const auto rules = parse_charset_config("read CUSTOM PRIVATE\n", "rules");
    REQUIRE(rules);
    auto message = sample_message();
    for (auto &field : message.fields) {
        if (field.low == field_sender) {
            field.data = jam::bytes_from("A\xe9");
        }
        if (field.low == field_kludge) {
            field.data = jam::bytes_from("CHRS: CUSTOM 2");
        }
    }
    message.text = "legacy\xe9\r";
    auto text = read_message_text(message, group, *rules, ConversionPolicy::strict);
    REQUIRE(text);
    CHECK(text->opaque);
    CHECK(text->sender == "A\xe9");
    auto rendered = render_message(message, *text, group, {}, {});
    REQUIRE(rendered);
    CHECK(rendered->headers.find("=?PRIVATE?B?") != std::string::npos);
    CHECK(rendered->body.find("legacy\xe9") != std::string::npos);
    Config no_encode;
    no_encode.no_encode = true;
    rendered = render_message(message, *text, group, {}, no_encode);
    REQUIRE(rendered);
    CHECK(rendered->headers.find("A\xe9") != std::string::npos);
    text->charset.chrs = std::string(80, 'x');
    CHECK_FALSE(render_message(message, *text, group, {}, {}));
    text->charset.chrs = "PRIVATE";
    text->subject = "Bad\r\nInjected";
    CHECK_FALSE(render_message(message, *text, group, {}, {}));
    text->subject = "Safe";
    text->charset.chrs = "bad\nlabel";
    CHECK_FALSE(render_message(message, *text, group, {}, {}));
    text->charset.chrs = "PRIVATE";
    text->sender = "";
    text->recipient = "";
    text->origin = "";
    text->reply_address = "";
    message.header.attributes = 0;
    rendered = render_message(message, *text, group, {}, {});
    REQUIRE(rendered);
    CHECK(rendered->headers.find("X-JAM-Attributes:") == std::string::npos);
    text->charset.chrs = "UTF-8";
    text->opaque = false;
    text->body = "\xff";
    CHECK_FALSE(render_message(message, *text, group, {}, {}));
    text->body = "Body";
    text->timezone = "invalid";
    CHECK_FALSE(render_message(message, *text, group, {}, {}));
    text->timezone = "+0000";
    text->sender = "Alice";
    text->recipient = "All";
    message.text = " * Origin: Place (2:3/4)\r";
    Config read_origin;
    read_origin.read_origin = true;
    UserOptions options;
    options.showto = false;
    options.flowed = false;
    rendered = render_message(message, *text, group, options, read_origin);
    REQUIRE(rendered);
    CHECK(rendered->headers.find("<0@4.3.2>") != std::string::npos);
    add_jam_field(message, field_pid, "bad\nheader");
    CHECK_FALSE(render_message(message, *text, group, {}, {}));
}
TEST_CASE("Post conversion distinguishes guest names custom bytes and malformed headers") {
    const Group group{
        .name = "test", .access = 'a', .address = "2:1/2", .base = {}, .default_charset = "!CP866"};
    const auto rules = parse_charset_config("post UTF-8 CP866 -builtin\n", "rules");
    REQUIRE(rules);
    const auto plan =
        resolve_post(*rules, {.source = "UTF-8", .group_default = group.default_charset});
    REQUIRE(plan);
    RawMimeArticle raw;
    raw.headers = {{"from", "Sender <a@example.org>"}, {"subject", "Re:"}};
    raw.source_charset = "UTF-8";
    raw.body = "Body";
    Identity guest{.username = "", .permissions = {.read = "a", .post = "a"}, .realnames = "*"};
    Config config;
    config.guest_suffix = " (guest)";
    config.origin = "Configured origin";
    auto built = build_post(raw, group, guest, {}, *plan, *rules, config, {});
    REQUIRE(built);
    CHECK(jam_field(built->message, field_sender) == "Sender (guest)");
    CHECK(jam_field(built->message, field_subject).empty());
    CHECK(built->message.text.find("Configured origin") != std::string::npos);
    config.guest_suffix = "\xff";
    CHECK_FALSE(build_post(raw, group, guest, {}, *plan, *rules, config, {}));
    config.guest_suffix = "";
    config.origin = "\xff";
    CHECK_FALSE(build_post(raw, group, guest, {}, *plan, *rules, config, {}));
    config.origin = "";
    guest.realnames = "\xff";
    CHECK_FALSE(build_post(raw, group, guest, {}, *plan, *rules, config, {}));
    guest.realnames = "*";
    for (const auto *key : {"from", "subject", "organization", "x-newsreader"}) {
        auto malformed = raw;
        malformed.headers[key] = std::string{"=?unknown?B?/w==?="} +
                                 (std::string_view{key} == "from" ? " <a@example.org>" : "");
        CHECK_FALSE(build_post(malformed, group, guest, {}, *plan, *rules, config, {}));
    }
    const auto opaque_rules = parse_charset_config("post PRIVATE CUSTOM\n", "rules");
    REQUIRE(opaque_rules);
    const auto opaque_plan =
        resolve_post(*opaque_rules, {.source = "PRIVATE", .group_default = "CUSTOM"});
    REQUIRE(opaque_plan);
    raw.source_charset = "PRIVATE";
    raw.body = "byte\xe9";
    raw.headers["subject"] = "Raw\xe9";
    auto custom_group = group;
    custom_group.default_charset = "!CUSTOM";
    built = build_post(raw, custom_group, guest, {}, *opaque_plan, *opaque_rules, config, {});
    INFO((built ? "built" : built.error().message));
    REQUIRE(built);
    CHECK(jam_field(built->message, field_subject) == "Raw\xe9");
    CHECK(built->message.text.find("byte\xe9") != std::string::npos);
}
TEST_CASE("IBMPC CODEPAGE is represented unambiguously in outgoing MIME") {
    const Group group{.name = "test",
                      .access = 'a',
                      .address = "2:1/2",
                      .base = {},
                      .default_charset = "!IBMPC,866"};
    const auto rules = parse_charset_config("read IBMPC,866 IBMPC,866\n", "rules");
    REQUIRE(rules);
    auto message = sample_message();
    for (auto &field : message.fields) {
        if (field.low == field_kludge) {
            field.data = jam::bytes_from("CHRS: IBMPC 2");
        }
        if (field.low == field_subject) {
            field.data = jam::bytes_from("\x8d");
        }
    }
    add_jam_field(message, field_kludge, "CODEPAGE: 866");
    message.text = "\x8d\r";
    const auto text = read_message_text(message, group, *rules, ConversionPolicy::strict);
    REQUIRE(text);
    CHECK(text->subject == "Н");
    const auto rendered = render_message(message, *text, group, {}, {});
    REQUIRE(rendered);
    CHECK(rendered->headers.find("charset=CP866") != std::string::npos);
    CHECK(rendered->headers.find("=?CP866?B?") != std::string::npos);
    CHECK(rendered->body.find('\x8d') != std::string::npos);
}
