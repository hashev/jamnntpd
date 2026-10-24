#ifndef JAMNNTPD_CORE_MIME_HPP
#define JAMNNTPD_CORE_MIME_HPP
#include "codec.hpp"
#include <functional>
#include <map>
#include <string>
#include <string_view>
namespace jamnntpd {
[[nodiscard]] auto decode_base64(std::string_view input) -> Result<std::string>;
[[nodiscard]] auto encode_base64(std::string_view input) -> std::string;
[[nodiscard]] auto decode_quoted_printable(std::string_view input, bool header)
    -> Result<std::string>;
[[nodiscard]] auto decode_header(std::string_view input, SourceEncoding fallback,
                                 ConversionPolicy policy = ConversionPolicy::strict)
    -> Result<Conversion>;
[[nodiscard]] auto encode_header_words(std::string_view utf8, DestinationEncoding destination,
                                       bool no_encode = false) -> Result<std::string>;
using HeaderDecoder = std::function<Result<Conversion>(std::string_view, SourceEncoding)>;
[[nodiscard]] auto decode_header_with(std::string_view input, SourceEncoding fallback,
                                      const HeaderDecoder &decoder) -> Result<Conversion>;
struct RawMimeArticle {
    std::map<std::string, std::string> headers;
    std::string body;
    std::string source_charset;
    bool flowed{};
};
[[nodiscard]] auto parse_raw_article(std::string_view input, SourceEncoding default_encoding)
    -> Result<RawMimeArticle>;
struct MimeArticle {
    std::map<std::string, std::string> headers;
    std::string body;
    std::string source_charset;
    bool replaced{};
};
[[nodiscard]] auto parse_article(std::string_view input, SourceEncoding default_encoding,
                                 ConversionPolicy policy = ConversionPolicy::strict)
    -> Result<MimeArticle>;
} // namespace jamnntpd
#endif
