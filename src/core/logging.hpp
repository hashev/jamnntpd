#ifndef JAMNNTPD_CORE_LOGGING_HPP
#define JAMNNTPD_CORE_LOGGING_HPP
#include "config.hpp"
#include <fstream>
#include <memory>
#include <spdlog/logger.h>
#include <string_view>
#include <vector>
namespace jamnntpd {
class ServiceLogger {
  public:
    explicit ServiceLogger(const std::vector<spdlog::sink_ptr> &sinks, bool debug);
    [[nodiscard]] static auto create(const Config &config)
        -> Result<std::unique_ptr<ServiceLogger>>;
    void write(spdlog::level::level_enum level, std::string_view session, std::string_view message);
    void protocol_input(std::string_view session, std::string_view input);
    void flush();

  private:
    // The stream must outlive the sinks held by logger_.
    std::unique_ptr<std::ofstream> file_;
    spdlog::logger logger_;
};
} // namespace jamnntpd
#endif
