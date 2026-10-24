#include "core/backend.hpp"
#include "temp_directory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
using namespace jamnntpd;
TEST_CASE("JAM backend posts replies and atomically authorizes cancellation") {
    TempDirectory directory;
    const auto base = directory.file("base");
    for (const auto *suffix : {".jhr", ".jdt", ".jdx", ".jlr"}) {
        std::filesystem::copy_file(std::filesystem::path{JAM_FIXTURE_DIR} / "jam" /
                                       (std::string{"sample"} + suffix),
                                   directory.file(std::string{"base"} + suffix));
    }
    Group group{.name = "test",
                .access = 'a',
                .address = "2:1/2",
                .base = base,
                .default_charset = "!CP866"};
    const auto charsets =
        parse_charset_config("read CP866 UTF-8 -builtin\npost UTF-8 CP866 -builtin\n", "test.xlat");
    REQUIRE(charsets);
    JamBackend backend{Config{}, *charsets, {group}};
    const Identity identity{
        .username = "alice", .permissions = {.read = "a", .post = "a"}, .realnames = "Alice"};
    const std::string posted{"From: Alice <a@example.org>\r\nNewsgroups: test\r\nContent-Type: "
                             "text/plain; charset=UTF-8\r\n\r\nН─│\r\n"};
    const auto result = backend.submit(posted, identity, {}, {});
    INFO((result ? "posted" : result.error().message));
    REQUIRE(result);
    const auto reply =
        backend.submit("References: <3$test@JamNNTPd>\r\n" + posted, identity, {}, {});
    INFO((reply ? "replied" : reply.error().message));
    REQUIRE(reply);
    const auto storage = jam::Storage::open(base);
    REQUIRE(storage);
    const auto snapshot = storage->read(3);
    REQUIRE(snapshot);
    CHECK(snapshot->header.first_reply == 4);
    CHECK(snapshot->header.attributes == (attribute_local | attribute_type_echo));
    const auto text = read_message_text(*snapshot, group, *charsets, ConversionPolicy::strict);
    REQUIRE(text);
    CHECK(text->sender == "Alice");
    CHECK(identity_matches(identity, text->sender, *text));
    const auto cancelled =
        backend.submit("Control: cancel <3$test@JamNNTPd>\r\n" + posted, identity, {}, {});
    INFO((cancelled ? "cancelled" : cancelled.error().message));
    REQUIRE(cancelled);
}
TEST_CASE("JAM backend rejects invalid references and enforces permission before mutation") {
    TempDirectory directory;
    const auto base = directory.file("base");
    for (const auto *suffix : {".jhr", ".jdt", ".jdx", ".jlr"}) {
        std::filesystem::copy_file(std::filesystem::path{JAM_FIXTURE_DIR} / "jam" /
                                       (std::string{"sample"} + suffix),
                                   directory.file(std::string{"base"} + suffix));
    }
    const Group group{.name = "test",
                      .access = 'a',
                      .address = "2:1/2",
                      .base = base,
                      .default_charset = "!CP866"};
    const auto rules =
        parse_charset_config("read CP866 UTF-8 -builtin\npost UTF-8 CP866 -builtin\n", "rules");
    REQUIRE(rules);
    Config config;
    JamBackend backend{config, *rules, {group}};
    const Identity alice{
        .username = "alice", .permissions = {.read = "a", .post = "a"}, .realnames = "Alice"};
    const Identity denied{
        .username = "denied", .permissions = {.read = "", .post = ""}, .realnames = "Alice"};
    CHECK_FALSE(backend.index(group, denied, {}));
    CHECK_FALSE(backend.article(group, 1, denied, {}, {}));
    CHECK_FALSE(backend.article(group, 2, alice, {}, {}));
    CHECK_FALSE(backend.article(group, 100, alice, {}, {}));
    std::stop_source stopped;
    stopped.request_stop();
    CHECK_FALSE(backend.index(group, alice, stopped.get_token()));
    const std::string prefix = "From: Alice <a@example.org>\r\nNewsgroups: test\r\nContent-Type: "
                               "text/plain; charset=UTF-8\r\n";
    const auto post = [&](std::string_view extra) -> std::string {
        return prefix + std::string{extra} + "\r\nBody\r\n";
    };
    for (const auto *reference :
         {"garbage", "<0$test@JamNNTPd>", "<x$test@JamNNTPd>", "<1$test@other>",
          "<1$missing@JamNNTPd>", "<2$test@JamNNTPd>", "<100$test@JamNNTPd>"}) {
        CHECK_FALSE(
            backend.submit(post("References: " + std::string{reference} + "\r\n"), alice, {}, {}));
        CHECK_FALSE(backend.submit(post("Control: cancel " + std::string{reference} + "\r\n"),
                                   alice, {}, {}));
    }
    CHECK_FALSE(backend.submit(post("Control: unknown thing\r\n"), alice, {}, {}));
    CHECK_FALSE(backend.submit(
        post("Control: cancel <1$test@JamNNTPd>\r\n"),
        Identity{.username = "bob", .permissions = {.read = "a", .post = "a"}, .realnames = "Bob"},
        {}, {}));
    CHECK_FALSE(backend.submit(post(""), denied, {}, {}));
    CHECK_FALSE(backend.submit(post(""), alice, {}, stopped.get_token()));
    CHECK_FALSE(backend.submit(std::string(maximum_post_bytes + 1, 'x'), alice, {}, {}));
    CHECK_FALSE(backend.submit("broken", alice, {}, {}));
    config.no_cancel = true;
    JamBackend no_cancel{config, *rules, {group}};
    CHECK_FALSE(no_cancel.submit(post("Control: cancel <1$test@JamNNTPd>\r\n"), alice, {}, {}));
    Group missing = group;
    missing.base = directory.file("missing");
    JamBackend unavailable{Config{}, *rules, {missing}};
    CHECK_FALSE(unavailable.index(missing, alice, {}));
    CHECK_FALSE(unavailable.article(missing, 1, alice, {}, {}));
    CHECK_FALSE(unavailable.submit(post(""), alice, {}, {}));
    const auto storage = jam::Storage::open(base);
    REQUIRE(storage);
    CHECK(storage->info()->slots == 2);
}
