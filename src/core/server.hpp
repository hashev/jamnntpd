#ifndef JAMNNTPD_CORE_SERVER_HPP
#define JAMNNTPD_CORE_SERVER_HPP
#include "config.hpp"
namespace jamnntpd {
[[nodiscard]] auto run_server(const Config &config) -> Result<void>;
[[nodiscard]] auto command_help() -> std::string_view;
} // namespace jamnntpd
#endif
