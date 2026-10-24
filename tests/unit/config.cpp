#include "core/config.hpp"
#include "core/text.hpp"
#include "temp_directory.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <string>
#include <vector>

using namespace jamnntpd;
TEST_CASE("Configuration preserves aliases and validates values atomically") {
    auto config = default_config();
    const std::vector<std::string> args{"--P",
                                        "1234",
                                        "-m",
                                        "9",
                                        "-debug",
                                        "-smartquote",
                                        "-a",
                                        "path with spaces",
                                        "-g",
                                        "groups",
                                        "-u",
                                        "users",
                                        "-x",
                                        "xlat",
                                        "-l",
                                        "log",
                                        "-origin",
                                        "Some Origin",
                                        "-guestsuffix",
                                        " Guest",
                                        "-conversion_errors",
                                        "replace",
                                        "-def_flowed",
                                        "off",
                                        "-def_showto",
                                        "off"};
    REQUIRE(apply_config_arguments(config, args));
    CHECK(config.port == 1234);
    CHECK(config.maximum_connections == 9);
    CHECK(config.allow_file == "path with spaces");
    CHECK(config.defaults.squote);
    CHECK_FALSE(config.defaults.flowed);
    CHECK_FALSE(config.defaults.showto);
    CHECK(config.conversion_policy == ConversionPolicy::replace);
    for (const auto &bad :
         std::vector<std::vector<std::string>>{std::vector<std::string>{"port", "65536"},
                                               {"port", "-1"},
                                               {"port", "80junk"},
                                               {"port"},

                                               {"max", "-1"},
                                               {"unknown", "value"},
                                               {"def_flowed", "maybe"},
                                               {"def_squote", "off"},
                                               {"conversion_errors", "ignore"},
                                               {"config", "nested.config"},
                                               {"create", "nested.config"}}) {
        CHECK_FALSE(apply_config_arguments(config, bad));
        CHECK(config.port == 1234);
    }
    const std::vector<std::string> partial{"port", "7000", "max", "invalid"};
    CHECK_FALSE(apply_config_arguments(config, partial));
    CHECK(config.port == 1234);
}
TEST_CASE("Configuration files roundtrip documented settings and fail safely") {
    TempDirectory directory;
    auto config = default_config();
    const auto original = config_text(config);
    REQUIRE(load_config(config, directory.write("default.config", original)));
    CHECK(config_text(config) == original);
    REQUIRE(load_config(
        config, directory.write("good.config", "# comment\nport 0\nmax 12\norigin \"A B\"\n")));
    CHECK(config.port == 0);
    CHECK(config.maximum_connections == 12);
    CHECK(config.origin == "A B");
    CHECK_FALSE(load_config(config, directory.write("bad.config", "port 42\norigin \"unclosed\n")));
    CHECK(config.port == 0);
    CHECK_FALSE(load_config(config, directory.write("big.config", std::string(65537, 'a'))));
    CHECK_FALSE(load_config(config, directory.file("missing.config")));
    UserOptions options;
    for (auto name : std::array{"flowed", "showto", "nonbsp", "delssq", "addcr", "squote"}) {
        REQUIRE(set_user_option(options, {name, "OFF"}));
        REQUIRE(set_user_option(options, {name, "on"}));
    }
    CHECK_FALSE(set_user_option(options, {"unknown", "on"}));
    CHECK_FALSE(set_user_option(options, {"flowed", "yes"}));
}
TEST_CASE("Group syntax retains netmail local forced charset and spaces") {
    TempDirectory directory;
    auto groups = load_groups(
        directory.write("groups", "!NETMAIL A 2:1/2 \"base with spaces\" !CP866 -nochrs\n$LOCAL B "
                                  "2:1/2 local\nECHO A 2:1/2 echo\n"));
    REQUIRE(groups);
    REQUIRE(groups->size() == 3);
    CHECK(groups->at(0).name == "NETMAIL");
    CHECK(groups->at(0).netmail);
    CHECK(groups->at(0).no_chrs);
    CHECK(groups->at(0).default_charset == "!CP866");
    CHECK(groups->at(0).base == "base with spaces");
    CHECK(groups->at(1).local);
    CHECK_FALSE(groups->at(2).netmail);
    for (const auto &text : std::array{"", "ECHO XX 2:1/2 base\n", "! A 2:1/2 base\n",
                                       "ECHO A 2:1/2 base -bad\n", "ECHO A\n"}) {
        CHECK_FALSE(load_groups(directory.write("invalid", text)));
    }
}
TEST_CASE("Allow and authentication rules do not truncate credentials or names") {
    TempDirectory directory;
    auto allow = directory.write("allow", "127.0.*.ignored AX A\n10.*\n");
    auto permissions = guest_permissions(allow, "127.0.0.1");
    REQUIRE(permissions);
    CHECK(permissions->read == "AX");
    CHECK(permissions->post == "A");
    CHECK(guest_permissions(allow, "10.0.0.1")->read.empty());
    CHECK_FALSE(guest_permissions(allow, "192.0.2.1"));
    CHECK_FALSE(guest_permissions(directory.write("invalid-allow", "* A B C\n"), "127.0.0.1"));
    for (std::size_t length : {35U, 36U, 99U, 10000U}) {
        const std::string name(length, 'N');
        auto users = directory.write("users", "alice Secret AX B \"" + name + "\"\n");
        const auto identity = authenticate(users, {"ALICE", "Secret"});
        REQUIRE(identity);
        CHECK(identity->realnames == name);
        CHECK(identity->permissions.post == "B");
        CHECK_FALSE(authenticate(users, {"alice", "secret"}));
        CHECK_FALSE(authenticate(users, {"nobody", "Secret"}));
    }
    CHECK_FALSE(authenticate(directory.write("bad-users", "alice Secret\n"), {"alice", "Secret"}));
}

