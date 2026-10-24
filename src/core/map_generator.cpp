#include "map_generator.hpp"
#include "jam_format.hpp"
#include "makechs_fallbacks.hpp"
#include "text.hpp"
#include <algorithm>
#include <bit>
#include <fstream>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>

namespace jamnntpd {
namespace {
constexpr std::uint32_t maximum_code_point = 0x10ffff;
constexpr std::uint32_t first_surrogate = 0xd800;
constexpr std::uint32_t last_surrogate = 0xdfff;
constexpr std::size_t ascii_count = 128;
constexpr unsigned char ascii_delete = '\x7f';
constexpr std::size_t maximum_expansion = 4;
constexpr std::uint32_t extended_identifier = 100000;
constexpr std::size_t maximum_map_line = 4096;
auto map_hex(std::string_view word) -> Result<std::uint32_t> {
    if (!word.starts_with("0x") && !word.starts_with("0X")) {
        return failure(ErrorKind::input, "Expected hexadecimal map value");
    }
    return parse_number(word.substr(2), maximum_code_point, NumberBase::hexadecimal);
}
auto utf8_scalar(char32_t value) -> Result<std::string> {
    const auto view = std::as_bytes(std::span{&value, 1});
    const auto *const source_encoding =
        std::endian::native == std::endian::little ? "UTF-32LE" : "UTF-32BE";
    auto converted = transcode(jam::text_from(view), {source_encoding}, {"UTF-8"});
    if (!converted) {
        return std::unexpected(converted.error());
    }
    return std::move(converted->bytes);
}
void emit_byte(std::ostream &output, unsigned char value) {
    if (value == 0) {
        output << "\\0";
    } else if (value <= ' ' || value >= ascii_delete || value == '\\' || value == ';') {
        output << "\\x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<unsigned int>(value) << std::dec;
    } else {
        output << static_cast<char>(value);
    }
}
auto emit_row(std::ostream &output, std::string_view mapping) -> Result<void> {
    if (mapping.size() > maximum_expansion) {
        return failure(ErrorKind::input, "Generated CHS expansion exceeds four bytes");
    }
    if (mapping.size() <= 1) {
        emit_byte(output, 0);
        if (!mapping.empty()) {
            output << '\t';
        }
    }
    bool first = true;
    for (char byte : mapping) {
        if (!first) {
            output << '\t';
        }
        emit_byte(output, static_cast<unsigned char>(byte));
        first = false;
    }
    output << '\n';
    return {};
}
auto legacy_mapping(char32_t value, const UnicodeMap &destination, ChsGeneration &result)
    -> std::string {
    const auto target =
        std::ranges::distance(destination.begin(), std::ranges::find(destination, value));
    if (target != static_cast<std::ptrdiff_t>(destination.size())) {
        std::string mapped(1, static_cast<char>(target));
        return mapped;
    }
    const auto fallback = static_cast<std::size_t>(std::ranges::distance(
        mapping_fallbacks.begin(),
        std::ranges::find(mapping_fallbacks, value, &MappingFallback::code_point)));
    std::ostringstream message;
    message << "Unicode " << std::hex << static_cast<std::uint32_t>(value)
            << " absent from target map";
    if (fallback != mapping_fallbacks.size()) {
        result.warnings.push_back(message.str() + "; using explicit table fallback");
        return std::string{mapping_fallbacks.at(fallback).text};
    }
    result.warnings.push_back(message.str() + "; using '?' in generated table");
    return "?";
}
} // namespace

auto read_unicode_map(const std::filesystem::path &path) -> Result<UnicodeMap> {
    std::ifstream input{path};
    if (!input) {
        return failure(ErrorKind::io, "Cannot open Unicode map: " + path.string());
    }
    UnicodeMap map{};
    for (std::size_t index = 0; index < map.size(); ++index) {
        map.at(index) = static_cast<char32_t>(index);
    }
    std::string line;
    std::size_t line_number{};
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.front() == '\x1a') {
            continue;
        }
        auto words = config_words(trim(std::string_view{line}.substr(0, line.find('#'))));
        if (line.size() > maximum_map_line || !words || words->size() > 2) {
            return failure(ErrorKind::input,
                           path.string() + ":" + std::to_string(line_number) + ": invalid map row");
        }
        if (words->empty()) {
            continue;
        }
        const auto source = map_hex(words->front());
        const auto target = words->size() == 2 ? map_hex(words->at(1)) : Result<std::uint32_t>{0};
        if (!source || !target || *source >= chs_entries ||
            (*target >= first_surrogate && *target <= last_surrogate)) {
            return failure(ErrorKind::input, path.string() + ":" + std::to_string(line_number) +
                                                 ": invalid map value");
        }
        map.at(*source) = *target == 0 && *source != 0 ? '?' : static_cast<char32_t>(*target);
    }
    if (!input.eof()) {
        return failure(ErrorKind::io, "Cannot read Unicode map");
    }
    return map;
}
auto generate_chs(const UnicodeMap &source, const std::optional<UnicodeMap> &destination,
                  SourceEncoding source_name, DestinationEncoding destination_name)
    -> Result<ChsGeneration> {
    if (source_name.name.empty() || destination_name.name.empty() ||
        source_name.name.contains('\n') || destination_name.name.contains('\n') ||
        source_name.name.contains('\r') || destination_name.name.contains('\r')) {
        return failure(ErrorKind::input, "Invalid charset label");
    }
    bool extended = !destination;
    if (destination) {
        for (std::size_t index = 0; index < ascii_count; ++index) {
            if (source.at(index) != destination->at(index)) {
                extended = true;
                break;
            }
        }
    }
    ChsGeneration result;
    std::ostringstream output;
    output << "; Generated by makechs, based on Johan Billing's utility\n"
           << (extended ? extended_identifier : 0) << "\n0\n2\n"
           << source_name.name << '\n'
           << destination_name.name << '\n';
    for (std::size_t index = extended ? 0 : ascii_count; index < source.size(); ++index) {
        auto mapped =
            destination
                ? Result<std::string>{legacy_mapping(source.at(index), *destination, result)}
                : utf8_scalar(source.at(index));
        if (!mapped) {
            return std::unexpected(mapped.error());
        }
        auto emitted = emit_row(output, *mapped);
        if (!emitted) {
            return std::unexpected(emitted.error());
        }
    }
    output << "END\n";
    result.text = output.str();
    return result;
}
} // namespace jamnntpd
