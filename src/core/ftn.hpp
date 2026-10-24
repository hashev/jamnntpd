#ifndef JAMNNTPD_CORE_FTN_HPP
#define JAMNNTPD_CORE_FTN_HPP
#include "charset_rules.hpp"
#include "config.hpp"
#include "session.hpp"
#include "storage.hpp"
#include <map>

namespace jamnntpd {
inline constexpr std::uint16_t field_origin = 0;
inline constexpr std::uint16_t field_destination = 1;
inline constexpr std::uint16_t field_sender = 2;
inline constexpr std::uint16_t field_recipient = 3;
inline constexpr std::uint16_t field_message_id = 4;
inline constexpr std::uint16_t field_reply_id = 5;
inline constexpr std::uint16_t field_subject = 6;
inline constexpr std::uint16_t field_pid = 7;
inline constexpr std::uint16_t field_kludge = 2000;
inline constexpr std::uint16_t field_timezone = 2004;
inline constexpr std::uint32_t attribute_local = 0x1;
inline constexpr std::uint32_t attribute_private = 0x4;
inline constexpr std::uint32_t attribute_sent = 0x10;
inline constexpr std::uint32_t attribute_locked = 0x40000000;
inline constexpr std::uint32_t attribute_type_local = 0x00800000;
inline constexpr std::uint32_t attribute_type_echo = 0x01000000;
inline constexpr std::uint32_t attribute_type_net = 0x02000000;
inline constexpr std::size_t maximum_post_bytes = 20000;
struct MessageText {
    std::string sender;
    std::string recipient;
    std::string subject;
    std::string body;
    std::string origin;
    std::string destination;
    std::string message_id;
    std::string reply_id;
    std::string reply_address;
    std::string timezone;
    Charset charset;
    bool opaque{};
    bool replaced{};
};
[[nodiscard]] auto jam_field(const jam::Message &message, std::uint16_t number) -> std::string;
void add_jam_field(jam::Message &message, std::uint16_t number, std::string_view text);
[[nodiscard]] auto message_charset(const jam::Message &message) -> Charset;
[[nodiscard]] auto encoding_name(const Charset &charset) -> std::string;
[[nodiscard]] auto read_message_text(const jam::Message &message, const Group &group,
                                     const CharsetConfig &charsets, ConversionPolicy policy)
    -> Result<MessageText>;
[[nodiscard]] auto identity_matches(const Identity &identity, std::string_view name,
                                    const MessageText &text) -> bool;
[[nodiscard]] auto visible_message(const Group &group, const Identity &identity,
                                   const MessageText &text) -> bool;
[[nodiscard]] auto render_message(const jam::Message &message, const MessageText &text,
                                  const Group &group, const UserOptions &options,
                                  const Config &config) -> Result<ProtocolArticle>;
} // namespace jamnntpd
#endif
