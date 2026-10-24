#ifndef JAMNNTPD_CORE_FORMATTING_HPP
#define JAMNNTPD_CORE_FORMATTING_HPP
#include "codec.hpp"
#include <vector>

#include <cstddef>
#include <string>
#include <string_view>

namespace jamnntpd {
struct PostTextOptions {
    bool flowed{};
    bool delete_stuffed_quote_space{true};
    bool replace_nbsp{};
    std::string quote_name;
};
struct TearlineOptions {
    bool add_cr{true};
    bool no_tearline{};
    std::string newsreader;
    std::string origin;
    std::string address;
};
// Source bytes must be processed before conversion; UTF-8 continuation bytes and
// DOS letter/glyph bytes are never interpreted as the legacy soft-CR control.
[[nodiscard]] auto remove_soft_cr(std::string_view bytes, SourceEncoding source, bool keep)
    -> std::string;
[[nodiscard]] auto format_post_text(std::string_view utf8, const PostTextOptions &options)
    -> Result<std::string>;
[[nodiscard]] auto append_tearline(std::string_view utf8, const TearlineOptions &options)
    -> Result<std::string>;
// Returns complete dot-stuffed CRLF lines, without the NNTP final dot line.
[[nodiscard]] auto format_read_text(std::string_view utf8, DestinationEncoding destination,
                                    bool flowed, ConversionPolicy policy = ConversionPolicy::strict)
    -> Result<Conversion>;
[[nodiscard]] auto encode_post_text(std::string_view utf8, DestinationEncoding destination,
                                    std::size_t maximum_bytes,
                                    ConversionPolicy policy = ConversionPolicy::strict)
    -> Result<Conversion>;
} // namespace jamnntpd
#endif
