#include "config.hpp"
#include "text.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <utility>

namespace jamnntpd {
namespace {
constexpr std::size_t maximum_config_line = 65536;
constexpr auto maximum_sessions = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t user_field_count = 5;
struct FlagOption {
    std::string_view name;
    bool Config::*member;
};
constexpr std::array flags{
    FlagOption{.name = "debug", .member = &Config::debug},
    FlagOption{.name = "noecholog", .member = &Config::no_echo_log},
    FlagOption{.name = "nostripre", .member = &Config::no_strip_re},
    FlagOption{.name = "notearline", .member = &Config::no_tearline},
    FlagOption{.name = "nocancel", .member = &Config::no_cancel},
    FlagOption{.name = "strictnetmail", .member = &Config::strict_netmail},
    FlagOption{.name = "readorigin", .member = &Config::read_origin},
    FlagOption{.name = "noreplyaddr", .member = &Config::no_reply_address},
    FlagOption{.name = "noencode", .member = &Config::no_encode},
    FlagOption{.name = "notzutc", .member = &Config::no_tzutc},
    FlagOption{.name = "loggroups", .member = &Config::log_groups},
    FlagOption{.name = "note", .member = &Config::note},
};
struct UserOption {
    std::string_view name;
    bool UserOptions::*member;
};
constexpr std::array user_options{
    UserOption{.name = "flowed", .member = &UserOptions::flowed},
    UserOption{.name = "showto", .member = &UserOptions::showto},
    UserOption{.name = "nonbsp", .member = &UserOptions::nonbsp},
    UserOption{.name = "delssq", .member = &UserOptions::delssq},
    UserOption{.name = "addcr", .member = &UserOptions::addcr},
    UserOption{.name = "squote", .member = &UserOptions::squote},
};
struct PathOption {
    std::string_view name;
    std::filesystem::path Config::*member;
};
constexpr std::array paths{
    PathOption{.name = "allow", .member = &Config::allow_file},
    PathOption{.name = "groups", .member = &Config::groups_file},
    PathOption{.name = "users", .member = &Config::users_file},
    PathOption{.name = "xlat", .member = &Config::xlat_file},
    PathOption{.name = "logfile", .member = &Config::log_file},
    PathOption{.name = "echomailjam", .member = &Config::echomail_file},
};
auto normalized_option(std::string_view word) -> std::string {
    if (word.starts_with("--")) {
        word.remove_prefix(2);
    } else if (word.starts_with('-')) {
        word.remove_prefix(1);
    }
    auto name = lower_ascii(word);
    constexpr std::array aliases{
        std::pair{"p", "port"},    std::pair{"m", "max"},   std::pair{"a", "allow"},
        std::pair{"g", "groups"},  std::pair{"u", "users"}, std::pair{"x", "xlat"},
        std::pair{"l", "logfile"},
    };
    for (const auto &[short_name, long_name] : aliases) {
        if (name == short_name) {
            return long_name;
        }
    }
    return name;
}
auto set_value(Config &config, std::string_view name, std::string_view value) -> Result<void> {
    for (const auto &option : paths) {
        if (name == option.name) {
            config.*option.member = value;
            return {};
        }
    }
    if (name.starts_with("def_") && name != "def_squote") {
        return set_user_option(config.defaults, {.name = name.substr(4), .value = value});
    }
    if (name == "origin") {
        config.origin = value;
        return {};
    }
    if (name == "guestsuffix") {
        config.guest_suffix = value;
        return {};
    }
    if (name == "conversion_errors") {
        if (equal_ascii(value, "strict")) {
            config.conversion_policy = ConversionPolicy::strict;
        } else if (equal_ascii(value, "replace")) {
            config.conversion_policy = ConversionPolicy::replace;
        } else {
            return failure(ErrorKind::input, "conversion_errors must be strict or replace");
        }
        return {};
    }
    if (name == "port" || name == "max") {
        const auto maximum =
            name == "port" ? std::numeric_limits<std::uint16_t>::max() : maximum_sessions;
        auto number = parse_number(value, maximum);
        if (!number) {
            return failure(ErrorKind::input, "Invalid " + std::string{name});
        }
        if (name == "port") {
            config.port = static_cast<std::uint16_t>(*number);
        } else {
            config.maximum_connections = *number;
        }
        return {};
    }
    return failure(ErrorKind::input, "Unknown option: " + std::string{name});
}

using Rows = std::vector<std::vector<std::string>>;
auto read_rows(const std::filesystem::path &path) -> Result<Rows> {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return failure(ErrorKind::io, "Not a readable configuration file: " + path.string());
    }
    std::ifstream input{path};
    if (!input) {
        return failure(ErrorKind::io, "Cannot open configuration: " + path.string());
    }
    Rows rows;
    std::string line;
    std::size_t line_number{};
    constexpr std::size_t maximum_config_file = 16UZ * 1024 * 1024;
    std::size_t total = 0;
    while (input.peek() != std::char_traits<char>::eof()) {
        line.clear();
        char byte{};
        while (input.get(byte) && byte != '\n') {
            if (++total > maximum_config_file || line.size() == maximum_config_line) {
                return failure(ErrorKind::input, path.string() + ": excessive configuration input");
            }
            line += byte;
        }
        if (++total > maximum_config_file) {
            return failure(ErrorKind::input, path.string() + ": excessive configuration input");
        }
        ++line_number;
        if (line.size() > maximum_config_line || line.contains('\0')) {
            return failure(ErrorKind::input, path.string() + ":" + std::to_string(line_number) +
                                                 ": invalid or excessive configuration line");
        }
        auto words = config_words(line);
        if (!words) {
            return failure(ErrorKind::input, path.string() + ":" + std::to_string(line_number) +
                                                 ": " + words.error().message);
        }
        if (!words->empty()) {
            rows.push_back(std::move(*words));
        }
    }
    if (!input.eof()) {
        return failure(ErrorKind::io, "Cannot read configuration: " + path.string());
    }
    return rows;
}
auto make_group(const std::vector<std::string> &row) -> Result<Group> {
    if (row.size() < 4 || row.at(0).empty() || row.at(1).size() != 1 || row.at(2).empty() ||
        row.at(3).empty()) {
        return failure(ErrorKind::input,
                       "Group requires name, access letter, address and base path");
    }
    Group group{
        .name = row.at(0),
        .access = row.at(1).front(),
        .address = row.at(2),
        .base = row.at(3),
        .default_charset = {},
        .netmail = false,
        .local = false,
        .no_chrs = false,
    };
    group.netmail = group.name.starts_with('!');
    group.local = group.name.starts_with('$');
    if (group.netmail || group.local) {
        group.name.erase(0, 1);
    }
    constexpr std::size_t maximum_group_name = 99;
    if (group.name.empty() || group.name.size() > maximum_group_name ||
        group.name.find_first_of(" \t\r\n<>@$,") != std::string::npos ||
        std::ranges::any_of(group.name, [](char value) -> bool {
            return static_cast<unsigned char>(value) < ' ' || value == '\x7f';
        })) {
        return failure(ErrorKind::input, "Invalid or excessive group name");
    }
    for (const auto &option : std::span{row}.subspan(4)) {
        if (equal_ascii(option, "-nochrs")) {
            group.no_chrs = true;
        } else if (!option.empty() && !option.starts_with('-') && group.default_charset.empty()) {
            group.default_charset = option;
        } else {
            return failure(ErrorKind::input, "Unknown group option");
        }
    }
    return group;
}
auto load_implicit_config(Config &config, const std::filesystem::path &path) -> Result<void> {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        return failure(ErrorKind::io, "Cannot inspect default configuration: " + path.string());
    }
    return exists ? load_config(config, path) : Result<void>{};
}
auto apply_file_option(CommandLine &command, std::string_view name,
                       const std::filesystem::path &path) -> Result<void> {
    if (name == "create") {
        command.config = default_config();
        command.action = CommandAction::create_config;
        command.output_file = path;
        return {};
    }
    return load_config(command.config, path);
}
auto help_requested(std::span<const std::string> arguments) -> bool {
    return arguments.size() == 1 &&
           (arguments.front() == "?" || equal_ascii(arguments.front(), "-h") ||
            equal_ascii(arguments.front(), "--help"));
}
auto option_argument_count(std::string_view name) -> std::size_t {
    const auto *const flag = std::ranges::find(flags, name, &FlagOption::name);
    return flag != flags.end() || name == "smartquote" ? 1 : 2;
}
} // namespace

