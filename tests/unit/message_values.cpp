#include "core/message_values.hpp"
#include <array>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <thread>
#include <vector>

using namespace jamnntpd;
TEST_CASE("JAM timezone offsets use signed checked minutes") {
    CHECK(parse_utc_offset("-0330")->value == std::chrono::minutes{-210});
    CHECK(parse_utc_offset("0530")->value == std::chrono::minutes{330});
    CHECK(*format_utc_offset(*parse_utc_offset("-0030")) == "-0030");
    CHECK(*format_utc_offset(*parse_utc_offset("+0000")) == "+0000");
    for (const auto *offset : {"", "030", "2400", "1260", "00-1", "03000", "--0300", " 0300"}) {
        CHECK_FALSE(parse_utc_offset(offset));
    }
    CHECK_FALSE(format_utc_offset({std::chrono::minutes{1440}}));
    CHECK_FALSE(format_utc_offset({std::chrono::minutes::min()}));
}
TEST_CASE("JAM dates use stored wall clock and explicit timezone without host TZ arithmetic") {
    CHECK(*format_jam_date(0, "-0330") == "Thu, 1 Jan 1970 00:00:00 -0330");
    CHECK(*format_jam_date(1709168523, "0530") == "Thu, 29 Feb 2024 01:02:03 +0530");
    CHECK(*format_jam_date(0xffffffffU, "") == "Sun, 7 Feb 2106 06:28:15 GMT");
    CHECK_FALSE(format_jam_date(0, "garbage"));
    CHECK_FALSE(local_jam_time(std::chrono::sys_seconds::max()));
    CHECK_FALSE(local_jam_time(std::chrono::sys_seconds::min()));
    using namespace std::chrono;
    const auto winter = local_jam_time(sys_days{2024y / January / 15} + 12h);
    const auto summer = local_jam_time(sys_days{2024y / July / 15} + 12h);
    REQUIRE(winter);
    REQUIRE(summer);
    CHECK(
        static_cast<std::int64_t>(winter->wall_seconds) - winter->offset.value.count() * 60 ==
        duration_cast<seconds>((sys_days{2024y / January / 15} + 12h).time_since_epoch()).count());
    CHECK(static_cast<std::int64_t>(summer->wall_seconds) - summer->offset.value.count() * 60 ==
          duration_cast<seconds>((sys_days{2024y / July / 15} + 12h).time_since_epoch()).count());
}
TEST_CASE("Concurrent time formatting and local conversion have no shared tm buffers") {
    constexpr int workers = 8;
    std::barrier start{workers};
    std::array<bool, workers> valid{};
    std::vector<std::jthread> threads;
    for (std::size_t index = 0; index < valid.size(); ++index) {
        threads.emplace_back([&, index] {
            start.arrive_and_wait();
            valid.at(index) = true;
            for (int iteration = 0; iteration < 100; ++iteration) {
                const auto date = format_jam_date(1709168523, "-0030");
                const auto local =
                    local_jam_time(std::chrono::sys_seconds{std::chrono::seconds{1709168523}});
                if (!date || !local || *date != "Thu, 29 Feb 2024 01:02:03 -0030") {
                    valid.at(index) = false;
                }
            }
        });
    }
    threads.clear();
    for (bool success : valid) {
        CHECK(success);
    }
}
TEST_CASE("FTN addresses require complete numeric components and preserve legacy normalization") {
    const auto address = parse_ftn_address("002:0001/0002.0003@fidonet");
    REQUIRE(address);
    CHECK(address->text() == "2:1/2.3");
    CHECK(address->mailbox() == "3@2.1.2");
    CHECK(parse_ftn_address("2:1/2.0")->text() == "2:1/2");
    CHECK(parse_ftn_address("4294967295:0/0")->zone == 0xffffffffU);
    for (const auto *text : {"", "2:1", "2/1:2", "2:1/2x", "-1:1/2", "+1:1/2", "2:1/2.",
                             "2:1/2.3.4", "4294967296:1/2", "2:1/2\r", " 2:1/2"}) {
        CHECK_FALSE(parse_ftn_address(text));
    }
    CHECK(origin_address("body\r * Origin: Old (1:2/3)\r * Origin: New (2:3/4)\r")->text() ==
          "2:3/4");
    CHECK_FALSE(origin_address(" * Origin: no address\r"));
    CHECK_FALSE(origin_address(" * Origin: bad (2:3/4\r"));
}
TEST_CASE("Mailbox parsing owns Unicode text and rejects header framing injection") {
    const auto mailbox = parse_mailbox(" \"Иван \\\"И\\\" Петров\" <name@example.org> ");
    REQUIRE(mailbox);
    CHECK(mailbox->name == "Иван \"И\" Петров");
    CHECK(mailbox->address == "name@example.org");
    CHECK(parse_mailbox("Name Only")->name == "Name Only");
    CHECK(parse_mailbox("x@y")->address == "x@y");
    CHECK(parse_mailbox("\"x@y\"")->name == "x@y");
    for (const auto *text :
         {"Name <x@y", "a@b,c@d", "\"unterminated", "x\r\nInjected: a", "Name <x@y> trailing",
          "Name <>", "Name <a b>", "x@y>", "x@y z", "\"bad\\\" <x@y>", "\"a\"b\" <x@y>"}) {
        CHECK_FALSE(parse_mailbox(text));
    }
    CHECK_FALSE(parse_mailbox(std::string{"a\0b", 3}));
}

TEST_CASE("Native local JAM conversion honors winter summer DST and date rollover", "[native-tz]") {
    using namespace std::chrono;
    const auto winter = local_jam_time(sys_days{2024y / January / 15} + 2h);
    const auto summer = local_jam_time(sys_days{2024y / July / 15} + 2h);
    REQUIRE(winter);
    REQUIRE(summer);
    CHECK(winter->offset.value == -5h);
    CHECK(summer->offset.value == -4h);
    CHECK(*format_jam_date(winter->wall_seconds, "-0500") == "Sun, 14 Jan 2024 21:00:00 -0500");
    CHECK(*format_jam_date(summer->wall_seconds, "-0400") == "Sun, 14 Jul 2024 22:00:00 -0400");
}