TEST_CASE("Legacy defaults distinguish omitted origin from an explicit empty override") {
    TempDirectory directory;
    auto config = default_config();
    CHECK_FALSE(config.origin);
    CHECK_FALSE(config.guest_suffix);
    CHECK(config_text(config).find("origin ") == std::string::npos);
    const std::vector<std::string> arguments{"max",         "0", "origin",    "",
                                             "guestsuffix", "",  "smartquote"};
    REQUIRE(apply_config_arguments(config, arguments));
    CHECK(config.maximum_connections == 0);
    REQUIRE(config.origin);
    CHECK(config.origin->empty());
    REQUIRE(config.guest_suffix);
    CHECK(config.guest_suffix->empty());
    const auto saved = config_text(config);
    CHECK(saved.find("def_squote") == std::string::npos);
    CHECK(saved.find("smartquote\n") != std::string::npos);
    auto restored = default_config();
    REQUIRE(load_config(restored, directory.write("legacy.config", saved)));
    REQUIRE(restored.origin);
    CHECK(restored.origin->empty());
    CHECK(restored.maximum_connections == 0);
    CHECK(restored.defaults.squote);
}

TEST_CASE("Command line applies config at its position and preserves help and create actions") {
    TempDirectory directory;
    const auto path = directory.write("ordered.cfg", "port 5011\nmax 0\n");
    const auto implicit = directory.file("absent.cfg");
    const std::vector<std::string> arguments{"-port", "5010", "--config", path.string(),
                                             "-port", "5012", "-debug"};
    const auto command = parse_command_line(arguments, implicit);
    REQUIRE(command);
    CHECK(command->action == CommandAction::run);
    CHECK(command->config.port == 5012);
    CHECK(command->config.maximum_connections == 0);
    CHECK(command->config.debug);
    CHECK(parse_command_line({}, path)->config.port == 5011);
    CHECK(parse_command_line({}, implicit)->config.port == default_port);
    for (const auto *help : {"?", "-h", "--help"}) {
        const std::vector<std::string> options{help};
        CHECK(parse_command_line(options, implicit)->action == CommandAction::help);
    }
    const auto output = directory.file("new.cfg");
    const std::vector<std::string> create{"-port", "5012", "-create", output.string()};
    const auto created = parse_command_line(create, implicit);
    REQUIRE(created);
    CHECK(created->action == CommandAction::create_config);
    CHECK(created->config.port == default_port);
    CHECK(created->output_file == output);
    CHECK_FALSE(std::filesystem::exists(output)); // parsing has no file write side effects
    for (const auto &options : std::vector<std::vector<std::string>>{{"port", "5"},
                                                                     {"-config"},
                                                                     {"-create"},
                                                                     {"-port"},
                                                                     {"-unknown", "value"},
                                                                     {"-config", implicit.string()},
                                                                     {"--help", "-debug"}}) {
        CHECK_FALSE(parse_command_line(options, implicit));
    }
    directory.write("nested.cfg", "config other.cfg\n");
    auto config = default_config();
    CHECK_FALSE(load_config(config, directory.file("nested.cfg")));
    CHECK_FALSE(load_config(config, directory.file("")));
    const auto large = directory.write("large.cfg", std::string(65537, 'x'));
    CHECK_FALSE(load_config(config, large));
}

TEST_CASE("Group names reject protocol framing and message-id delimiters") {
    TempDirectory directory;
    for (const auto &name : {std::string{"a b"}, std::string{"a\rb"}, std::string{"x@JamNNTPd"},
                             std::string{"x$y"}, std::string{"a,b"}, std::string(100, 'a')}) {
        const auto file = directory.write("groups", "\"" + name + "\" a 2:1/2 base\n");
        CHECK_FALSE(load_groups(file));
    }
}
