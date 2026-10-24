#ifndef JAMNNTPD_CORE_CODEC_HPP
#define JAMNNTPD_CORE_CODEC_HPP

#include "error.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <cstddef>

namespace jamnntpd {
enum class ConversionPolicy : std::uint8_t { strict, replace };
inline constexpr std::size_t chs_entries = 256;
struct SourceEncoding {
    std::string_view name;
};
struct DestinationEncoding {
    std::string_view name;
};
struct Conversion {
    std::string bytes;
    bool replaced{};
};
[[nodiscard]] auto transcode(std::string_view input, SourceEncoding source,
                             DestinationEncoding destination,
                             ConversionPolicy policy = ConversionPolicy::strict)
    -> Result<Conversion>;
struct ChsTable {
    std::string source;
    std::string destination;
    std::array<std::string, chs_entries> entries{};
    [[nodiscard]] auto apply(std::string_view input) const -> std::string;
};
[[nodiscard]] auto parse_chs(std::string_view input, const std::filesystem::path &filename)
    -> Result<ChsTable>;
[[nodiscard]] auto load_chs(const std::filesystem::path &filename) -> Result<ChsTable>;
} // namespace jamnntpd

#endif
