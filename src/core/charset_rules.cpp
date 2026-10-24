#include "charset_rules.hpp"
#include "text.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <system_error>
#include <utility>

namespace jamnntpd {
namespace {
constexpr std::size_t maximum_rule_line = 65536;
constexpr std::size_t maximum_rule_file = 16UZ * 1024 * 1024;
auto split_charset(std::string_view text) -> Charset {
    const auto comma = text.find(',');
    return {
        .chrs = std::string{text.substr(0, comma)},
        .codepage = comma == std::string_view::npos ? "" : std::string{text.substr(comma + 1)},
    };
}
auto charset_label(const Charset &charset) -> std::string {
    return charset.chrs + (charset.codepage.empty() ? "" : "," + charset.codepage);
}
auto matches_charset(std::string_view pattern, const Charset &charset) -> bool {
    const auto comma = pattern.find(',');
    return match_pattern(pattern.substr(0, comma), charset.chrs) &&
           (comma == std::string_view::npos ||
            match_pattern(pattern.substr(comma + 1), charset.codepage));
}
auto standard_encoding(const Charset &charset) -> Result<std::string> {
    auto name = lower_ascii(charset.chrs);
    if (name == "ibmpc") {
        name = "cp" + (charset.codepage.empty() ? "437" : charset.codepage);
    }
    if (name == "latin-1") {
        name = "iso-8859-1";
    }
    if (name == "utf8") {
        name = "utf-8";
    }
    constexpr std::array names{
        "utf-8",  "cp866",        "cp437",        "cp850",      "cp865",
        "koi8-r", "windows-1251", "windows-1252", "iso-8859-1", "iso-8859-15",
    };
    if (std::ranges::find(names, name) == names.end()) {
        return failure(ErrorKind::encoding,
                       "Unsupported standard charset: " + charset_label(charset));
    }
    return name;
}
auto forced_default(std::string_view group, std::string_view global) -> std::string_view {
    if (group.starts_with('!')) {
        return group.substr(1);
    }
    if (global.starts_with('!')) {
        return global.substr(1);
    }
    return {};
}
auto selected_plan(const Charset &source, const CharsetRule &rule) -> ConversionPlan {
    return {
        .source = source,
        .destination = split_charset(rule.destination),
        .method = rule.method,
        .table = rule.table,
        .keep_soft_cr = rule.keep_soft_cr,
    };
}
auto automatic_plan(const Charset &source, const Charset &destination,
                    const std::vector<CharsetRule> &opposite_rules) -> Result<ConversionPlan> {
    auto from = standard_encoding(source);
    if (!from) {
        return std::unexpected{from.error()};
    }
    auto target = standard_encoding(destination);
    if (!target) {
        return std::unexpected{target.error()};
    }
    // A standard codec is not the inverse of a user-supplied byte mapping.
    for (const auto &rule : opposite_rules) {
        const auto reverse_source = standard_encoding(split_charset(rule.source_pattern));
        const auto reverse_destination = standard_encoding(split_charset(rule.destination));
        if (rule.method == ConversionMethod::table &&
            (matches_charset(rule.source_pattern, destination) ||
             (reverse_source && *reverse_source == *target)) &&
            (equal_ascii(rule.destination, charset_label(source)) ||
             (reverse_destination && *reverse_destination == *from))) {
            return failure(ErrorKind::encoding,
                           "Custom conversion requires an explicit reverse rule");
        }
    }
    return ConversionPlan{
        .source = source,
        .destination = destination,
        .method = ConversionMethod::builtin,
        .table = std::nullopt,
        .keep_soft_cr = false,
    };
}
auto parse_rule(const std::vector<std::string> &words, const std::filesystem::path &directory)
    -> Result<CharsetRule> {
    CharsetRule rule{
        .source_pattern = words.at(1),
        .destination = words.at(2),
        .table = std::nullopt,
    };
    std::optional<std::filesystem::path> table_file;
    bool builtin = false;
    for (std::size_t index = 3; index < words.size(); ++index) {
        const auto &option = words.at(index);
        if (equal_ascii(option, "-keepsoftcr")) {
            rule.keep_soft_cr = true;
        } else if (equal_ascii(option, "-builtin")) {
            if (builtin) {
                return failure(ErrorKind::input, "Duplicate -builtin");
            }
            builtin = true;
        } else if (option.starts_with('-') || table_file || option.empty()) {
            return failure(ErrorKind::input, "Invalid charset rule option: " + option);
        } else {
            table_file = directory / option;
        }
    }
    if (builtin && table_file) {
        return failure(ErrorKind::input, "-builtin cannot be combined with a CHS file");
    }
    if (builtin) {
        if (auto encoding = standard_encoding(split_charset(rule.destination)); !encoding) {
            return std::unexpected{encoding.error()};
        }
        rule.method = ConversionMethod::builtin;
    } else if (table_file) {
        auto table = load_chs(*table_file);
        if (!table) {
            return std::unexpected{table.error()};
        }
        rule.method = ConversionMethod::table;
        rule.table = std::move(*table);
    }
    return rule;
}
auto parse_directive(CharsetConfig &config, const std::vector<std::string> &words,
                     std::filesystem::path &directory) -> Result<void> {
    const auto directive = lower_ascii(words.front());
    if (directive == "read" || directive == "post") {
        if (words.size() < 3 || words.at(1).empty()) {
            return failure(ErrorKind::input, "Expected source pattern and destination");
        }
        auto rule = parse_rule(words, directory);
        if (!rule) {
            return std::unexpected{rule.error()};
        }
        (directive == "read" ? config.read_rules : config.post_rules).push_back(std::move(*rule));
    } else if (directive == "readalias" || directive == "postalias") {
        if (words.size() != 3 || words.at(1).empty()) {
            return failure(ErrorKind::input, "Expected alias pattern and replacement");
        }
        (directive == "readalias" ? config.read_aliases : config.post_aliases)
            .push_back({.pattern = words.at(1), .replacement = words.at(2)});
    } else if (words.size() != 2) {
        return failure(ErrorKind::input, "Expected one charset directive value");
    } else if (directive == "chsdir") {
        directory = words.at(1);
    } else if (directive == "defaultread") {
        config.default_read = words.at(1);
    } else if (directive == "defaultpost") {
        config.default_post = words.at(1);
    } else {
        return failure(ErrorKind::input, "Unknown charset directive: " + directive);
    }
    return {};
}
} // namespace

auto ConversionPlan::apply(std::string_view bytes, ConversionPolicy policy) const
    -> Result<Conversion> {
    if (method == ConversionMethod::pass_through) {
        return Conversion{.bytes = std::string{bytes}, .replaced = false};
    }
    if (method == ConversionMethod::table) {
        if (!table) {
            return failure(ErrorKind::input, "Missing CHS table in conversion plan");
        }
        return Conversion{.bytes = table->apply(bytes), .replaced = false};
    }
    auto from = standard_encoding(source);
    if (!from) {
        return std::unexpected{from.error()};
    }
    auto target = standard_encoding(destination);
    if (!target) {
        return std::unexpected{target.error()};
    }
    return transcode(bytes, {.name = *from}, {.name = *target}, policy);
}

auto parse_charset_config(std::string_view text, const std::filesystem::path &filename)
    -> Result<CharsetConfig> {
    if (text.size() > maximum_rule_file) {
        return failure(ErrorKind::input, filename.string() + ": charset configuration too large");
    }
    CharsetConfig config;
    std::filesystem::path directory;
    std::size_t line_number = 0;
    while (!text.empty()) {
        ++line_number;
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const auto context = filename.string() + ":" + std::to_string(line_number) + ": ";
        if (line.size() > maximum_rule_line || line.contains('\0')) {
            return failure(ErrorKind::input, context + "Invalid or excessive charset rule line");
        }
        auto words = config_words(line);
        if (!words) {
            return failure(ErrorKind::input, context + words.error().message);
        }
        if (words->empty()) {
            continue;
        }
        if (auto result = parse_directive(config, *words, directory); !result) {
            return failure(result.error().kind, context + result.error().message);
        }
    }
    return config;
}
auto load_charset_config(const std::filesystem::path &filename) -> Result<CharsetConfig> {
    std::error_code error;
    if (!std::filesystem::is_regular_file(filename, error)) {
        return failure(ErrorKind::io,
                       "Not a readable charset configuration file: " + filename.string());
    }
    std::ifstream input{filename, std::ios::binary};
    if (!input) {
        return failure(ErrorKind::io, "Cannot open charset configuration: " + filename.string());
    }
    std::string text;
    char byte{};
    while (input.get(byte)) {
        if (text.size() == maximum_rule_file) {
            return failure(ErrorKind::input,
                           filename.string() + ": charset configuration too large");
        }
        text.push_back(byte);
    }
    if (!input.eof()) {
        return failure(ErrorKind::io, "Cannot read charset configuration: " + filename.string());
    }
    return parse_charset_config(text, filename);
}
auto resolve_read(const CharsetConfig &config, const ReadCharsetRequest &request)
    -> Result<ConversionPlan> {
    auto source = request.metadata;
    const auto forced = forced_default(request.group_default, config.default_read);
    if (!forced.empty()) {
        source = split_charset(forced);
    } else if (source.chrs.empty()) {
        source = split_charset(request.group_default.empty() ? config.default_read
                                                             : request.group_default);
    }
    for (const auto &alias : config.read_aliases) {
        if (matches_charset(alias.pattern, source)) {
            source = split_charset(alias.replacement);
            break;
        }
    }
    for (const auto &rule : config.read_rules) {
        if (matches_charset(rule.source_pattern, source) &&
            (!request.destination_pattern ||
             match_pattern(*request.destination_pattern, rule.destination))) {
            return selected_plan(source, rule);
        }
    }
    return automatic_plan(source, split_charset(request.destination_pattern.value_or("UTF-8")),
                          config.post_rules);
}
auto resolve_post(const CharsetConfig &config, const PostCharsetRequest &request)
    -> Result<ConversionPlan> {
    std::string source{request.source.empty() ? config.default_post : request.source};
    for (const auto &alias : config.post_aliases) {
        if (match_pattern(alias.pattern, source)) {
            source = alias.replacement;
            break;
        }
    }
    const auto forced = forced_default(request.group_default, config.default_read);
    for (const auto &rule : config.post_rules) {
        if (match_pattern(rule.source_pattern, source) &&
            (forced.empty() || match_pattern(forced, rule.destination))) {
            return selected_plan(split_charset(source), rule);
        }
    }
    auto destination = forced;
    if (destination.empty()) {
        destination = request.group_default.empty() ? std::string_view{config.default_read}
                                                    : request.group_default;
    }
    if (destination.empty()) {
        destination = source;
    }
    return automatic_plan(split_charset(source), split_charset(destination), config.read_rules);
}
} // namespace jamnntpd
