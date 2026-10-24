#ifndef JAMNNTPD_CORE_TEXT_HPP
#define JAMNNTPD_CORE_TEXT_HPP

#include "error.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jamnntpd {
enum class NumberBase : std::uint8_t { decimal = 10, hexadecimal = 16 };
struct NamePatterns {
    std::string_view text;
};
[[nodiscard]] auto ascii_lower(char value) noexcept -> char;
[[nodiscard]] auto lower_ascii(std::string_view text) -> std::string;
[[nodiscard]] auto equal_ascii(std::string_view left, std::string_view right) noexcept -> bool;
[[nodiscard]] auto match_pattern(std::string_view pattern, std::string_view text) -> bool;
[[nodiscard]] auto match_names(NamePatterns patterns, std::string_view text) -> bool;
[[nodiscard]] auto match_group(std::string_view groups, char group) noexcept -> bool;
[[nodiscard]] auto trim(std::string_view text) noexcept -> std::string_view;
[[nodiscard]] auto config_words(std::string_view line) -> Result<std::vector<std::string>>;
[[nodiscard]] auto parse_number(std::string_view text, std::uint32_t maximum,
                                NumberBase base = NumberBase::decimal) -> Result<std::uint32_t>;
} // namespace jamnntpd

#endif
