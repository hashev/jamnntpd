#include "ftn.hpp"
#include "formatting.hpp"
#include "message_values.hpp"
#include "mime.hpp"
#include "text.hpp"
#include <algorithm>
#include <array>
#include <format>

namespace jamnntpd {
namespace {
constexpr unsigned char high_bit_boundary = 128;
auto header_safe(std::string_view value) -> bool {
    return std::ranges::none_of(value, [](char byte) -> bool {
        return (static_cast<unsigned char>(byte) < ' ' && byte != '\t') || byte == '\x7f';
    });
}
auto kludge_value(std::string_view text, std::string_view prefix) -> std::string_view {
    if (text.size() >= prefix.size() && equal_ascii(text.substr(0, prefix.size()), prefix)) {
        return trim(text.substr(prefix.size()));
    }
    return {};
}
auto opaque_header(std::string_view bytes, std::string_view charset, bool no_encode)
    -> Result<std::string> {
    if (!header_safe(bytes) || !header_safe(charset)) {
        return failure(ErrorKind::corrupt, "Invalid stored header");
    }
    if (no_encode || std::ranges::all_of(bytes, [](char byte) -> bool {
            return static_cast<unsigned char>(byte) < high_bit_boundary;
        })) {
        return std::string{bytes};
    }
    constexpr std::size_t word_limit = 75;
    constexpr std::size_t overhead = 7;
    constexpr std::size_t quartet = 4;
    constexpr std::size_t triple = 3;
    if (charset.size() + overhead + quartet >= word_limit) {
        return failure(ErrorKind::encoding, "Opaque charset label too long");
    }
    const auto count = ((word_limit - charset.size() - overhead) / quartet) * triple;
    std::string output;
    while (!bytes.empty()) {
        if (!output.empty()) {
            output += "\r\n ";
        }
        const auto size = std::min(count, bytes.size());
        output += "=?" + std::string{charset} + "?B?" + encode_base64(bytes.substr(0, size)) + "?=";
        bytes.remove_prefix(size);
    }
    return output;
}
auto encoded_header(std::string_view value, const MessageText &text, bool no_encode)
    -> Result<std::string> {
    return text.opaque ? opaque_header(value, text.charset.chrs, no_encode)
                       : encode_header_words(value, {encoding_name(text.charset)}, no_encode);
}
auto wire_body(const MessageText &text, bool flowed, ConversionPolicy policy)
    -> Result<Conversion> {
    if (!text.opaque) {
        return format_read_text(text.body, {encoding_name(text.charset)}, flowed, policy);
    }
    // A reversible byte bridge permits framing opaque legacy data without claiming Unicode
    // semantics.
    auto bridge = transcode(text.body, {"ISO-8859-1"}, {"UTF-8"});
    if (!bridge) {
        return std::unexpected{bridge.error()};
    }
    return format_read_text(bridge->bytes, {"ISO-8859-1"}, flowed, policy);
}
auto address_for_mailbox(std::string_view address) -> std::string {
    auto parsed = parse_ftn_address(address);
    if (parsed) {
        return parsed->mailbox();
    }
    const auto mailbox = parse_mailbox(address);
    return mailbox && !mailbox->address.empty() ? mailbox->address : "unknown";
}
auto attributes_header(std::uint32_t attributes) -> std::string {
    if (attributes == 0) {
        return {};
    }
    constexpr std::array names{
        std::pair{std::uint32_t{0x00000001}, "Local"},
        std::pair{std::uint32_t{0x00000002}, "InTransit"},
        std::pair{std::uint32_t{0x00000004}, "Private"},
        std::pair{std::uint32_t{0x00000008}, "Read"},
        std::pair{std::uint32_t{0x00000010}, "Sent"},
        std::pair{std::uint32_t{0x00000020}, "KillSent"},
        std::pair{std::uint32_t{0x00000040}, "ArchiveSent"},
        std::pair{std::uint32_t{0x00000080}, "Hold"},
        std::pair{std::uint32_t{0x00000100}, "Crash"},
        std::pair{std::uint32_t{0x00000200}, "Immediate"},
        std::pair{std::uint32_t{0x00000400}, "Direct"},
        std::pair{std::uint32_t{0x00000800}, "Gate"},
        std::pair{std::uint32_t{0x00001000}, "FReq"},
        std::pair{std::uint32_t{0x00002000}, "FAttach"},
        std::pair{std::uint32_t{0x00004000}, "TruncFile"},
        std::pair{std::uint32_t{0x00008000}, "KillFile"},
        std::pair{std::uint32_t{0x00010000}, "ReceiptReq"},
        std::pair{std::uint32_t{0x00020000}, "ConfirmReq"},
        std::pair{std::uint32_t{0x00040000}, "Orphan"},
        std::pair{std::uint32_t{0x00080000}, "Encrypted"},
        std::pair{std::uint32_t{0x00100000}, "Compressed"},
        std::pair{std::uint32_t{0x00200000}, "Escaped"},
        std::pair{std::uint32_t{0x00400000}, "ForcePickup"},
        std::pair{std::uint32_t{0x00800000}, "TypeLocal"},
        std::pair{std::uint32_t{0x01000000}, "TypeEcho"},
        std::pair{std::uint32_t{0x02000000}, "TypeNet"},
        std::pair{std::uint32_t{0x20000000}, "NoDisp"},
        std::pair{std::uint32_t{0x40000000}, "Locked"},
        std::pair{std::uint32_t{0x80000000}, "Deleted"},
    };
    std::string result{"X-JAM-Attributes:"};
    for (const auto &[bit, name] : names) {
        if ((attributes & bit) != 0) {
            result += " " + std::string{name};
        }
    }
    return result + "\r\n";
}
auto decode_stored_value(std::string_view bytes, const ConversionPlan &plan,
                         ConversionPolicy policy, bool opaque) -> Result<Conversion> {
    auto converted = plan.apply(bytes, policy);
    if (!converted || opaque) {
        return converted;
    }
    auto unicode =
        transcode(converted->bytes, {encoding_name(plan.destination)}, {"UTF-8"}, policy);
    if (!unicode) {
        return std::unexpected{unicode.error()};
    }
    unicode->replaced = unicode->replaced || converted->replaced;
    return unicode;
}
} // namespace

auto jam_field(const jam::Message &message, std::uint16_t number) -> std::string {
    const auto found = std::ranges::find(message.fields, number, &jam::Subfield::low);
    return found == message.fields.end() ? "" : jam::text_from(found->data);
}
void add_jam_field(jam::Message &message, std::uint16_t number, std::string_view text) {
    message.fields.push_back({.low = number, .high = 0, .data = jam::bytes_from(text)});
}
auto encoding_name(const Charset &charset) -> std::string {
    if (equal_ascii(charset.chrs, "IBMPC")) {
        return "CP" + (charset.codepage.empty() ? "437" : charset.codepage);
    }
    return charset.chrs;
}
auto message_charset(const jam::Message &message) -> Charset {
    Charset charset;
    for (const auto &field : message.fields) {
        if (field.low != field_kludge) {
            continue;
        }
        const auto text = jam::text_from(field.data);
        if (const auto value = kludge_value(text, "CHRS: "); !value.empty()) {
            charset.chrs = value.substr(0, value.find(' '));
        } else if (const auto alternate = kludge_value(text, "CHARSET: "); !alternate.empty()) {
            charset.chrs = alternate;
        } else if (const auto page = kludge_value(text, "CODEPAGE: "); !page.empty()) {
            charset.codepage = page;
        }
    }
    return charset;
}
auto read_message_text(const jam::Message &message, const Group &group,
                       const CharsetConfig &charsets, ConversionPolicy policy)
    -> Result<MessageText> {
    const auto plan = resolve_read(charsets, {
                                                 .metadata = message_charset(message),
                                                 .group_default = group.default_charset,
                                                 .destination_pattern = std::nullopt,
                                             });
    if (!plan) {
        return std::unexpected{plan.error()};
    }
    MessageText text{
        .sender = {},
        .recipient = {},
        .subject = {},
        .body = {},
        .origin = jam_field(message, field_origin),
        .destination = jam_field(message, field_destination),
        .message_id = jam_field(message, field_message_id),
        .reply_id = jam_field(message, field_reply_id),
        .reply_address = {},
        .timezone = jam_field(message, field_timezone),
        .charset = plan->destination,
        .opaque = false,
        .replaced = false,
    };
    const auto probe = transcode({}, {encoding_name(plan->destination)}, {"UTF-8"});
    text.opaque = !probe;
    if (text.opaque && plan->method == ConversionMethod::builtin) {
        return std::unexpected{probe.error()};
    }
    for (auto [number, target] : std::array{
             std::pair{field_sender, &text.sender},
             std::pair{field_recipient, &text.recipient},
             std::pair{field_subject, &text.subject},
         }) {
        auto converted =
            decode_stored_value(jam_field(message, number), *plan, policy, text.opaque);
        if (!converted) {
            return std::unexpected{converted.error()};
        }
        if (!header_safe(converted->bytes)) {
            return failure(ErrorKind::corrupt, "Control in stored header");
        }
        *target = std::move(converted->bytes);
        text.replaced = text.replaced || converted->replaced;
    }
    const auto bytes =
        remove_soft_cr(message.text, {encoding_name(plan->source)}, plan->keep_soft_cr);
    auto body = decode_stored_value(bytes, *plan, policy, text.opaque);
    if (!body) {
        return std::unexpected{body.error()};
    }
    text.replaced = text.replaced || body->replaced;
    text.body = std::move(body->bytes);
    for (const auto &field : message.fields) {
        if (field.low != field_kludge) {
            continue;
        }
        const auto value = jam::text_from(field.data);
        if (const auto zone = kludge_value(value, "TZUTC: "); !zone.empty()) {
            text.timezone = zone;
        }
        if (const auto zone = kludge_value(value, "TZUTCINFO: "); !zone.empty()) {
            text.timezone = zone;
        }
        if (const auto address = kludge_value(value, "REPLYADDR "); !address.empty()) {
            text.reply_address = address;
        }
    }
    for (const auto *value :
         {&text.origin, &text.destination, &text.message_id, &text.reply_id, &text.reply_address}) {
        if (!header_safe(*value)) {
            return failure(ErrorKind::corrupt, "Control in stored metadata");
        }
    }
    return text;
}
auto identity_matches(const Identity &identity, std::string_view name, const MessageText &text)
    -> bool {
    if (text.opaque) {
        return match_names({identity.realnames}, name);
    }
    const auto bytes = transcode(name, {"UTF-8"}, {encoding_name(text.charset)});
    return bytes && match_names({identity.realnames}, bytes->bytes);
}
auto visible_message(const Group &group, const Identity &identity, const MessageText &text)
    -> bool {
    return match_group(identity.permissions.read, group.access) &&
           (!group.netmail || identity_matches(identity, text.sender, text) ||
            identity_matches(identity, text.recipient, text));
}
auto render_message(const jam::Message &message, const MessageText &text, const Group &group,
                    const UserOptions &options, const Config &config) -> Result<ProtocolArticle> {
    const auto name = text.sender.empty() ? "unknown" : text.sender;
    const auto recipient = text.recipient.empty() ? "(none)" : text.recipient;
    auto from =
        encoded_header(options.showto ? name + " -> " + recipient : name, text, config.no_encode);
    auto recipient_header = encoded_header(recipient, text, config.no_encode);
    auto subject = encoded_header(text.subject, text, config.no_encode);
    auto date = format_jam_date(message.header.written, text.timezone);
    auto body = wire_body(text, options.flowed, config.conversion_policy);
    if (!from || !recipient_header || !subject || !date || !body) {
        return failure(ErrorKind::encoding, "Cannot render stored article");
    }
    const auto identifier = std::format("<{}${}@JamNNTPd>", message.header.number, group.name);
    const auto references =
        message.header.reply_to == 0
            ? ""
            : std::format("<{}${}@JamNNTPd>", message.header.reply_to, group.name);
    std::string origin = text.origin;
    if (config.read_origin && !group.netmail && !group.local) {
        if (auto address = origin_address(message.text); address) {
            origin = address->text();
        }
    }
    const auto mailbox =
        address_for_mailbox(text.reply_address.empty() ? origin : text.reply_address);
    ProtocolArticle article;
    article.headers = "Path: JamNNTPd!not-for-mail\r\nFrom: " + *from + " <" + mailbox +
                      ">\r\nX-Comment-To: " + *recipient_header + "\r\nNewsgroups: " + group.name +
                      "\r\nSubject: " + *subject + "\r\nDate: " + *date +
                      "\r\nMessage-ID: " + identifier + "\r\n";
    if (!references.empty()) {
        article.headers += "References: " + references + "\r\n";
    }
    const auto wire_sender = encoded_header(name, text, true);
    const auto wire_recipient = encoded_header(recipient, text, true);
    if (!wire_sender || !wire_recipient) {
        return failure(ErrorKind::encoding, "Invalid display name");
    }
    article.headers += "X-JAM-From: " + *wire_sender + (origin.empty() ? "" : " <" + origin + ">") +
                       "\r\nX-JAM-To: " + *wire_recipient +
                       (text.destination.empty() ? "" : " <" + text.destination + ">") + "\r\n";
    constexpr std::array extra_fields{
        std::pair{field_message_id, "MSGID"},
        std::pair{field_reply_id, "REPLYID"},
        std::pair{field_pid, "PID"},
        std::pair{std::uint16_t{8}, "TRACE"},
        std::pair{field_kludge, "FTSKLUDGE"},
        std::pair{std::uint16_t{2001}, "SEENBY2D"},
        std::pair{std::uint16_t{2002}, "PATH2D"},
        std::pair{std::uint16_t{2003}, "FLAGS"},
        std::pair{field_timezone, "TZUTCINFO"},
    };
    for (const auto &field : message.fields) {
        const auto *const found =
            std::ranges::find(extra_fields, field.low,
                              [](const auto &entry) -> std::uint16_t { return entry.first; });
        if (found == extra_fields.end()) {
            continue;
        }
        const auto value = jam::text_from(field.data);
        if (!header_safe(value)) {
            return failure(ErrorKind::corrupt, "Invalid JAM header metadata");
        }
        article.headers += "X-JAM-" + std::string{found->second} + ": " + value + "\r\n";
    }
    article.headers += attributes_header(message.header.attributes);
    const bool high_bit = std::ranges::any_of(body->bytes, [](char value) -> bool {
        return static_cast<unsigned char>(value) >= high_bit_boundary;
    });
    const auto charset = high_bit ? text.charset.chrs : "us-ascii";
    article.headers +=
        std::format("MIME-Version: 1.0\r\nContent-Type: text/plain; charset={}; "
                    "format={}\r\nContent-Transfer-Encoding: {}\r\n",
                    charset, options.flowed ? "flowed" : "fixed", high_bit ? "8bit" : "7bit");
    article.body = std::move(body->bytes);
    auto overview_from = *from;
    auto overview_subject = *subject;
    for (auto *value : {&overview_from, &overview_subject}) {
        std::erase(*value, '\r');
        std::erase(*value, '\n');
        std::ranges::replace(*value, '\t', ' ');
    }
    article.overview = overview_subject + "\t" + overview_from + " <" + mailbox + ">\t" + *date +
                       "\t" + identifier + "\t" + references + "\t\t";
    return article;
}
} // namespace jamnntpd
