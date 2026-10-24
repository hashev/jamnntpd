#ifndef JAMNNTPD_CORE_MESSAGE_VALUES_HPP
#define JAMNNTPD_CORE_MESSAGE_VALUES_HPP
#include "error.hpp"
#include <chrono>
#include <cstdint>
#include <string_view>

#include <string>

namespace jamnntpd {
struct UtcOffset {
    std::chrono::minutes value{};
};
struct JamTime {
    std::uint32_t wall_seconds{};
    UtcOffset offset;
};
[[nodiscard]] auto parse_utc_offset(std::string_view text) -> Result<UtcOffset>;
[[nodiscard]] auto format_utc_offset(UtcOffset offset) -> Result<std::string>;
[[nodiscard]] auto local_jam_time(std::chrono::sys_seconds time) -> Result<JamTime>;
[[nodiscard]] auto format_jam_date(std::uint32_t wall_seconds, std::string_view timezone)
    -> Result<std::string>;
struct FtnAddress {
    std::uint32_t zone{};
    std::uint32_t net{};
    std::uint32_t node{};
    std::uint32_t point{};
    [[nodiscard]] auto text() const -> std::string;
    [[nodiscard]] auto mailbox() const -> std::string;
};
[[nodiscard]] auto parse_ftn_address(std::string_view text) -> Result<FtnAddress>;
struct Mailbox {
    std::string name;
    std::string address;
};
[[nodiscard]] auto parse_mailbox(std::string_view text) -> Result<Mailbox>;
[[nodiscard]] auto origin_address(std::string_view text) -> Result<FtnAddress>;
} // namespace jamnntpd
#endif
