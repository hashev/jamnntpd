#ifndef JAMNNTPD_CORE_MAP_GENERATOR_HPP
#define JAMNNTPD_CORE_MAP_GENERATOR_HPP
#include "codec.hpp"
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
namespace jamnntpd {
using UnicodeMap = std::array<char32_t, chs_entries>;
struct ChsGeneration {
    std::string text;
    std::vector<std::string> warnings;
};
[[nodiscard]] auto read_unicode_map(const std::filesystem::path &path) -> Result<UnicodeMap>;
[[nodiscard]] auto generate_chs(const UnicodeMap &source,
                                const std::optional<UnicodeMap> &destination,
                                SourceEncoding source_name, DestinationEncoding destination_name)
    -> Result<ChsGeneration>;
} // namespace jamnntpd
#endif
