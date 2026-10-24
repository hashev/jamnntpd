#include "text.hpp"
#include <algorithm>
#include <charconv>
#include <memory>
#include <system_error>

namespace jamnntpd {
auto ascii_lower(char value) noexcept -> char {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

auto lower_ascii(std::string_view text) -> std::string {
    std::string result{text};
    std::ranges::transform(result, result.begin(), ascii_lower);
    return result;
}

auto equal_ascii(std::string_view left, std::string_view right) noexcept -> bool {
    return std::ranges::equal(left, right, {}, ascii_lower, ascii_lower);
}

auto match_pattern(std::string_view pattern, std::string_view text) -> bool {
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        if (pattern.at(index) == '*') {
            return true; // Legacy '*' ignores the remaining pattern, including suffixes.
        }
        if (index >= text.size() || (pattern.at(index) != '?' && ascii_lower(pattern.at(index)) !=
                                                                     ascii_lower(text.at(index)))) {
            return false;
        }
    }
    return pattern.size() == text.size();
}

auto match_names(NamePatterns name_patterns, std::string_view text) -> bool {
    auto patterns = name_patterns.text;
    while (!patterns.empty()) {
        const auto comma = patterns.find(',');
        auto name = patterns;
        if (comma != std::string_view::npos) {
            name.remove_suffix(name.size() - comma);
        }
        if (!name.empty() && match_pattern(name, text)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        patterns.remove_prefix(comma + 1);
    }
    return false;
}

auto match_group(std::string_view groups, char group) noexcept -> bool {
    if (groups == "*") {
        return true;
    }
    if (groups == "-") {
        return false;
    }
    return std::ranges::any_of(
        groups, [group](char value) -> bool { return ascii_lower(value) == ascii_lower(group); });
}

auto trim(std::string_view text) noexcept -> std::string_view {
    constexpr std::string_view whitespace{" \t\r\n\v\f"};
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(whitespace);
    text.remove_suffix(text.size() - end - 1);
    text.remove_prefix(first);
    return text;
}

auto config_words(std::string_view line) -> Result<std::vector<std::string>> {
    std::vector<std::string> words;
    line = trim(line);
    if (line.starts_with('#')) {
        return words;
    }
    while (!line.empty()) {
        if (line.front() == '"') {
            line.remove_prefix(1);
            const auto end = line.find('"');
            if (end == std::string_view::npos) {
                return failure(ErrorKind::input, "Unterminated quoted field");
            }
            words.emplace_back(line.substr(0, end));
            line.remove_prefix(end + 1);
            if (!line.empty() && trim(line).size() == line.size()) {
                return failure(ErrorKind::input, "Expected whitespace after quoted field");
            }
        } else {
            const auto end = line.find_first_of(" \t\r\n\v\f");
            words.emplace_back(line.substr(0, end));
            if (end == std::string_view::npos) {
                break;
            }
            line.remove_prefix(end);
        }
        line = trim(line);
    }
    return words;
}

auto parse_number(std::string_view text, std::uint32_t maximum, NumberBase base)
    -> Result<std::uint32_t> {
    std::uint32_t value{};
    if (text.empty() || (base != NumberBase::decimal && base != NumberBase::hexadecimal)) {
        return failure(ErrorKind::input, "Invalid number");
    }
    const auto parsed = std::from_chars(std::to_address(text.begin()), std::to_address(text.end()),
                                        value, static_cast<int>(base));
    if (parsed.ec != std::errc{} || parsed.ptr != std::to_address(text.end()) || value > maximum) {
        return failure(ErrorKind::input, "Number outside permitted range");
    }
    return value;
}
} // namespace jamnntpd
