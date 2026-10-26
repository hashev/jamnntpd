#include "core/session.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>

using namespace jamnntpd;
namespace {
auto services() -> SessionServices {
    return {
        .index = [](const Group &, const Identity &,
                    const std::stop_token &) -> Result<GroupIndex> {
            return GroupIndex{.count = 2, .first = 1, .last = 3, .visible = {1, 3}};
        },
        .article = [](const Group &, std::uint32_t number, const Identity &, const UserOptions &,
                      const std::stop_token &) -> Result<ProtocolArticle> {
            if (number != 1 && number != 3) {
                return failure(ErrorKind::permission, "not visible");
            }
            return ProtocolArticle{.headers = "Subject: Test\r\n",
                                   .body = "..line\r\n",
                                   .overview = "Test\tAlice\tdate\t<id>\t\t\t"};
        },
        .submit = [](std::string_view, const Identity &, const UserOptions &,
                     const std::stop_token &) -> Result<std::string> { return "Article posted"; },
        .login = [](Credentials credentials) -> Result<Identity> {
            if (credentials.username != "alice" || credentials.password != "secret") {
                return failure(ErrorKind::permission, "denied");
            }
            return Identity{.username = "alice",
                            .permissions = {.read = "a", .post = "a"},
                            .realnames = std::string(200, 'x')};
        },
    };
}
auto make_session() -> Session {
    std::vector<Group> groups{{.name = "test",
                               .access = 'a',
                               .address = "2:1/2",
                               .base = "unused",
                               .default_charset = "CP866"},
                              {.name = "hidden",
                               .access = 'b',
                               .address = "2:1/2",
                               .base = "unused",
                               .default_charset = "CP866"}};
    return Session{std::move(groups),
                   {.username = "", .permissions = {.read = "a", .post = "a"}, .realnames = ""},
                   {},
                   services()};
}
} // namespace
TEST_CASE("Session preserves basic transcripts and HELP content") {
    auto session = make_session();
    CHECK(session.command("GROUP").bytes == "501 No group specified\r\n");
    CHECK(session.command("GROUP hidden").bytes == "411 No such newsgroup\r\n");
    CHECK(session.command("LIST NEWSGROUPS *").bytes ==
          "501 Patterns not supported for LIST NEWSGROUPS\r\n");
    CHECK(session.command("LIST unknown").bytes == "501 Unknown argument for LIST command\r\n");
    CHECK(session.command("LIST").bytes == "215 List of newsgroups follows\r\ntest 3 1 y\r\n.\r\n");
    CHECK(session.command("LIST NEWSGROUPS").bytes ==
          "215 List of newsgroups follows\r\ntest\t\r\n.\r\n");
    CHECK(session.command("MODE READER").bytes == "200 Server ready - posting allowed\r\n");
    CHECK(session.command("IHAVE <id>").bytes == "435 Article not wanted - do not send it\r\n");
    CHECK(session.command("NEWGROUPS").bytes ==
          "231 Warning: NEWGROUPS not implemented, returning empty list\r\n.\r\n");
    CHECK(session.command("NEWNEWS").bytes ==
          "230 Warning: NEWNEWS not implemented, returning empty list\r\n.\r\n");
    CHECK(session.command("SLAVE").bytes == "202 Slave status noted (but ignored)\r\n");
    CHECK(session.command("unknown").bytes == "500 Unknown command\r\n");
    CHECK(session.command(" ").bytes.empty());
    std::ifstream input{std::filesystem::path{JAM_FIXTURE_DIR} / "protocol/help-response.txt",
                        std::ios::binary};
    std::ostringstream contents;
    contents << input.rdbuf();
    CHECK(session.command("HELP").bytes == contents.str());
    CHECK(session.command("QUIT").action == ProtocolAction::close);
    CHECK(session.command(std::string(999, 'x')).action == ProtocolAction::close);
    CHECK(session.command("HELP\r\nQUIT").action == ProtocolAction::close);
}
TEST_CASE("Session article state validates identifiers and hides deleted and private articles even "
          "for STAT") {
    auto session = make_session();
    CHECK(session.command("STAT").bytes == "412 No newsgroup selected\r\n");
    CHECK(session.command("GROUP test").bytes == "211 2 1 3 test Group selected\r\n");
    CHECK(session.command("STAT").bytes == "223 1 <1$test@JamNNTPd> Article retrieved\r\n");
    CHECK(session.command("STAT 2").bytes == "423 No article with that number\r\n");
    CHECK(session.command("STAT <2$test@JamNNTPd>").bytes == "430 No such article found\r\n");
    CHECK(session.command("STAT <1$hidden@JamNNTPd>").bytes == "430 No such article found\r\n");
    CHECK(session.command("STAT <1$test@wrong>").bytes == "430 No such article found\r\n");
    CHECK(session.command("STAT <4294967296$test@JamNNTPd>").bytes ==
          "430 No such article found\r\n");
    CHECK(session.command("STAT -1").bytes.starts_with("501"));
    CHECK(session.command("STAT 1x").bytes.starts_with("501"));
    CHECK(session.command("NEXT").bytes == "223 3 <3$test@JamNNTPd> Article retrieved\r\n");
    CHECK(session.command("NEXT").bytes.starts_with("421"));
    CHECK(session.command("LAST").bytes == "223 1 <1$test@JamNNTPd> Article retrieved\r\n");
    CHECK(session.command("LAST").bytes.starts_with("422"));
    CHECK(session.command("ARTICLE").bytes ==
          "220 1 <1$test@JamNNTPd> Article retrieved - Head and body follow\r\nSubject: "
          "Test\r\n\r\n..line\r\n.\r\n");
    CHECK(session.command("HEAD").bytes ==
          "221 1 <1$test@JamNNTPd> Article retrieved - Head follows\r\nSubject: Test\r\n.\r\n");
    CHECK(session.command("BODY").bytes ==
          "222 1 <1$test@JamNNTPd> Article retrieved - Body follows\r\n..line\r\n.\r\n");
    CHECK(
        session.command("XOVER 1-").bytes ==
        "224 Overview information "
        "follows\r\n1\tTest\tAlice\tdate\t<id>\t\t\t\r\n3\tTest\tAlice\tdate\t<id>\t\t\t\r\n.\r\n");
    CHECK(session.command("XOVER 4-5").bytes.starts_with("420"));
    CHECK(session.command("XOVER 1-x").bytes.starts_with("501"));
}
TEST_CASE("AUTHINFO commits identity and options together only after successful authentication") {
    auto session = make_session();
    CHECK(session.command("AUTHINFO").bytes ==
          "501 Only AUTHINFO USER or AUTHINFO pass are understood\r\n");
    CHECK(session.command("AUTHINFO PASS sentinel").bytes ==
          "482 Use AUTHINFO USER before AUTHINFO pass\r\n");
    CHECK(session.command("AUTHINFO USER").bytes == "482 No user specified for AUTHINFO USER\r\n");
    CHECK(session.command("AUTHINFO USER alice/flowed=off,showto=off").bytes.starts_with("381"));
    CHECK(session.command("AUTHINFO PASS").bytes.starts_with("482"));
    CHECK(session.command("AUTHINFO PASS bad").bytes == "481 Authentication rejected\r\n");
    CHECK(session.options().flowed);
    CHECK(session.identity().username.empty());
    CHECK(session.command("AUTHINFO PASS secret").bytes == "281 Authentication accepted\r\n");
    CHECK_FALSE(session.options().flowed);
    CHECK_FALSE(session.options().showto);
    CHECK(session.identity().realnames.size() == 200);
    CHECK(session.command("AUTHINFO USER /nonbsp=on").bytes.starts_with("381"));
    CHECK(session.command("AUTHINFO PASS any").bytes ==
          "281 Authentication accepted (options set, no login)\r\n");
    CHECK(session.options().nonbsp);
    CHECK(session.command("AUTHINFO USER alice/invalid").bytes.starts_with("381"));
    CHECK(session.command("AUTHINFO PASS secret").bytes.starts_with("482"));
    CHECK(session.command("AUTHINFO USER alice/flowed=maybe").bytes.starts_with("381"));
    CHECK(session.command("AUTHINFO PASS secret").bytes.starts_with("482"));
    CHECK(session.command("POST").action == ProtocolAction::receive_post);
    CHECK(session.post("article").bytes == "240 Article posted\r\n");
}

