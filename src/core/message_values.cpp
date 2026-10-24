#include "message_values.hpp"
#include "text.hpp"
#include <array>
#include <ctime>
#include <format>
#include <limits>
#include <utility>

namespace jamnntpd {
namespace {
constexpr std::uint32_t maximum_zone_hours = 23;
constexpr std::uint32_t maximum_zone_minutes = 59;
constexpr std::int64_t minutes_per_hour = 60;
constexpr std::string_view months = "JanFebMarAprMayJunJulAugSepOctNovDec";
constexpr std::array weekdays{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
auto header_safe(std::string_view text) -> bool {
    return text.find_first_of("\r\n") == std::string_view::npos && !text.contains('\0');
}
auto unquote(std::string_view text) -> Result<std::string> {
    if (!text.starts_with('"')) {
        return std::string{text};
    }
    if (text.size() < 2 || !text.ends_with('"')) {
        return failure(ErrorKind::input, "Unclosed mailbox quote");
    }
    text.remove_prefix(1);
    text.remove_suffix(1);
    std::string output;
    while (!text.empty()) {
        if (text.front() == '\\') {
            text.remove_prefix(1);
            if (text.empty()) {
                return failure(ErrorKind::input, "Unfinished mailbox escape");
            }
        } else if (text.front() == '"') {
            return failure(ErrorKind::input, "Unescaped mailbox quote");
        }
        output += text.front();
        text.remove_prefix(1);
    }
    return output;
}
auto mailbox_angle(std::string_view text) -> Result<std::size_t> {
    bool quoted = false;
    bool escaped = false;
    std::size_t angle = std::string_view::npos;
    for (std::size_t position = 0; position < text.size(); ++position) {
        const auto byte = text.at(position);
        if (escaped) {
            escaped = false;
        } else if (quoted && byte == '\\') {
            escaped = true;
        } else if (byte == '"') {
            quoted = !quoted;
        } else if (!quoted && byte == '<') {
            angle = position;
            break;
        } else if (!quoted && byte == ',') {
            return failure(ErrorKind::input, "Multiple mailboxes are unsupported");
        }
    }
    if (quoted || escaped) {
        return failure(ErrorKind::input, "Unclosed mailbox quote");
    }
    return angle;
}
} // namespace

auto parse_utc_offset(std::string_view text) -> Result<UtcOffset> {
    const bool negative = text.starts_with('-');
    if (negative || text.starts_with('+')) {
        text.remove_prefix(1);
    }
    if (text.size() != 4) {
        return failure(ErrorKind::input, "Timezone must be [+-]HHMM");
    }
    auto hours = parse_number(text.substr(0, 2), maximum_zone_hours);
    auto minutes = parse_number(text.substr(2), maximum_zone_minutes);
    if (!hours || !minutes) {
        return failure(ErrorKind::input, "Invalid timezone offset");
    }
    const auto total = (static_cast<std::int64_t>(*hours) * minutes_per_hour) + *minutes;
    return UtcOffset{std::chrono::minutes{negative ? -total : total}};
}
auto format_utc_offset(UtcOffset offset) -> Result<std::string> {
    const auto count = offset.value.count();
    constexpr auto maximum = (maximum_zone_hours * minutes_per_hour) + maximum_zone_minutes;
    if (count < -maximum || count > maximum) {
        return failure(ErrorKind::input, "Timezone offset outside representable range");
    }
    const auto magnitude = count < 0 ? -count : count;
    return std::format("{}{:02}{:02}", count < 0 ? "-" : "+", magnitude / minutes_per_hour,
                       magnitude % minutes_per_hour);
}
auto local_jam_time(std::chrono::sys_seconds time) -> Result<JamTime> {
    constexpr std::int64_t seconds_per_day = 86400;
    const auto epoch_seconds = time.time_since_epoch().count();
    if (epoch_seconds < -seconds_per_day ||
        epoch_seconds > std::numeric_limits<std::uint32_t>::max() + seconds_per_day) {
        return failure(ErrorKind::input, "Timestamp exceeds JAM calendar range");
    }
    const auto native = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &native) != 0) {
#else
    if (localtime_r(&native, &local) == nullptr) {
#endif
        return failure(ErrorKind::input, "Cannot represent local timestamp");
    }
    using namespace std::chrono;
    constexpr int base_year = 1900;
    const year_month_day calendar{year{local.tm_year + base_year},
                                  month{static_cast<unsigned>(local.tm_mon + 1)},
                                  day{static_cast<unsigned>(local.tm_mday)}};
    if (!calendar.ok()) {
        return failure(ErrorKind::input, "Invalid local calendar date");
    }
    const auto wall =
        sys_days{calendar} + hours{local.tm_hour} + minutes{local.tm_min} + seconds{local.tm_sec};
    const auto count = duration_cast<seconds>(wall.time_since_epoch()).count();
    if (count < 0 || std::cmp_greater(count, std::numeric_limits<std::uint32_t>::max())) {
        return failure(ErrorKind::input, "Timestamp exceeds JAM uint32 range");
    }
    return JamTime{
        .wall_seconds = static_cast<std::uint32_t>(count),
        .offset = {duration_cast<minutes>(wall - time)},
    };
}
auto format_jam_date(std::uint32_t wall_seconds, std::string_view timezone) -> Result<std::string> {
    std::string label{"GMT"};
    if (!timezone.empty()) {
        const auto offset = parse_utc_offset(timezone);
        if (!offset) {
            return std::unexpected{offset.error()};
        }
        auto formatted = format_utc_offset(*offset);
        if (!formatted) {
            return std::unexpected{formatted.error()};
        }
        label = *formatted;
    }
    using namespace std::chrono;
    const sys_seconds wall{seconds{wall_seconds}};
    const auto date = floor<days>(wall);
    const year_month_day calendar{date};
    const hh_mm_ss clock{wall - date};
    const auto month = static_cast<unsigned>(calendar.month());
    constexpr std::size_t month_length = 3;
    return std::format("{}, {} {} {} {:02}:{:02}:{:02} {}", weekdays.at(weekday{date}.c_encoding()),
                       static_cast<unsigned>(calendar.day()),
                       months.substr((month - 1) * month_length, month_length),
                       static_cast<int>(calendar.year()), clock.hours().count(),
                       clock.minutes().count(), clock.seconds().count(), label);
}
auto FtnAddress::text() const -> std::string {
    const auto base = std::format("{}:{}/{}", zone, net, node);
    return point == 0 ? base : base + "." + std::to_string(point);
}
auto FtnAddress::mailbox() const -> std::string {
    return std::format("{}@{}.{}.{}", point, node, net, zone);
}
auto parse_ftn_address(std::string_view text) -> Result<FtnAddress> {
    if (!header_safe(text)) {
        return failure(ErrorKind::input, "Control characters in FTN address");
    }
    // Retain the legacy normalization: an optional @domain is discarded.
    text = text.substr(0, text.find('@'));
    const auto colon = text.find(':');
    const auto slash = text.find('/');
    if (colon == std::string_view::npos || slash == std::string_view::npos || colon >= slash) {
        return failure(ErrorKind::input, "Expected zone:net/node[.point]");
    }
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    const auto zone = parse_number(text.substr(0, colon), maximum);
    const auto net = parse_number(text.substr(colon + 1, slash - colon - 1), maximum);
    text.remove_prefix(slash + 1);
    const auto dot = text.find('.');
    const auto node = parse_number(text.substr(0, dot), maximum);
    const auto point = dot == std::string_view::npos ? Result<std::uint32_t>{0}
                                                     : parse_number(text.substr(dot + 1), maximum);
    if (!zone || !net || !node || !point) {
        return failure(ErrorKind::input, "Invalid FTN address component");
    }
    return FtnAddress{.zone = *zone, .net = *net, .node = *node, .point = *point};
}
auto parse_mailbox(std::string_view text) -> Result<Mailbox> {
    if (!header_safe(text)) {
        return failure(ErrorKind::input, "Control characters in mailbox");
    }
    text = trim(text);
    const auto opening = mailbox_angle(text);
    if (!opening) {
        return std::unexpected{opening.error()};
    }
    const auto angle = *opening;
    if (angle == std::string_view::npos) {
        auto name = unquote(text);
        if (!name) {
            return std::unexpected{name.error()};
        }
        const bool address = text.contains('@') && !text.starts_with('"');
        if (text.contains('>') ||
            (address && text.find_first_of(" \t\"") != std::string_view::npos)) {
            return failure(ErrorKind::input, "Malformed bare mailbox");
        }
        return Mailbox{.name = address ? "" : *name, .address = address ? *name : ""};
    }
    if (!text.ends_with('>')) {
        return failure(ErrorKind::input, "Malformed mailbox brackets");
    }
    auto name = unquote(trim(text.substr(0, angle)));
    const auto address = text.substr(angle + 1, text.size() - angle - 2);
    if (!name || address.empty() || address.find_first_of("<> \t,\"") != std::string_view::npos) {
        return failure(ErrorKind::input, "Malformed mailbox name or address");
    }
    return Mailbox{.name = *name, .address = std::string{address}};
}
auto origin_address(std::string_view text) -> Result<FtnAddress> {
    std::string_view origin;
    while (!text.empty()) {
        const auto end = text.find_first_of("\r\n");
        const auto line = text.substr(0, end);
        if (line.starts_with(" * Origin: ")) {
            origin = line;
        }
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
    const auto opening = origin.rfind('(');
    if (opening == std::string_view::npos || !origin.ends_with(')')) {
        return failure(ErrorKind::input, "No address in origin line");
    }
    return parse_ftn_address(origin.substr(opening + 1, origin.size() - opening - 2));
}
} // namespace jamnntpd
