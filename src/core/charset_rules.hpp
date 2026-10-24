#ifndef JAMNNTPD_CORE_CHARSET_RULES_HPP
#define JAMNNTPD_CORE_CHARSET_RULES_HPP

#include "codec.hpp"
#include <optional>
#include <vector>

#include <cstdint>
#include <string>
#include <string_view>

namespace jamnntpd {
struct Charset {
    std::string chrs;
    std::string codepage;
};
struct CharsetAlias {
    std::string pattern;
    std::string replacement;
};
enum class ConversionMethod : std::uint8_t { pass_through, builtin, table };
struct CharsetRule {
    std::string source_pattern;
    std::string destination;
    ConversionMethod method{ConversionMethod::pass_through};
    std::optional<ChsTable> table;
    bool keep_soft_cr{};
};
struct CharsetConfig {
    std::string default_read;
    std::string default_post;
    std::vector<CharsetAlias> read_aliases;
    std::vector<CharsetAlias> post_aliases;
    std::vector<CharsetRule> read_rules;
    std::vector<CharsetRule> post_rules;
};
struct ConversionPlan {
    Charset source;
    Charset destination;
    ConversionMethod method{ConversionMethod::pass_through};
    std::optional<ChsTable> table;
    bool keep_soft_cr{};
    [[nodiscard]] auto apply(std::string_view bytes,
                             ConversionPolicy policy = ConversionPolicy::strict) const
        -> Result<Conversion>;
};
struct ReadCharsetRequest {
    Charset metadata;
    std::string_view group_default;
    std::optional<std::string_view> destination_pattern;
};
struct PostCharsetRequest {
    std::string_view source;
    std::string_view group_default;
};
[[nodiscard]] auto parse_charset_config(std::string_view text,
                                        const std::filesystem::path &filename)
    -> Result<CharsetConfig>;
[[nodiscard]] auto load_charset_config(const std::filesystem::path &filename)
    -> Result<CharsetConfig>;
[[nodiscard]] auto resolve_read(const CharsetConfig &config, const ReadCharsetRequest &request)
    -> Result<ConversionPlan>;
[[nodiscard]] auto resolve_post(const CharsetConfig &config, const PostCharsetRequest &request)
    -> Result<ConversionPlan>;
} // namespace jamnntpd
#endif