TEST_CASE(
    "Session rejects invalid state ranges and backend failures without partial article framing") {
    auto session = make_session();
    for (const auto *command : {"ARTICLE", "HEAD", "BODY", "STAT", "NEXT", "LAST", "XOVER"}) {
        CHECK(session.command(command).bytes == "412 No newsgroup selected\r\n");
    }
    CHECK(session.command("LIST OVERVIEW.FMT").bytes ==
          "215 List of fields in XOVER "
          "result\r\nSubject:\r\nFrom:\r\nDate:\r\nMessage-ID:\r\nReferences:\r\nBytes:\r\nLines:"
          "\r\n.\r\n");
    CHECK(session.command("GROUP test").bytes.starts_with("211"));
    CHECK(session.selected_group() == "test");
    CHECK(session.command("STAT <3$test@JamNNTPd>").bytes.starts_with("223 3"));
    CHECK(session.command("STAT").bytes.starts_with("223 1"));
    CHECK(session.command("STAT 3").bytes.starts_with("223 3"));
    CHECK(session.command("XOVER").bytes.starts_with("224"));
    CHECK(session.command("XOVER 3-1").bytes.starts_with("420"));
    CHECK(session.command("XOVER 4294967296-").bytes.starts_with("501"));
    CHECK(session.command("STAT <$test@JamNNTPd>").bytes.starts_with("430"));
    CHECK(session.command("STAT <1$@JamNNTPd>").bytes.starts_with("430"));
    CHECK(session.command(std::string{"a\0b", 3}).action == ProtocolAction::close);
    std::stop_source stop;
    stop.request_stop();
    CHECK(session.command("GROUP test", stop.get_token()).action == ProtocolAction::close);
    auto backend = services();
    backend.index = [](const Group &, const Identity &,
                       const std::stop_token &) -> Result<GroupIndex> { return GroupIndex{}; };
    std::vector<Group> groups{
        {.name = "test", .access = 'a', .address = "2:1/2", .base = "none", .default_charset = ""}};
    Session empty{groups,
                  {.username = "", .permissions = {.read = "a", .post = ""}, .realnames = ""},
                  {},
                  backend};
    CHECK(empty.command("GROUP test").bytes == "211 0 0 0 test Group selected\r\n");
    for (const auto *command : {"ARTICLE", "NEXT", "LAST", "XOVER"}) {
        CHECK(empty.command(command).bytes == "420 No current article has been selected\r\n");
    }
    CHECK(empty.command("POST").bytes.starts_with("440"));
    CHECK(empty.post("anything").bytes.starts_with("440"));
    backend = services();
    backend.article = [](const Group &, std::uint32_t, const Identity &, const UserOptions &,
                         const std::stop_token &) -> Result<ProtocolArticle> {
        return failure(ErrorKind::io, "injected");
    };
    backend.submit = [](std::string_view, const Identity &, const UserOptions &,
                        const std::stop_token &) -> Result<std::string> {
        return failure(ErrorKind::io, "injected");
    };
    Session failed{groups,
                   {.username = "", .permissions = {.read = "a", .post = "a"}, .realnames = ""},
                   {},
                   backend};
    CHECK(failed.command("GROUP test").bytes.starts_with("211"));
    CHECK(failed.command("ARTICLE").bytes.starts_with("503"));
    CHECK(failed.command("XOVER 1-3").bytes.starts_with("503"));
    CHECK(failed.post("input").bytes == "503 Local error: Could not access messagebase\r\n");
}
TEST_CASE("Successful reauthentication replaces permissions and invalidates an unauthorized "
          "current group") {
    auto backend = services();
    backend.login = [](Credentials) -> Result<Identity> {
        return Identity{
            .username = "limited", .permissions = {.read = "b", .post = ""}, .realnames = ""};
    };
    std::vector<Group> groups{
        {.name = "test", .access = 'a', .address = "2:1/2", .base = "none", .default_charset = ""}};
    Session session{
        groups,
        {.username = "first", .permissions = {.read = "a", .post = "a"}, .realnames = ""},
        {},
        backend};
    CHECK(session.command("GROUP test").bytes.starts_with("211"));
    CHECK(session.command("AUTHINFO USER limited").bytes.starts_with("381"));
    CHECK(session.command("AUTHINFO PASS value").bytes.starts_with("281"));
    CHECK(session.selected_group().empty());
    CHECK(session.command("STAT").bytes.starts_with("412"));
    CHECK(session.command("GROUP test").bytes.starts_with("411"));
    CHECK(session.command("STAT <1$test@JamNNTPd>").bytes.starts_with("430"));
    CHECK(session.command("POST").bytes.starts_with("440"));
}

TEST_CASE("POST distinguishes local storage failures from article rejection") {
    for (const auto kind :
         {ErrorKind::input, ErrorKind::encoding, ErrorKind::permission, ErrorKind::stopped,
          ErrorKind::io, ErrorKind::corrupt, ErrorKind::timeout}) {
        auto backend = services();
        backend.submit = [kind](std::string_view, const Identity &, const UserOptions &,
                                const std::stop_token &) -> Result<std::string> {
            return failure(kind, "private backend diagnostic");
        };
        Session session{
            {},
            {.username = "alice", .permissions = {.read = "a", .post = "a"}, .realnames = "Alice"},
            {},
            backend};
        REQUIRE(session.command("POST").bytes.starts_with("340"));
        const bool local =
            kind == ErrorKind::io || kind == ErrorKind::corrupt || kind == ErrorKind::timeout;
        CHECK(session.post("input").bytes ==
              (local ? "503 Local error: Could not access messagebase\r\n"
                     : "441 Posting failed\r\n"));
    }
}
