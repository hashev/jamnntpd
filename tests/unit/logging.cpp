#include "core/logging.hpp"
#include "temp_directory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/ostream_sink.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace jamnntpd;
TEST_CASE("Protocol diagnostics redact every authentication command before reaching sinks") {
    std::ostringstream output;
    const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    ServiceLogger logger{{sink}, true};
    for (auto line :
         {"AUTHINFO PASS secret-sentinel", "authinfo\tpass\tsecret-sentinel",
          " AUTHINFO USER secret-sentinel", "AUTHINFO PASS \"unterminated-secret-sentinel"}) {
        logger.protocol_input("client", line);
    }
    logger.protocol_input("client", "GROUP echo");
    logger.flush();
    CHECK(output.str().find("secret-sentinel") == std::string::npos);
    CHECK(output.str().find("AUTHINFO [redacted]") != std::string::npos);
    CHECK(output.str().find("GROUP echo") != std::string::npos);
}
TEST_CASE("Thread-safe logger keeps long records complete and prevents line injection") {
    std::ostringstream output;
    const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    ServiceLogger logger{{sink}, true};
    const std::string payload(5000, 'x');
    std::vector<std::jthread> workers;
    for (int number = 0; number < 8; ++number) {
        workers.emplace_back([&, number] {
            for (int count = 0; count < 10; ++count) {
                logger.write(spdlog::level::info, std::to_string(number), payload);
            }
        });
    }
    workers.clear();
    logger.write(spdlog::level::warn, "forged\r\nclient", "line\nsecond");
    logger.flush();
    std::istringstream records{output.str()};
    std::string line;
    int count{};
    while (std::getline(records, line)) {
#ifdef _WIN32
        REQUIRE(line.ends_with('\r'));
        line.pop_back();
#endif
        if (count < 80) {
            CHECK(line.ends_with(payload));
        } else {
            CHECK(line.find("forged\\r\\nclient") != std::string::npos);
            CHECK(line.find("line\\nsecond") != std::string::npos);
        }
        ++count;
    }
    CHECK(count == 81);
}
TEST_CASE("Diagnostic records escape terminal controls as printable data") {
    std::ostringstream output;
    const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    ServiceLogger logger{{sink}, true};
    logger.protocol_input("client", "UNKNOWN\t\x1b[31m\x7f");
    logger.flush();
    CHECK(output.str().find("UNKNOWN\\x09\\x1B[31m\\x7F") != std::string::npos);
    CHECK(output.str().find('\x1b') == std::string::npos);
    CHECK(output.str().find('\x7f') == std::string::npos);
}
TEST_CASE("Log destination failures do not recurse or escape workers") {
    const auto sink =
        std::make_shared<spdlog::sinks::callback_sink_mt>([](const spdlog::details::log_msg &) {
            throw std::runtime_error{"injected sink failure"};
        });
    ServiceLogger logger{{sink}, true};
    CHECK_NOTHROW(logger.write(spdlog::level::info, "client", "event"));
    CHECK_NOTHROW(logger.flush());
    TempDirectory directory;
    auto config = default_config();
    config.log_file = directory.file("missing-parent") / "test.log";
    // spdlog creates parent directories; a directory used as a file is an actual failure.
    config.log_file = directory.file("directory.log");
    std::filesystem::create_directory(config.log_file);
    CHECK_FALSE(ServiceLogger::create(config));
}
TEST_CASE("File logger honors its destination and flushes complete output") {
    TempDirectory directory;
    auto config = default_config();
    config.log_file = directory.file("service.log");
    config.no_echo_log = true;
    config.debug = true;
    auto logger = ServiceLogger::create(config);
    REQUIRE(logger);
    (*logger)->write(spdlog::level::info, "test", "visible event");
    (*logger)->protocol_input("test", "AUTHINFO PASS never-log-this");
    (*logger)->flush();
    std::ifstream input{config.log_file};
    const std::string contents{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
    CHECK(contents.find("visible event") != std::string::npos);
    CHECK(contents.find("never-log-this") == std::string::npos);
}
