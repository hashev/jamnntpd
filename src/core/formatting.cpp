#include "formatting.hpp"
#include "text.hpp"
#include <algorithm>
#include <cstdint>
#include <utility>

namespace jamnntpd {
namespace {
constexpr std::size_t wrap_width = 72;
constexpr std::size_t line_width = 79;
constexpr std::size_t maximum_wire_payload = 997;
constexpr std::size_t maximum_text_size = 1024UZ * 1024;
constexpr std::size_t smart_quote_limit = 200;
constexpr std::uint8_t continuation_mask = 0xc0;
constexpr std::uint8_t continuation_tag = 0x80;
constexpr std::uint8_t ascii_space = 32;
constexpr char soft_cr = static_cast<char>(0x8d);
struct QuoteInitials {
    std::string_view text;
};
auto next_character(std::string_view text, std::size_t position) -> std::size_t {
    ++position;
    while (position < text.size() &&
           (static_cast<std::uint8_t>(text.at(position)) & continuation_mask) == continuation_tag) {
        ++position;
    }
    return position;
}
auto validated_text(std::string_view text) -> Result<void> {
    if (text.size() > maximum_text_size || text.contains('\0')) {
        return failure(ErrorKind::input, "Invalid or excessive text body");
    }
    auto valid = transcode(text, {"UTF-8"}, {"UTF-8"});
    if (!valid) {
        return std::unexpected{valid.error()};
    }
    return {};
}
auto logical_lines(std::string_view text) -> std::vector<std::string> {
    std::vector<std::string> lines;
    while (!text.empty()) {
        const auto end = text.find_first_of("\r\n");
        lines.emplace_back(text.substr(0, end));
        if (end == std::string_view::npos) {
            break;
        }
        const auto consumed =
            end +
            (text.at(end) == '\r' && end + 1 < text.size() && text.at(end + 1) == '\n' ? 2UZ : 1UZ);
        text.remove_prefix(consumed);
    }
    return lines;
}
auto strip_end(std::string_view text) -> std::string_view {
    while (!text.empty() && static_cast<std::uint8_t>(text.back()) <= ascii_space) {
        text.remove_suffix(1);
    }
    return text;
}
auto quote_initials(std::string_view name) -> std::string {
    std::string initials;
    bool beginning = true;
    for (std::size_t position = 0; position < name.size();) {
        const auto next = next_character(name, position);
        if (name.at(position) == ' ') {
            beginning = true;
        } else if (beginning) {
            initials.append(name.substr(position, next - position));
            beginning = false;
        }
        position = next;
    }
    return initials;
}
auto smart_quote(std::string_view line, QuoteInitials initials) -> std::string {
    if (!line.starts_with('>') || line.size() >= smart_quote_limit) {
        return std::string{line};
    }
    std::string input{initials.text};
    input += '>';
    if (line.starts_with(">>")) {
        input += ' ';
    }
    input += line.substr(1);
    const auto view = strip_end(input);
    std::size_t last_quote = 0;
    std::size_t count = 0;
    for (std::size_t position = 0; position < view.size(); ++position) {
        const auto byte = view.at(position);
        if (byte == '>') {
            last_quote = position;
            ++count;
        } else if (byte == '<' ||
                   (byte != ' ' && position + 1 < view.size() && view.at(position + 1) == ' ')) {
            break;
        }
    }
    if (count == 0) {
        return input;
    }
    auto suffix = view.substr(last_quote + 1);
    if (suffix.empty()) {
        return {};
    }
    if (suffix.starts_with(' ')) {
        suffix.remove_prefix(1);
    }
    auto end = last_quote;
    while (end > 0 && view.at(end) == '>') {
        --end;
    }
    auto begin = end;
    if (view.at(end) != '>') {
        while (begin > 0 && view.at(begin) != ' ' && view.at(begin) != '>') {
            --begin;
        }
        if (view.at(begin) == ' ' || view.at(begin) == '>') {
            ++begin;
        }
        return " " + std::string{view.substr(begin, end + 1 - begin)} + std::string(count, '>') +
               " " + std::string{suffix};
    }
    return " " + std::string(count, '>') + " " + std::string{suffix};
}
struct WrappedLine {
    std::string_view text;
    std::size_t consumed{};
    bool wrapped{};
};
auto wrap_line(std::string_view text) -> WrappedLine {
    std::size_t position = 0;
    std::size_t characters = 0;
    std::size_t last_space = 0;
    while (position < text.size() && characters <= wrap_width) {
        if (text.at(position) == ' ') {
            last_space = position;
        }
        position = next_character(text, position);
        ++characters;
    }
    auto lookahead = position;
    while (lookahead < text.size() && characters < line_width) {
        lookahead = next_character(text, lookahead);
        ++characters;
    }
    if (lookahead == text.size()) {
        return {.text = text, .consumed = text.size(), .wrapped = false};
    }
    if (last_space != 0) {
        return {.text = text.substr(0, last_space), .consumed = last_space + 1, .wrapped = true};
    }
    while (position < text.size() && text.at(position) != ' ') {
        position = next_character(text, position);
    }
    const bool space = position < text.size();
    return {
        .text = text.substr(0, position),
        .consumed = position + (space ? 1 : 0),
        .wrapped = space,
    };
}
auto prepare_segment(const WrappedLine &wrapped, bool flowed) -> std::string {
    std::string segment{wrapped.text};
    if (flowed && segment != "-- ") {
        if (wrapped.wrapped) {
            segment += ' ';
        } else {
            segment = strip_end(segment);
        }
        if (segment.starts_with(' ') || segment.starts_with("From ")) {
            segment.insert(0, 1, ' ');
        }
    }
    return segment;
}
auto emit_line(Conversion &output, std::string_view line, DestinationEncoding destination,
               ConversionPolicy policy) -> Result<void> {
    auto encoded = transcode(line, {"UTF-8"}, destination, policy);
    if (!encoded) {
        return std::unexpected{encoded.error()};
    }
    if (encoded->bytes.contains('\r') || encoded->bytes.contains('\n') ||
        encoded->bytes.contains('\0')) {
        return failure(ErrorKind::encoding, "Conversion injected text framing control");
    }
    if (encoded->bytes.starts_with('.')) {
        output.bytes += '.';
    }
    output.bytes += encoded->bytes;
    output.bytes += "\r\n";
    output.replaced = output.replaced || encoded->replaced;
    return {};
}
auto emit_bounded(Conversion &output, std::string_view line, DestinationEncoding destination,
                  ConversionPolicy policy) -> Result<void> {
    while (!line.empty()) {
        auto encoded = transcode(line, {"UTF-8"}, destination, policy);
        if (!encoded) {
            return std::unexpected{encoded.error()};
        }
        if (encoded->bytes.size() <= maximum_wire_payload) {
            return emit_line(output, line, destination, policy);
        }
        std::size_t prefix = 0;
        std::size_t size = 0;
        while (prefix < line.size()) {
            const auto next = next_character(line, prefix);
            auto character =
                transcode(line.substr(prefix, next - prefix), {"UTF-8"}, destination, policy);
            if (!character) {
                return std::unexpected{character.error()};
            }
            if (size + character->bytes.size() > maximum_wire_payload) {
                break;
            }
            size += character->bytes.size();
            prefix = next;
        }
        if (prefix == 0) {
            return failure(ErrorKind::encoding, "Encoded character exceeds line limit");
        }
        if (auto result = emit_line(output, line.substr(0, prefix), destination, policy); !result) {
            return result;
        }
        line.remove_prefix(prefix);
    }
    return {};
}
} // namespace

auto remove_soft_cr(std::string_view bytes, SourceEncoding source, bool keep) -> std::string {
    const auto name = lower_ascii(source.name);
    const bool unicode_or_dos = name == "utf-8" || name == "utf8" || name == "cp866" ||
                                name == "cp437" || name == "cp850" || name == "cp865" ||
                                name == "ibmpc" || name.starts_with("ibmpc,");
    std::string result{bytes};
    if (!keep && !unicode_or_dos) {
        std::erase(result, soft_cr);
    }
    return result;
}
auto format_post_text(std::string_view utf8, const PostTextOptions &options)
    -> Result<std::string> {
    if (auto valid = validated_text(utf8); !valid) {
        return std::unexpected{valid.error()};
    }
    if (auto valid = validated_text(options.quote_name); !valid) {
        return std::unexpected{valid.error()};
    }
    auto lines = logical_lines(utf8);
    const auto initials = quote_initials(options.quote_name);
    std::string output;
    for (auto &line : lines) {
        if (options.replace_nbsp) {
            for (auto position = line.find("\xc2\xa0"); position != std::string::npos;
                 position = line.find("\xc2\xa0", position + 1)) {
                line.replace(position, 2, " ");
            }
        }
        if (options.flowed && options.delete_stuffed_quote_space && line.starts_with(">  ") &&
            line.size() > 3) {
            line.erase(1, 1);
        }
        const bool join =
            options.flowed && !line.empty() && line.front() != '>' && !line.starts_with("-- ");
        const bool soft = join && line.back() == ' ';
        if (join && line.starts_with(' ')) {
            line.erase(0, 1);
        }
        if (!line.starts_with("-- ")) {
            line = strip_end(line);
        }
        if (!initials.empty()) {
            line = smart_quote(line, {initials});
        }
        output += line;
        output += soft ? ' ' : '\r';
    }
    return output;
}
auto append_tearline(std::string_view utf8, const TearlineOptions &options) -> Result<std::string> {
    for (const auto value : {
             utf8,
             std::string_view{options.newsreader},
             std::string_view{options.origin},
             std::string_view{options.address},
         }) {
        if (auto valid = validated_text(value); !valid) {
            return std::unexpected{valid.error()};
        }
    }
    for (const auto *value : {&options.newsreader, &options.origin, &options.address}) {
        if (value->find_first_of("\r\n") != std::string::npos) {
            return failure(ErrorKind::input, "Multiline tearline metadata");
        }
    }
    std::string output{utf8};
    if (options.add_cr) {
        output += '\r';
    }
    output += "---";
    if (!options.no_tearline && !options.newsreader.empty()) {
        output += " " + options.newsreader;
    }
    output += "\r * Origin: " + options.origin + " (" + options.address + ")\r";
    return output;
}
auto format_read_text(std::string_view utf8, DestinationEncoding destination, bool flowed,
                      ConversionPolicy policy) -> Result<Conversion> {
    if (auto valid = validated_text(utf8); !valid) {
        return std::unexpected{valid.error()};
    }
    Conversion output;
    for (const auto &line : logical_lines(utf8)) {
        std::string_view remaining{line};
        if (remaining.empty()) {
            if (auto result = emit_line(output, {}, destination, policy); !result) {
                return std::unexpected{result.error()};
            }
        }
        while (!remaining.empty()) {
            const auto wrapped = wrap_line(remaining);
            const auto segment = prepare_segment(wrapped, flowed);
            const auto result = segment.empty()
                                    ? emit_line(output, {}, destination, policy)
                                    : emit_bounded(output, segment, destination, policy);
            if (!result) {
                return std::unexpected{result.error()};
            }
            remaining.remove_prefix(wrapped.consumed);
        }
    }
    return output;
}
auto encode_post_text(std::string_view utf8, DestinationEncoding destination,
                      std::size_t maximum_bytes, ConversionPolicy policy) -> Result<Conversion> {
    auto converted = transcode(utf8, {"UTF-8"}, destination, policy);
    if (!converted) {
        return std::unexpected{converted.error()};
    }
    if (converted->bytes.size() > maximum_bytes || converted->bytes.contains('\0')) {
        return failure(ErrorKind::input, "Encoded text exceeds limit or contains NUL");
    }
    return converted;
}
} // namespace jamnntpd
