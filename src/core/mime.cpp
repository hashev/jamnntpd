#include "mime.hpp"
#include "text.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace jamnntpd {
namespace {
constexpr std::string_view alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr unsigned int sextet_bits = 6;
constexpr unsigned int octet_bits = 8;
constexpr unsigned int sextet_mask = 63;
constexpr unsigned int octet_mask = 255;
constexpr unsigned int two_octet_mask = 65535;
constexpr unsigned int ascii_count = 128;
constexpr std::size_t quartet_size = 4;
constexpr std::size_t triple_size = 3;
constexpr std::size_t encoded_word_limit = 75;
constexpr std::size_t encoded_word_overhead = 7;
constexpr unsigned char continuation_mask = 0xc0;
constexpr unsigned char continuation_bits = 0x80;
auto base64_digit(char value) -> Result<unsigned int> {
    const auto index = alphabet.find(value);
    if (index == std::string_view::npos) {
        return failure(ErrorKind::input, "Invalid base64 digit");
    }
    return static_cast<unsigned int>(index);
}
auto decode_quartet(std::string_view group) -> Result<std::string> {
    std::size_t padding{};
    if (group.ends_with("==")) {
        padding = 2;
    } else if (group.ends_with('=')) {
        padding = 1;
    }
    const auto digits = group.substr(0, quartet_size - padding);
    std::uint32_t bits{};
    for (char value : digits) {
        auto digit = base64_digit(value);
        if (!digit) {
            return std::unexpected(digit.error());
        }
        bits = (bits << sextet_bits) | *digit;
    }
    bits <<= static_cast<unsigned int>(padding) * sextet_bits;
    if ((padding == 2 && (bits & two_octet_mask) != 0) ||
        (padding == 1 && (bits & octet_mask) != 0)) {
        return failure(ErrorKind::input, "Noncanonical base64 tail bits");
    }
    std::string output;
    for (std::size_t index = 0; index < triple_size - padding; ++index) {
        const auto shift = static_cast<unsigned int>(triple_size - index - 1) * octet_bits;
        output.push_back(static_cast<char>((bits >> shift) & octet_mask));
    }
    return output;
}
auto has_header_control(std::string_view value) -> bool {
    return std::ranges::any_of(value, [](char byte) -> bool {
        const auto character = static_cast<unsigned char>(byte);
        return (character < ' ' && character != '\t') || character == '\x7f';
    });
}
struct HeaderBlock {
    std::map<std::string, std::string> headers;
    std::string_view body;
};
auto split_headers(std::string_view input) -> Result<HeaderBlock> {
    HeaderBlock block;
    std::string previous;
    while (!input.empty()) {
        const auto end = input.find('\n');
        auto line = input.substr(0, end);
        if (end == std::string_view::npos) {
            input = {};
        } else {
            input.remove_prefix(end + 1);
        }
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            block.body = input;
            return block;
        }
        if (has_header_control(line)) {
            return failure(ErrorKind::input, "Invalid header control character");
        }
        if (line.starts_with(' ') || line.starts_with('\t')) {
            if (previous.empty()) {
                return failure(ErrorKind::input, "Header continuation without field");
            }
            block.headers.at(previous) += " " + std::string{trim(line)};
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return failure(ErrorKind::input, "Malformed article header");
        }
        previous = lower_ascii(line.substr(0, colon));
        if (previous.find_first_of(" \t") != std::string::npos ||
            block.headers.contains(previous)) {
            return failure(ErrorKind::input, "Invalid or duplicate article header");
        }
        block.headers.emplace(previous, trim(line.substr(colon + 1)));
    }
    return failure(ErrorKind::input, "Article has no header/body separator");
}
auto content_charset(std::string_view type, SourceEncoding default_encoding)
    -> Result<std::string> {
    const auto semicolon = type.find(';');
    if (!type.empty() && !equal_ascii(trim(type.substr(0, semicolon)), "text/plain")) {
        return failure(ErrorKind::input, "Only text/plain articles are supported");
    }
    std::string charset{default_encoding.name};
    while (semicolon != std::string_view::npos && type.contains(';')) {
        type.remove_prefix(type.find(';') + 1);
        const auto part = trim(type.substr(0, type.find(';')));
        const auto equal = part.find('=');
        if (equal != std::string_view::npos &&
            equal_ascii(trim(part.substr(0, equal)), "charset")) {
            auto value = trim(part.substr(equal + 1));
            if (value.starts_with('"') && value.ends_with('"') && value.size() >= 2) {
                value.remove_prefix(1);
                value.remove_suffix(1);
            }
            if (value.empty() || has_header_control(value)) {
                return failure(ErrorKind::input, "Invalid content charset");
            }
            charset = value;
        }
    }
    return charset;
}
struct TransferEncoding {
    std::string_view name;
};
auto decode_transfer(TransferEncoding encoding, std::string_view input) -> Result<std::string> {
    if (encoding.name == "base64") {
        return decode_base64(input);
    }
    if (encoding.name == "quoted-printable") {
        return decode_quoted_printable(input, false);
    }
    if (encoding.name == "7bit" || encoding.name == "8bit") {
        return std::string{input};
    }
    return failure(ErrorKind::input, "Unsupported transfer encoding");
}
auto decode_word_payload(char method, std::string_view input) -> Result<std::string> {
    if (method == 'b') {
        return decode_base64(input);
    }
    if (method == 'q') {
        return decode_quoted_printable(input, true);
    }
    return failure(ErrorKind::input, "Unknown encoded-word method");
}
} // namespace

