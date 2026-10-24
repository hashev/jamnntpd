#include "post_builder.hpp"
#include "formatting.hpp"
#include "message_values.hpp"
#include "text.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <utility>

namespace jamnntpd {
namespace {
constexpr std::size_t name_limit = 35;
constexpr std::size_t subject_limit = 71;
constexpr std::size_t newsreader_limit = 75;
constexpr std::size_t address_limit = 100;
auto header(const RawMimeArticle &raw, std::string_view name) -> std::string_view {
    const auto found = raw.headers.find(std::string{name});
    return found == raw.headers.end() ? std::string_view{} : std::string_view{found->second};
}
auto next_message_id() -> std::uint64_t {
    static std::atomic<std::uint64_t> previous{};
    constexpr std::uint64_t step = 10;
    const auto now =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count()) /
        step * step;
    auto expected = previous.load();
    while (!previous.compare_exchange_weak(expected, std::max(now, expected + 1))) {
    }
    return std::max(now, expected + 1);
}
struct PostConversion {
    ConversionPlan plan;
    ConversionPolicy policy;
    bool opaque{};
    bool replaced{};
    std::string codec;
    auto convert(std::string_view bytes) -> Result<Conversion> {
        auto converted = plan.apply(bytes, policy);
        if (!converted) {
            return std::unexpected{converted.error()};
        }
        auto unicode = transcode(converted->bytes, {codec}, {"UTF-8"}, policy);
        if (!unicode) {
            return std::unexpected{unicode.error()};
        }
        unicode->replaced = unicode->replaced || converted->replaced;
        replaced = replaced || unicode->replaced;
        return unicode;
    }
};
auto post_conversion(const ConversionPlan &plan, ConversionPolicy policy) -> PostConversion {
    auto codec = encoding_name(plan.destination);
    const bool opaque = !transcode({}, {codec}, {"UTF-8"});
    if (opaque) {
        codec = "ISO-8859-1";
    }
    return {.plan = plan, .policy = policy, .opaque = opaque, .replaced = false, .codec = codec};
}
auto encoded_field(std::string_view unicode, DestinationEncoding encoding, std::size_t maximum,
                   ConversionPolicy policy) -> Result<Conversion> {
    if (std::ranges::any_of(unicode, [](char byte) -> bool {
            return (static_cast<unsigned char>(byte) < ' ' && byte != '\t') || byte == '\x7f';
        })) {
        return failure(ErrorKind::input, "Multiline message field");
    }
    return encode_post_text(unicode, encoding, maximum, policy);
}
struct PostFields {
    Mailbox from;
    std::string subject;
    std::string organization;
    std::string newsreader;
};
auto sender_override(PostFields &fields, const Identity &identity, PostConversion &conversion,
                     const Config &config) -> Result<void> {
    const auto first_name =
        std::string_view{identity.realnames}.substr(0, identity.realnames.find(','));
    if (!first_name.empty() && first_name.find_first_of("*?") == std::string_view::npos) {
        auto name = conversion.convert(first_name);
        if (!name) {
            return std::unexpected{name.error()};
        }
        fields.from.name = std::move(name->bytes);
    }
    if (identity.username.empty() && config.guest_suffix) {
        auto suffix = conversion.convert(*config.guest_suffix);
        if (!suffix) {
            return std::unexpected{suffix.error()};
        }
        fields.from.name += suffix->bytes;
    }
    return {};
}
auto decode_fields(const RawMimeArticle &raw, PostConversion &conversion, const Group &group,
                   const CharsetConfig &charsets) -> Result<PostFields> {
    const auto decode = [&](std::string_view bytes, SourceEncoding source) -> Result<Conversion> {
        auto selected =
            resolve_post(charsets, {.source = source.name, .group_default = group.default_charset});
        if (!selected) {
            return std::unexpected{selected.error()};
        }
        auto part = post_conversion(*selected, conversion.policy);
        if ((part.opaque || conversion.opaque) &&
            !equal_ascii(encoding_name(selected->destination),
                         encoding_name(conversion.plan.destination))) {
            return failure(ErrorKind::encoding, "Incompatible opaque header and body charsets");
        }
        auto result = part.convert(bytes);
        if (result) {
            conversion.replaced = conversion.replaced || result->replaced;
        }
        return result;
    };
    const auto fallback =
        raw.source_charset.empty() ? encoding_name(conversion.plan.source) : raw.source_charset;
    auto mailbox = parse_mailbox(header(raw, "from"));
    if (!mailbox || mailbox->name.empty() || mailbox->address.empty()) {
        return failure(ErrorKind::input, "No valid From line found");
    }
    auto from = decode_header_with(mailbox->name, {fallback}, decode);
    auto subject = decode_header_with(header(raw, "subject"), {fallback}, decode);
    auto organization = decode_header_with(header(raw, "organization"), {fallback}, decode);
    auto newsreader = decode_header_with(
        header(raw, header(raw, "x-newsreader").empty() ? "user-agent" : "x-newsreader"),
        {fallback}, decode);
    if (!from || !subject || !organization || !newsreader) {
        return failure(ErrorKind::encoding, "Invalid textual post header");
    }
    mailbox->name = std::move(from->bytes);
    return PostFields{
        .from = std::move(*mailbox),
        .subject = std::move(subject->bytes),
        .organization = std::move(organization->bytes),
        .newsreader = std::move(newsreader->bytes),
    };
}
struct PostBody {
    std::string text;
    std::string recipient;
    std::string address;
};
auto extract_to(PostBody &body, bool required) -> Result<void> {
    const auto end = body.text.find('\r');
    const auto line = std::string_view{body.text}.substr(0, end);
    const bool has_to = line.size() >= 3 && equal_ascii(line.substr(0, 3), "To:");
    if (!has_to) {
        return required ? Result<void>{failure(ErrorKind::input, "Netmail requires a To line")}
                        : Result<void>{};
    }
    const auto value = trim(line.substr(3));
    const auto comma = value.find(',');
    body.recipient = trim(value.substr(0, comma));
    if (comma != std::string_view::npos) {
        body.address = trim(value.substr(comma + 1));
    }
    body.text.erase(0, end == std::string::npos ? body.text.size() : end + 1);
    if (body.text.starts_with('\r')) {
        body.text.erase(0, 1);
    }
    return {};
}
auto format_body(const RawMimeArticle &raw, PostConversion &conversion, const PostFields &fields,
                 const ReplyTarget &parent, const Group &group, const UserOptions &options,
                 const Config &config) -> Result<PostBody> {
    auto decoded = conversion.convert(raw.body);
    if (!decoded) {
        return std::unexpected{decoded.error()};
    }
    auto formatted = format_post_text(
        decoded->bytes,
        {
            .flowed = raw.flowed,
            .delete_stuffed_quote_space = options.delssq,
            .replace_nbsp = options.nonbsp,
            .quote_name = options.squote && !header(raw, "references").empty() ? parent.name : "",
        });
    if (!formatted) {
        return std::unexpected{formatted.error()};
    }
    PostBody body{
        .text = std::move(*formatted),
        .recipient = parent.name,
        .address = parent.address,
    };
    if (auto parsed = extract_to(body, group.netmail && header(raw, "references").empty());
        !parsed) {
        return std::unexpected{parsed.error()};
    }
    if (group.netmail) {
        const auto address = parse_ftn_address(body.address);
        if (!address) {
            return std::unexpected{address.error()};
        }
        body.address = address->text();
    }
    if (!group.netmail && !group.local) {
        auto origin =
            Result<Conversion>{Conversion{.bytes = fields.organization, .replaced = false}};
        if (config.origin) {
            origin = conversion.convert(*config.origin);
        }
        if (!origin) {
            return std::unexpected{origin.error()};
        }
        auto appended = append_tearline(body.text, {
                                                       .add_cr = options.addcr,
                                                       .no_tearline = config.no_tearline,
                                                       .newsreader = fields.newsreader,
                                                       .origin = origin->bytes,
                                                       .address = group.address,
                                                   });
        if (!appended) {
            return std::unexpected{appended.error()};
        }
        body.text = std::move(*appended);
    }
    return body;
}
struct EncodedPost {
    std::string sender;
    std::string recipient;
    std::string subject;
    std::string body;
    std::string newsreader;
    bool replaced{};
};
auto encode_fields(const PostFields &fields, const PostBody &body, const PostConversion &conversion)
    -> Result<EncodedPost> {
    const auto sender =
        encoded_field(fields.from.name, {conversion.codec}, name_limit, conversion.policy);
    const auto recipient =
        encoded_field(body.recipient, {conversion.codec}, name_limit, conversion.policy);
    const auto subject =
        encoded_field(fields.subject, {conversion.codec}, subject_limit, conversion.policy);
    const auto text =
        encode_post_text(body.text, {conversion.codec}, maximum_post_bytes, conversion.policy);
    const auto newsreader =
        encoded_field(fields.newsreader, {conversion.codec}, newsreader_limit, conversion.policy);
    if (!sender || !recipient || !subject || !text || !newsreader) {
        return failure(ErrorKind::encoding,
                       "Post field exceeds encoded limit or is unrepresentable");
    }
    return EncodedPost{
        .sender = sender->bytes,
        .recipient = recipient->bytes,
        .subject = subject->bytes,
        .body = text->bytes,
        .newsreader = newsreader->bytes,
        .replaced = sender->replaced || recipient->replaced || subject->replaced ||
                    text->replaced || newsreader->replaced,
    };
}
auto message_attributes(const Group &group) -> std::uint32_t {
    if (group.netmail) {
        return attribute_local | attribute_private | attribute_type_net;
    }
    return attribute_local | (group.local ? attribute_type_local : attribute_type_echo);
}
auto add_post_metadata(jam::Message &message, const RawMimeArticle &raw, const PostFields &fields,
                       const ConversionPlan &plan, const Group &group, const Config &config)
    -> Result<void> {
    if (!config.no_reply_address) {
        const auto address = header(raw, "reply-to").empty() ? fields.from.address
                                                             : std::string{header(raw, "reply-to")};
        if (address.find_first_of("\r\n") != std::string::npos || address.size() > address_limit) {
            return failure(ErrorKind::input, "Invalid reply address");
        }
        add_jam_field(message, field_kludge, "REPLYADDR " + address);
    }
    if (!group.no_chrs && !plan.destination.chrs.empty()) {
        add_jam_field(message, field_kludge,
                      "CHRS: " + plan.destination.chrs +
                          (equal_ascii(plan.destination.chrs, "UTF-8") ? " 4" : " 2"));
        if (!plan.destination.codepage.empty()) {
            add_jam_field(message, field_kludge, "CODEPAGE: " + plan.destination.codepage);
        }
    }
    const auto now =
        local_jam_time(std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    if (!now) {
        return std::unexpected{now.error()};
    }
    const auto zone = format_utc_offset(now->offset);
    if (!zone) {
        return std::unexpected{zone.error()};
    }
    if (!config.no_tzutc) {
        add_jam_field(message, field_kludge, "TZUTC: " + *zone);
    }
    message.header.written = now->wall_seconds;
    message.header.received = now->wall_seconds;
    message.header.processed = now->wall_seconds;
    return {};
}
} // namespace

auto build_post(const RawMimeArticle &raw, const Group &group, const Identity &identity,
                const UserOptions &options, const ConversionPlan &plan,
                const CharsetConfig &charsets, const Config &config, const ReplyTarget &parent)
    -> Result<BuiltPost> {
    auto conversion = post_conversion(plan, config.conversion_policy);
    auto fields = decode_fields(raw, conversion, group, charsets);
    if (!fields) {
        return std::unexpected{fields.error()};
    }
    if (auto changed = sender_override(*fields, identity, conversion, config); !changed) {
        return std::unexpected{changed.error()};
    }
    if (!config.no_strip_re) {
        if (fields->subject.starts_with("Re: ")) {
            fields->subject.erase(0, 4);
        } else if (fields->subject == "Re:") {
            fields->subject.clear();
        }
    }
    auto body = format_body(raw, conversion, *fields, parent, group, options, config);
    if (!body) {
        return std::unexpected{body.error()};
    }
    auto encoded = encode_fields(*fields, *body, conversion);
    if (!encoded) {
        return std::unexpected{encoded.error()};
    }
    jam::Message message;
    add_jam_field(message, field_sender, encoded->sender);
    add_jam_field(message, field_recipient, encoded->recipient);
    add_jam_field(message, field_subject, encoded->subject);
    if (!group.local) {
        add_jam_field(message, field_origin, group.address);
    }
    if (group.netmail) {
        add_jam_field(message, field_destination, body->address);
    }
    const auto identifier = std::format("{} {:08x}", group.address, next_message_id());
    add_jam_field(message, field_message_id, identifier);
    if (!parent.message_id.empty()) {
        add_jam_field(message, field_reply_id, parent.message_id);
    }
    add_jam_field(message, field_pid, "JamNNTPd 2.0.0");
    if (auto added = add_post_metadata(message, raw, *fields, plan, group, config); !added) {
        return std::unexpected{added.error()};
    }
    if (config.note && !encoded->newsreader.empty()) {
        add_jam_field(message, field_kludge, "NOTE: " + encoded->newsreader);
    }
    message.header.message_crc = jam::crc32(identifier);
    message.header.reply_crc = jam::crc32(parent.message_id);
    message.header.reply_to = parent.number;
    message.header.attributes = message_attributes(group);
    message.text = std::move(encoded->body);
    return BuiltPost{
        .message = std::move(message),
        .replaced = conversion.replaced || encoded->replaced,
    };
}
} // namespace jamnntpd
