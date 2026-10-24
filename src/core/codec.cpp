#include "codec.hpp"
#include "text.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <unicode/ucnv.h>
#include <unicode/ucnv_cb.h>
#include <vector>

namespace jamnntpd {
namespace {
struct CallbackState {
    mutable bool replaced{};
};
constexpr auto byte_maximum = std::numeric_limits<unsigned char>::max();
constexpr std::size_t classic_chs_count = 128;
constexpr std::uint32_t classic_chs_maximum_id = 65535;
constexpr std::size_t maximum_expansion = 4;
constexpr std::size_t kibibyte = 1024;

struct CloseConverter {
    void operator()(UConverter *converter) const noexcept { ucnv_close(converter); }
};
using Converter = std::unique_ptr<UConverter, CloseConverter>;

enum class CodepageProfile : std::uint8_t {
    none,
    dos_controls,
    dos_micro,
    windows1251,
    windows1252,
};
enum class ProfileDirection : std::uint8_t { decode, encode };
constexpr std::int32_t cp437_ccsid = 437;
constexpr std::int32_t cp850_ccsid = 850;
constexpr std::int32_t cp865_ccsid = 865;
constexpr std::int32_t cp866_ccsid = 866;
constexpr std::int32_t cp1251_ccsid = 1251;
constexpr std::int32_t cp1252_ccsid = 1252;
constexpr std::int32_t cp5347_ccsid = 5347;
constexpr std::int32_t cp5348_ccsid = 5348;
auto codepage_profile(const Converter &converter) -> CodepageProfile {
    UErrorCode status = U_ZERO_ERROR;
    const auto identifier = ucnv_getCCSID(converter.get(), &status);
    if (U_FAILURE(status) != 0) {
        return CodepageProfile::none;
    }
    switch (identifier) {
    case cp437_ccsid:
    case cp865_ccsid:
        return CodepageProfile::dos_micro;
    case cp850_ccsid:
    case cp866_ccsid:
        return CodepageProfile::dos_controls;
    case cp1251_ccsid:
    case cp5347_ccsid:
        return CodepageProfile::windows1251;
    case cp1252_ccsid:
    case cp5348_ccsid:
        return CodepageProfile::windows1252;
    default:
        return CodepageProfile::none;
    }
}
auto undefined_windows_code_point(UChar value, CodepageProfile profile) -> bool {
    constexpr std::array windows1252_undefined{
        u'\u0081', u'\u008d', u'\u008f', u'\u0090', u'\u009d',
    };
    if (profile == CodepageProfile::windows1251) {
        return value == u'\u0098';
    }
    return profile == CodepageProfile::windows1252 &&
           std::ranges::find(windows1252_undefined, value) != windows1252_undefined.end();
}
auto adjusted_code_point(UChar value, CodepageProfile profile, ProfileDirection direction)
    -> std::optional<UChar> {
    if (undefined_windows_code_point(value, profile)) {
        return std::nullopt;
    }
    if (profile != CodepageProfile::dos_controls && profile != CodepageProfile::dos_micro) {
        return value;
    }
    // ICU's IBM tables use a different control convention from the published
    // DOS mappings retained in this project's Unicode CHS files.
    if (direction == ProfileDirection::decode) {
        switch (value) {
        case u'\x1c':
            return u'\x1a';
        case u'\x7f':
            return u'\x1c';
        case u'\x1a':
            return u'\x7f';
        default:
            break;
        }
        if (profile == CodepageProfile::dos_micro && value == u'\u03bc') {
            return u'\u00b5';
        }
        return value;
    }
    switch (value) {
    case u'\x1a':
        return u'\x1c';
    case u'\x1c':
        return u'\x7f';
    case u'\x7f':
        return u'\x1a';
    default:
        break;
    }
    if (profile == CodepageProfile::dos_micro && value == u'\u03bc') {
        return std::nullopt;
    }
    if (profile == CodepageProfile::dos_micro && value == u'\u00b5') {
        return u'\u03bc';
    }
    return value;
}
auto adapt_codepage(std::span<UChar> text, CodepageProfile profile, ProfileDirection direction,
                    ConversionPolicy policy, CallbackState &state) -> Result<void> {
    for (auto &code_point : text) {
        const auto adapted = adjusted_code_point(code_point, profile, direction);
        if (adapted) {
            code_point = *adapted;
            continue;
        }
        if (policy == ConversionPolicy::strict) {
            return failure(ErrorKind::encoding,
                           "Undefined byte or unrepresentable code point in selected encoding");
        }
        code_point = direction == ProfileDirection::decode ? u'\ufffd' : u'?';
        state.replaced = true;
    }
    return {};
}
auto converter_name(std::string_view name) -> std::string {
    if (equal_ascii(name, "LATIN-1")) {
        return "ISO-8859-1";
    }
    if (equal_ascii(name, "IBMPC")) {
        return "CP437";
    }
    return std::string{name};
}
auto open_converter(std::string_view name) -> Result<Converter> {
    if (name.empty() || name.contains('\0')) {
        return failure(ErrorKind::encoding, "Missing or invalid encoding name");
    }
    UErrorCode status = U_ZERO_ERROR;
    Converter converter{ucnv_open(converter_name(name).c_str(), &status)};
    if (U_FAILURE(status) != 0) {
        return failure(ErrorKind::encoding, "Unsupported encoding: " + std::string{name}, status);
    }
    ucnv_setFallback(converter.get(), 0);
    return converter;
}
void U_CALLCONV to_unicode_callback(const void *context, UConverterToUnicodeArgs *arguments,
                                    const char *code_units, std::int32_t length,
                                    UConverterCallbackReason reason, UErrorCode *status) {
    if (reason <= UCNV_IRREGULAR) {
        static_cast<const CallbackState *>(context)->replaced = true;
    }
    UCNV_TO_U_CALLBACK_SUBSTITUTE(nullptr, arguments, code_units, length, reason, status);
}
void U_CALLCONV from_unicode_callback(const void *context, UConverterFromUnicodeArgs *arguments,
                                      const UChar *code_units, std::int32_t length,
                                      UChar32 code_point, UConverterCallbackReason reason,
                                      UErrorCode *status) {
    if (reason <= UCNV_IRREGULAR) {
        static_cast<const CallbackState *>(context)->replaced = true;
    }
    UCNV_FROM_U_CALLBACK_SUBSTITUTE(nullptr, arguments, code_units, length, code_point, reason,
                                    status);
}
auto chs_byte(std::string_view token) -> Result<unsigned int> {
    if (token == "\\\\") {
        return static_cast<unsigned int>('\\');
    }
    if (token == "\\0") {
        return 0U;
    }
    if (token.starts_with("\\x") || token.starts_with("\\d")) {
        const auto base = token.at(1) == 'x' ? NumberBase::hexadecimal : NumberBase::decimal;
        auto number = parse_number(token.substr(2), byte_maximum, base);
        if (!number) {
            return std::unexpected(number.error());
        }
        return *number;
    }
    if (token.size() == 1 && token.front() != '\\') {
        return static_cast<unsigned char>(token.front());
    }
    return failure(ErrorKind::input, "Invalid CHS byte escape");
}
auto parse_chs_row(std::string_view line) -> Result<std::string> {
    std::string entry;
    std::size_t token_count{};
    while (!line.empty()) {
        const auto end = line.find_first_of(" \t\r\n\v\f");
        const auto token = line.substr(0, end);
        if (++token_count > maximum_expansion) {
            return failure(ErrorKind::input, "Invalid CHS mapping row");
        }
        auto byte = chs_byte(token);
        if (!byte) {
            return std::unexpected(byte.error());
        }
        if (*byte != 0) {
            entry.push_back(static_cast<char>(*byte));
        }
        if (end == std::string_view::npos) {
            break;
        }
        line = trim(line.substr(end));
    }
    return entry;
}
auto valid_chs_trailer(std::string_view input, std::size_t &line_number) -> bool {
    while (!input.empty()) {
        ++line_number;
        const auto end = input.find('\n');
        const auto line = trim(input.substr(0, end).substr(0, input.substr(0, end).find(';')));
        if (!line.empty() && line != "END") {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        input.remove_prefix(end + 1);
    }
    return true;
}
} // namespace

auto transcode(std::string_view input, SourceEncoding source, DestinationEncoding destination,
               ConversionPolicy policy) -> Result<Conversion> {
    CallbackState callback_state;
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return failure(ErrorKind::encoding, "Input too large for encoding conversion");
    }
    auto from = open_converter(source.name);
    if (!from) {
        return std::unexpected(from.error());
    }
    auto destination_converter = open_converter(destination.name);
    if (!destination_converter) {
        return std::unexpected(destination_converter.error());
    }
    // Context outlives converters, including their close callbacks.
    UErrorCode status = U_ZERO_ERROR;
    ucnv_setToUCallBack(from->get(),
                        policy == ConversionPolicy::strict ? UCNV_TO_U_CALLBACK_STOP
                                                           : to_unicode_callback,
                        &callback_state, nullptr, nullptr, &status);
    ucnv_setFromUCallBack(destination_converter->get(),
                          policy == ConversionPolicy::strict ? UCNV_FROM_U_CALLBACK_STOP
                                                             : from_unicode_callback,
                          &callback_state, nullptr, nullptr, &status);
    ucnv_setSubstChars(destination_converter->get(), "?", 1, &status);
    if (U_FAILURE(status) != 0) {
        return failure(ErrorKind::encoding, "Cannot configure encoding conversion", status);
    }
    const auto input_length = static_cast<std::int32_t>(input.size());
    // Keep a valid pointer even for a default-constructed empty string_view.
    const std::string owned_input{input};
    auto length = ucnv_toUChars(from->get(), nullptr, 0, owned_input.data(), input_length, &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && (U_FAILURE(status) != 0)) {
        return failure(ErrorKind::encoding, "Malformed source text", status);
    }
    status = U_ZERO_ERROR;
    if (length == std::numeric_limits<std::int32_t>::max()) {
        return failure(ErrorKind::encoding, "Decoded text exceeds conversion limit");
    }
    std::vector<UChar> unicode(static_cast<std::size_t>(length) + 1U);
    length = ucnv_toUChars(from->get(), unicode.data(), static_cast<std::int32_t>(unicode.size()),
                           owned_input.data(), input_length, &status);
    if (U_FAILURE(status) != 0) {
        return failure(ErrorKind::encoding, "Malformed source text", status);
    }
    auto code_points = std::span{unicode}.first(static_cast<std::size_t>(length));
    const auto source_profile = adapt_codepage(code_points, codepage_profile(*from),
                                               ProfileDirection::decode, policy, callback_state);
    if (!source_profile) {
        return std::unexpected(source_profile.error());
    }
    const auto destination_profile =
        adapt_codepage(code_points, codepage_profile(*destination_converter),
                       ProfileDirection::encode, policy, callback_state);
    if (!destination_profile) {
        return std::unexpected(destination_profile.error());
    }
    const auto output_length =
        ucnv_fromUChars(destination_converter->get(), nullptr, 0, unicode.data(), length, &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && (U_FAILURE(status) != 0)) {
        return failure(ErrorKind::encoding, "Character not representable in destination encoding",
                       status);
    }
    status = U_ZERO_ERROR;
    std::string output(static_cast<std::size_t>(output_length), '\0');
    ucnv_fromUChars(destination_converter->get(), output.data(), output_length, unicode.data(),
                    length, &status);
    if (U_FAILURE(status) != 0) {
        return failure(ErrorKind::encoding, "Destination encoding conversion failed", status);
    }
    return Conversion{.bytes = std::move(output), .replaced = callback_state.replaced};
}

auto ChsTable::apply(std::string_view input) const -> std::string {
    std::string result;
    for (char value : input) {
        result += entries.at(static_cast<unsigned char>(value));
    }
    return result;
}

auto parse_chs(std::string_view input, const std::filesystem::path &filename) -> Result<ChsTable> {
    std::size_t line_number = 0;
    auto next_line = [&] -> Result<std::string_view> {
        while (!input.empty()) {
            ++line_number;
            const auto end = input.find('\n');
            auto line = input.substr(0, end);
            if (end == std::string_view::npos) {
                input = {};
            } else {
                input.remove_prefix(end + 1);
            }
            line = trim(line);
            if (line.starts_with(';')) {
                continue;
            }
            return trim(line.substr(0, line.find(';')));
        }
        return failure(ErrorKind::input, "Unexpected end of CHS file");
    };
    auto diagnostic = [&](const std::string &message) -> std::unexpected<Error> {
        return failure(ErrorKind::input,
                       filename.string() + ":" + std::to_string(line_number) + ": " + message);
    };
    auto identifier = next_line();
    auto version = next_line();
    auto level = next_line();
    auto source = next_line();
    auto destination = next_line();
    if (!identifier || !version || !level || !source || !destination) {
        return diagnostic("Truncated CHS header");
    }
    auto id_value = parse_number(*identifier, std::numeric_limits<std::uint32_t>::max());
    auto version_value = parse_number(*version, std::numeric_limits<std::uint32_t>::max());
    auto level_value = parse_number(*level, 2);
    if (!id_value || !version_value || !level_value || *level_value == 0 || source->empty() ||
        destination->empty()) {
        return diagnostic("Invalid CHS header");
    }
    ChsTable table{
        .source = std::string{*source},
        .destination = std::string{*destination},
    };
    for (std::size_t index = 1; index < table.entries.size(); ++index) {
        table.entries.at(index) = std::string(1, static_cast<char>(index));
    }
    const std::size_t count = *id_value > classic_chs_maximum_id ? chs_entries : classic_chs_count;
    const std::size_t first = count == chs_entries || *level_value == 1 ? 0 : classic_chs_count;
    for (std::size_t index = 0; index < count; ++index) {
        auto line = next_line();
        if (!line) {
            return diagnostic("Truncated CHS mapping");
        }
        auto entry = parse_chs_row(*line);
        if (!entry) {
            return diagnostic(entry.error().message);
        }
        table.entries.at(first + index) = std::move(*entry);
    }
    if (!valid_chs_trailer(input, line_number)) {
        return diagnostic("Unexpected data after CHS mapping");
    }
    return table;
}

auto load_chs(const std::filesystem::path &filename) -> Result<ChsTable> {
    std::ifstream input{filename, std::ios::binary};
    if (!input) {
        return failure(ErrorKind::io, "Cannot open CHS file: " + filename.string());
    }
    constexpr std::size_t maximum = kibibyte * kibibyte;
    std::string text;
    for (char value{}; input.get(value);) {
        if (text.size() == maximum) {
            return failure(ErrorKind::input, "CHS file too large: " + filename.string());
        }
        text.push_back(value);
    }
    if (!input.eof()) {
        return failure(ErrorKind::io, "Cannot read CHS file: " + filename.string());
    }
    return parse_chs(text, filename);
}
} // namespace jamnntpd