auto encode_base64(std::string_view input) -> std::string {
    std::string output;
    while (!input.empty()) {
        const auto count = std::min(triple_size, input.size());
        std::uint32_t bits{};
        for (std::size_t index = 0; index < triple_size; ++index) {
            bits <<= octet_bits;
            if (index < count) {
                bits |= static_cast<unsigned char>(input.at(index));
            }
        }
        for (std::size_t index = 0; index < quartet_size; ++index) {
            const auto shift = static_cast<unsigned int>(quartet_size - index - 1) * sextet_bits;
            output.push_back(index > count ? '=' : alphabet.at((bits >> shift) & sextet_mask));
        }
        input.remove_prefix(count);
    }
    return output;
}
auto decode_base64(std::string_view input) -> Result<std::string> {
    std::string compact;
    for (char value : input) {
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
            compact.push_back(value);
        }
    }
    if (compact.size() % quartet_size != 0) {
        return failure(ErrorKind::input, "Truncated base64");
    }
    std::string output;
    for (std::size_t offset = 0; offset < compact.size(); offset += quartet_size) {
        const auto group = std::string_view{compact}.substr(offset, quartet_size);
        if (group.ends_with('=') && offset + quartet_size != compact.size()) {
            return failure(ErrorKind::input, "Base64 data after padding");
        }
        auto decoded = decode_quartet(group);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        output += *decoded;
    }
    return output;
}