auto default_config_file() -> std::filesystem::path {
#ifdef _WIN32
    return "jamnntpd.config";
#else
    return "/etc/jamnntpd.config";
#endif
}
auto default_config() -> Config {
    const auto base = default_config_file().parent_path();
    Config config;
    config.allow_file = base / "jamnntpd.allow";
    config.groups_file = base / "jamnntpd.groups";
    config.users_file = base / "jamnntpd.users";
    config.xlat_file = base / "jamnntpd.xlat";
#ifdef _WIN32
    config.log_file = "jamnntpd.log";
#else
    config.log_file = "/var/spool/log/jamnntpd.log";
#endif
    return config;
}
auto apply_config_arguments(Config &config, std::span<const std::string> arguments)
    -> Result<void> {
    Config candidate = config;
    while (!arguments.empty()) {
        const auto name = normalized_option(arguments.front());
        arguments = arguments.subspan(1);
        if (name == "smartquote") {
            candidate.defaults.squote = true;
            continue;
        }
        const auto *const flag = std::ranges::find(flags, name, &FlagOption::name);
        if (flag != flags.end()) {
            candidate.*flag->member = true;
            continue;
        }
        if (arguments.empty()) {
            return failure(ErrorKind::input, "Missing argument for " + name);
        }
        auto result = set_value(candidate, name, arguments.front());
        if (!result) {
            return result;
        }
        arguments = arguments.subspan(1);
    }
    config = std::move(candidate);
    return {};
}
auto load_config(Config &config, const std::filesystem::path &path) -> Result<void> {
    auto rows = read_rows(path);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    Config candidate = config;
    for (const auto &row : *rows) {
        auto result = apply_config_arguments(candidate, row);
        if (!result) {
            return failure(result.error().kind, path.string() + ": " + result.error().message);
        }
    }
    config = std::move(candidate);
    return {};
}
auto parse_command_line(std::span<const std::string> arguments,
                        const std::filesystem::path &implicit_config) -> Result<CommandLine> {
    CommandLine command{.config = default_config(), .output_file = {}};
    if (help_requested(arguments)) {
        command.action = CommandAction::help;
        return command;
    }
    if (arguments.empty()) {
        if (auto loaded = load_implicit_config(command.config, implicit_config); !loaded) {
            return std::unexpected{loaded.error()};
        }
        return command;
    }
    while (!arguments.empty()) {
        if (!arguments.front().starts_with('-')) {
            return failure(ErrorKind::input, "Expected command-line option");
        }
        const auto name = normalized_option(arguments.front());
        if (name == "create" || name == "config") {
            if (arguments.size() < 2) {
                return failure(ErrorKind::input, "Missing filename for " + name);
            }
            if (auto loaded = apply_file_option(command, name, arguments.subspan(1).front());
                !loaded) {
                return std::unexpected{loaded.error()};
            }
            if (command.action == CommandAction::create_config) {
                return command;
            }
            arguments = arguments.subspan(2);
            continue;
        }
        const auto count = option_argument_count(name);
        if (arguments.size() < count) {
            return failure(ErrorKind::input, "Missing value for " + name);
        }
        if (auto applied = apply_config_arguments(command.config, arguments.first(count));
            !applied) {
            return std::unexpected{applied.error()};
        }
        arguments = arguments.subspan(count);
    }
    return command;
}
auto set_user_option(UserOptions &options, UserOptionSetting setting) -> Result<void> {
    const auto *const option =
        std::ranges::find(user_options, lower_ascii(setting.name), &UserOption::name);
    if (option == user_options.end()) {
        return failure(ErrorKind::input, "Unknown user option");
    }
    if (!equal_ascii(setting.value, "on") && !equal_ascii(setting.value, "off")) {
        return failure(ErrorKind::input, "User option must be on or off");
    }
    options.*option->member = equal_ascii(setting.value, "on");
    return {};
}
auto config_text(const Config &config) -> std::string {
    std::ostringstream output;
    output << "port " << config.port << "\nmax " << config.maximum_connections << '\n';
    for (const auto &option : paths) {
        const auto &path = config.*option.member;
        if (!path.empty()) {
            output << option.name << " \"" << path.string() << "\"\n";
        }
    }
    for (const auto &option : flags) {
        if (config.*option.member) {
            output << option.name << '\n';
        }
    }
    for (const auto &option : user_options) {
        if (option.name == "squote") {
            continue;
        }
        output << "def_" << option.name << ' ' << (config.defaults.*option.member ? "on" : "off")
               << '\n';
    }
    if (config.defaults.squote) {
        output << "smartquote\n";
    }
    if (config.origin) {
        output << "origin \"" << *config.origin << "\"\n";
    }
    if (config.guest_suffix) {
        output << "guestsuffix \"" << *config.guest_suffix << "\"\n";
    }
    output << "conversion_errors "
           << (config.conversion_policy == ConversionPolicy::strict ? "strict" : "replace") << '\n';
    return output.str();
}
auto load_groups(const std::filesystem::path &path) -> Result<std::vector<Group>> {
    auto rows = read_rows(path);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    std::vector<Group> groups;
    for (const auto &row : *rows) {
        auto group = make_group(row);
        if (!group) {
            return failure(group.error().kind, path.string() + ": " + group.error().message);
        }
        groups.push_back(std::move(*group));
    }
    if (groups.empty()) {
        return failure(ErrorKind::input, "No groups configured");
    }
    return groups;
}
auto guest_permissions(const std::filesystem::path &path, std::string_view address)
    -> Result<Permissions> {
    auto rows = read_rows(path);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    for (const auto &row : *rows) {
        if (row.size() > 3) {
            return failure(ErrorKind::input, "Invalid allow rule");
        }
        if (match_pattern(row.front(), address)) {
            return Permissions{
                .read = row.size() > 1 ? row.at(1) : "",
                .post = row.size() > 2 ? row.at(2) : "",
            };
        }
    }
    return failure(ErrorKind::permission, "Access denied");
}
auto authenticate(const std::filesystem::path &path, Credentials credentials) -> Result<Identity> {
    auto rows = read_rows(path);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    for (const auto &row : *rows) {
        if (row.size() < 4 || row.size() > user_field_count) {
            return failure(ErrorKind::input, "Invalid user rule");
        }
        if (equal_ascii(row.front(), credentials.username)) {
            if (row.at(1) != credentials.password) {
                break;
            }
            return Identity{
                .username = row.front(),
                .permissions = {.read = row.at(2), .post = row.at(3)},
                .realnames = row.size() == user_field_count ? row.at(4) : "",
            };
        }
    }
    return failure(ErrorKind::permission, "Authentication rejected");
}
} // namespace jamnntpd
