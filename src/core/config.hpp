#ifndef JAMNNTPD_CORE_CONFIG_HPP
#define JAMNNTPD_CORE_CONFIG_HPP

#include "codec.hpp"
#include "error.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jamnntpd {
struct UserOptions {
    bool flowed{true};
    bool showto{true};
    bool nonbsp{};
    bool delssq{true};
    bool addcr{true};
    bool squote{};
};
inline constexpr std::uint16_t default_port = 5000;
inline constexpr std::uint32_t default_maximum_connections = 5;
struct Config {
    std::uint16_t port{default_port};
    std::uint32_t maximum_connections{default_maximum_connections};
    std::filesystem::path allow_file;
    std::filesystem::path groups_file;
    std::filesystem::path users_file;
    std::filesystem::path xlat_file;
    std::filesystem::path log_file;
    std::filesystem::path echomail_file;
    std::optional<std::string> origin;
    std::optional<std::string> guest_suffix;
    UserOptions defaults;
    ConversionPolicy conversion_policy{ConversionPolicy::strict};
    bool debug{};
    bool no_echo_log{};
    bool no_strip_re{};
    bool no_tearline{};
    bool no_cancel{};
    bool strict_netmail{};
    bool read_origin{};
    bool no_reply_address{};
    bool no_encode{};
    bool no_tzutc{};
    bool log_groups{};
    bool note{};
};
[[nodiscard]] auto default_config() -> Config;
[[nodiscard]] auto default_config_file() -> std::filesystem::path;
[[nodiscard]] auto apply_config_arguments(Config &config, std::span<const std::string> arguments)
    -> Result<void>;
[[nodiscard]] auto load_config(Config &config, const std::filesystem::path &path) -> Result<void>;
[[nodiscard]] auto config_text(const Config &config) -> std::string;
enum class CommandAction : std::uint8_t { run, help, create_config };
struct CommandLine {
    Config config;
    CommandAction action{CommandAction::run};
    std::filesystem::path output_file;
};
[[nodiscard]] auto
parse_command_line(std::span<const std::string> arguments,
                   const std::filesystem::path &implicit_config = default_config_file())
    -> Result<CommandLine>;
struct UserOptionSetting {
    std::string_view name;
    std::string_view value;
};
[[nodiscard]] auto set_user_option(UserOptions &options, UserOptionSetting setting) -> Result<void>;

struct Group {
    std::string name;
    char access{};
    std::string address;
    std::filesystem::path base;
    std::string default_charset;
    bool netmail{};
    bool local{};
    bool no_chrs{};
};
struct Permissions {
    std::string read;
    std::string post;
};
struct Identity {
    std::string username;
    Permissions permissions;
    std::string realnames;
};
[[nodiscard]] auto load_groups(const std::filesystem::path &path) -> Result<std::vector<Group>>;
[[nodiscard]] auto guest_permissions(const std::filesystem::path &path, std::string_view address)
    -> Result<Permissions>;
struct Credentials {
    std::string_view username;
    std::string_view password;
};
[[nodiscard]] auto authenticate(const std::filesystem::path &path, Credentials credentials)
    -> Result<Identity>;
} // namespace jamnntpd
#endif