auto decode_quoted_printable(std::string_view input, bool header) -> Result<std::string> {
    std::string output;
    while (!input.empty()) {
        const auto value = input.front();
        input.remove_prefix(1);
        if (value == '_' && header) {
            output.push_back(' ');
            continue;
        }
        if (value != '=') {
            output.push_back(value);
            continue;
        }
        if (!header && input.starts_with("\r\n")) {
            input.remove_prefix(2);
            continue;
        }
        if (!header && input.starts_with('\n')) {
            input.remove_prefix(1);
            continue;
        }
        if (input.size() < 2) {
            return failure(ErrorKind::input, "Truncated quoted-printable escape");
        }
        auto byte = parse_number(input.substr(0, 2), octet_mask, NumberBase::hexadecimal);
        if (!byte) {
            return failure(ErrorKind::input, "Invalid quoted-printable escape");
        }
        output.push_back(static_cast<char>(*byte));
        input.remove_prefix(2);
    }
    return output;
}
auto decode_header(std::string_view input, SourceEncoding fallback, ConversionPolicy policy)
    -> Result<Conversion> {
    return decode_header_with(
        input, fallback,
        [policy](std::string_view bytes, SourceEncoding source) -> Result<Conversion> {
            return transcode(bytes, source, {"UTF-8"}, policy);
        });
}
auto decode_header_with(std::string_view input, SourceEncoding fallback,
                        const HeaderDecoder &decoder) -> Result<Conversion> {
    Conversion result;
    bool previous_encoded = false;
    while (!input.empty()) {
        const auto start = input.find("=?");
        const auto plain = input.substr(0, start);
        if (!previous_encoded || !trim(plain).empty() || start == std::string_view::npos) {
            auto converted = decoder(plain, fallback);
            if (!converted) {
                return std::unexpected(converted.error());
            }
            result.bytes += converted->bytes;
            result.replaced |= converted->replaced;
        }
        if (start == std::string_view::npos) {
            break;
        }
        input.remove_prefix(start + 2);
        const auto charset_end = input.find('?');
        if (charset_end == std::string_view::npos) {
            return failure(ErrorKind::input, "Malformed encoded word");
        }
        const auto charset = input.substr(0, charset_end);
        input.remove_prefix(charset_end + 1);
        if (input.size() < 2 || input.at(1) != '?') {
            return failure(ErrorKind::input, "Malformed encoded word");
        }
        const char method = ascii_lower(input.front());
        input.remove_prefix(2);
        const auto end = input.find("?=");
        if (end == std::string_view::npos) {
            return failure(ErrorKind::input, "Unterminated encoded word");
        }
        auto decoded = decode_word_payload(method, input.substr(0, end));
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        auto converted = decoder(*decoded, {charset});
        if (!converted) {
            return std::unexpected(converted.error());
        }
        result.bytes += converted->bytes;
        result.replaced |= converted->replaced;
        input.remove_prefix(end + 2);
        previous_encoded = true;
    }
    if (has_header_control(result.bytes)) {
        return failure(ErrorKind::input, "Decoded header contains control characters");
    }
    return result;
}
auto encode_header_words(std::string_view utf8, DestinationEncoding destination, bool no_encode)
    -> Result<std::string> {
    if (has_header_control(utf8)) {
        return failure(ErrorKind::input, "Invalid header control character");
    }
    const auto validated = transcode(utf8, {"UTF-8"}, destination);
    if (!validated) {
        return std::unexpected(validated.error());
    }
    if (no_encode || std::ranges::all_of(validated->bytes, [](char value) -> bool {
            return static_cast<unsigned char>(value) < ascii_count;
        })) {
        return validated->bytes;
    }
    if (destination.name.size() + encoded_word_overhead + quartet_size >= encoded_word_limit) {
        return failure(ErrorKind::input, "Charset label too long for encoded word");
    }
    const auto payload = (encoded_word_limit - destination.name.size() - encoded_word_overhead) /
                         quartet_size * triple_size;
    std::string output;
    while (!utf8.empty()) {
        auto count = std::min(payload, utf8.size());
        while (count < utf8.size() && count != 0 &&
               (static_cast<unsigned char>(utf8.at(count)) & continuation_mask) ==
                   continuation_bits) {
            --count;
        }
        if (count == 0) {
            return failure(ErrorKind::input, "No room for UTF-8 code point in encoded word");
        }
        auto chunk = transcode(utf8.substr(0, count), {"UTF-8"}, destination);
        if (!chunk) {
            return std::unexpected(chunk.error());
        }
        if (!output.empty()) {
            output += "\r\n ";
        }
        output += "=?" + std::string{destination.name} + "?B?" + encode_base64(chunk->bytes) + "?=";
        utf8.remove_prefix(count);
    }
    return output;
}
auto parse_raw_article(std::string_view input, SourceEncoding default_encoding)
    -> Result<RawMimeArticle> {
    auto block = split_headers(input);
    if (!block) {
        return std::unexpected(block.error());
    }
    const auto content_type = block->headers.find("content-type");
    auto charset = content_charset(content_type == block->headers.end() ? "" : content_type->second,
                                   default_encoding);
    if (!charset) {
        return std::unexpected(charset.error());
    }
    const auto transfer = block->headers.find("content-transfer-encoding");
    const auto method =
        transfer == block->headers.end() ? "8bit" : lower_ascii(trim(transfer->second));
    auto decoded = decode_transfer({method}, block->body);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    if (decoded->contains('\0')) {
        return failure(ErrorKind::input, "NUL in text article");
    }
    bool flowed = false;
    if (content_type != block->headers.end()) {
        auto type = std::string_view{content_type->second};
        while (type.contains(';')) {
            type.remove_prefix(type.find(';') + 1);
            const auto parameter = trim(type.substr(0, type.find(';')));
            const auto equals = parameter.find('=');
            if (equals != std::string_view::npos &&
                equal_ascii(trim(parameter.substr(0, equals)), "format")) {
                const auto value = trim(parameter.substr(equals + 1));
                flowed = equal_ascii(value, "flowed") || equal_ascii(value, "\"flowed\"");
            }
        }
    }
    return RawMimeArticle{
        .headers = std::move(block->headers),
        .body = std::move(*decoded),
        .source_charset = std::move(*charset),
        .flowed = flowed,
    };
}
auto parse_article(std::string_view input, SourceEncoding default_encoding, ConversionPolicy policy)
    -> Result<MimeArticle> {
    auto block = parse_raw_article(input, default_encoding);
    if (!block) {
        return std::unexpected{block.error()};
    }
    auto body = transcode(block->body, {block->source_charset}, {"UTF-8"}, policy);
    if (!body) {
        return std::unexpected(body.error());
    }
    if (body->bytes.contains('\0')) {
        return failure(ErrorKind::input, "NUL in text article");
    }
    MimeArticle article{
        .headers = {},
        .body = std::move(body->bytes),
        .source_charset = block->source_charset,
        .replaced = body->replaced,
    };
    for (const auto &[name, value] : block->headers) {
        auto header = decode_header(value, {block->source_charset}, policy);
        if (!header) {
            return std::unexpected(header.error());
        }
        article.replaced |= header->replaced;
        article.headers.emplace(name, std::move(header->bytes));
    }
    return article;
}
} // namespace jamnntpd
