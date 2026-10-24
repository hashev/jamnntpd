#include "logging.hpp"
#include "text.hpp"
#include <atomic>
#include <cstdio>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <string>

namespace jamnntpd {
namespace {
auto safe_record(std::string_view input) -> std::string {
    std::string result;
    for (char value : input) {
        switch (value) {
        case '\r':
            result += "\\r";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\0':
            result += "\\0";
            break;
        default:
            result.push_back(value);
            break;
        }
    }
    return result;
}
void logging_fallback(const std::string & /*unused*/) noexcept {
    static std::atomic_flag reported{};
    if (!reported.test_and_set()) {
        // The original record/error text can contain private data; never echo it here.
        constexpr std::string_view message = "JamNNTPd: logging destination failed\n";
        static_cast<void>(std::fwrite(message.data(), 1, message.size(), stderr));
    }
}
} // namespace
ServiceLogger::ServiceLogger(const std::vector<spdlog::sink_ptr> &sinks, bool debug)
    : logger_("jamnntpd", sinks.begin(), sinks.end()) {
    logger_.set_level(debug ? spdlog::level::debug : spdlog::level::info);
    logger_.set_pattern("%Y-%m-%d %H:%M:%S [%l] %v");
    logger_.set_error_handler(logging_fallback);
}
auto ServiceLogger::create(const Config &config) -> Result<std::unique_ptr<ServiceLogger>> {
    auto file = std::make_unique<std::ofstream>(config.log_file, std::ios::app | std::ios::binary);
    if (!file->is_open()) {
        return failure(ErrorKind::io, "Cannot open configured log destination");
    }
    file->exceptions(std::ios::failbit | std::ios::badbit);
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::ostream_sink_mt>(*file));
    if (!config.no_echo_log) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_sink_mt>());
    }
    auto logger = std::make_unique<ServiceLogger>(sinks, config.debug);
    logger->file_ = std::move(file);
    return logger;
}

void ServiceLogger::write(spdlog::level::level_enum level, std::string_view session,
                          std::string_view message) {
    logger_.log(level, "({}) {}", safe_record(session), safe_record(message));
}
void ServiceLogger::protocol_input(std::string_view session, std::string_view input) {
    input = trim(input);
    const auto command = input.substr(0, input.find_first_of(" \t\r\n"));
    if (equal_ascii(command, "AUTHINFO")) {
        write(spdlog::level::debug, session, "AUTHINFO [redacted]");
    } else {
        write(spdlog::level::debug, session, input);
    }
}
void ServiceLogger::flush() { logger_.flush(); }
} // namespace jamnntpd
