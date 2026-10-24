#include "jam_format.hpp"
#include "text.hpp"
#include <algorithm>
#include <climits>
#include <limits>

namespace jamnntpd::jam {
namespace {
constexpr std::array signature{std::byte{'J'}, std::byte{'A'}, std::byte{'M'}, std::byte{0}};
constexpr auto word_size = sizeof(std::uint32_t);
constexpr auto short_size = sizeof(std::uint16_t);
constexpr auto subfield_header_size = short_size + short_size + word_size;
constexpr auto byte_mask = std::numeric_limits<unsigned char>::max();
constexpr std::uint32_t crc_polynomial = 0xedb88320U;
constexpr int bits_per_octet = 8;
static_assert(CHAR_BIT == bits_per_octet);
constexpr std::array base_members{
    &BaseHeader::created,      &BaseHeader::modifications, &BaseHeader::active,
    &BaseHeader::password_crc, &BaseHeader::first_number,
};
constexpr std::array message_members{
    &MessageHeader::subfield_length, &MessageHeader::times_read,  &MessageHeader::message_crc,
    &MessageHeader::reply_crc,       &MessageHeader::reply_to,    &MessageHeader::first_reply,
    &MessageHeader::next_reply,      &MessageHeader::written,     &MessageHeader::received,
    &MessageHeader::processed,       &MessageHeader::number,      &MessageHeader::attributes,
    &MessageHeader::attributes2,     &MessageHeader::text_offset, &MessageHeader::text_length,
    &MessageHeader::password_crc,    &MessageHeader::cost,
};

auto take32(ByteView &data) -> std::uint32_t {
    std::uint32_t value{};
    unsigned int shift{};
    for (auto byte : data.first(word_size)) {
        value |= std::to_integer<std::uint32_t>(byte) << shift;
        shift += CHAR_BIT;
    }
    data = data.subspan(word_size);
    return value;
}
auto take16(ByteView &data) -> std::uint16_t {
    std::uint16_t value{};
    unsigned int shift{};
    for (auto byte : data.first(short_size)) {
        value = static_cast<std::uint16_t>(value | (std::to_integer<unsigned int>(byte) << shift));
        shift += CHAR_BIT;
    }
    data = data.subspan(short_size);
    return value;
}
void append32(Bytes &data, std::uint32_t value) {
    for (unsigned int shift = 0; shift < std::numeric_limits<std::uint32_t>::digits;
         shift += CHAR_BIT) {
        data.push_back(static_cast<std::byte>((value >> shift) & byte_mask));
    }
}
void append16(Bytes &data, std::uint16_t value) {
    for (unsigned int shift = 0; shift < std::numeric_limits<std::uint16_t>::digits;
         shift += CHAR_BIT) {
        data.push_back(
            static_cast<std::byte>((static_cast<std::uint32_t>(value) >> shift) & byte_mask));
    }
}
auto valid_header(ByteView bytes, std::size_t size) -> bool {
    return bytes.size() == size && std::ranges::equal(bytes.first(signature.size()), signature);
}
} // namespace

auto decode_base(ByteView bytes) -> Result<BaseHeader> {
    if (!valid_header(bytes, base_size)) {
        return failure(ErrorKind::corrupt, "Invalid JAM base header");
    }
    bytes = bytes.subspan(signature.size());
    BaseHeader header;
    for (auto member : base_members) {
        header.*member = take32(bytes);
    }
    std::ranges::copy(bytes, header.reserved.begin());
    return header;
}
auto encode_base(const BaseHeader &header) -> Bytes {
    Bytes bytes(signature.begin(), signature.end());
    for (auto member : base_members) {
        append32(bytes, header.*member);
    }
    bytes.insert(bytes.end(), header.reserved.begin(), header.reserved.end());
    return bytes;
}
auto decode_message(ByteView bytes) -> Result<MessageHeader> {
    if (!valid_header(bytes, message_size)) {
        return failure(ErrorKind::corrupt, "Invalid JAM message header");
    }
    bytes = bytes.subspan(signature.size());
    MessageHeader header;
    header.revision = take16(bytes);
    header.reserved = take16(bytes);
    if (header.revision != 1) {
        return failure(ErrorKind::corrupt, "Invalid JAM message revision");
    }
    for (auto member : message_members) {
        header.*member = take32(bytes);
    }
    return header;
}
auto encode_message(const MessageHeader &header) -> Bytes {
    Bytes bytes(signature.begin(), signature.end());
    append16(bytes, header.revision);
    append16(bytes, header.reserved);
    for (auto member : message_members) {
        append32(bytes, header.*member);
    }
    return bytes;
}
auto decode_index(ByteView bytes) -> Result<Index> {
    if (bytes.size() != index_size) {
        return failure(ErrorKind::corrupt, "Invalid JAM index record");
    }
    return Index{.user_crc = take32(bytes), .header_offset = take32(bytes)};
}
auto encode_index(Index index) -> Bytes {
    Bytes bytes;
    append32(bytes, index.user_crc);
    append32(bytes, index.header_offset);
    return bytes;
}
auto decode_lastread(ByteView bytes) -> Result<LastRead> {
    if (bytes.size() != lastread_size) {
        return failure(ErrorKind::corrupt, "Invalid JAM lastread record");
    }
    return LastRead{
        .user_crc = take32(bytes),
        .user_id = take32(bytes),
        .last = take32(bytes),
        .high = take32(bytes),
    };
}
auto encode_lastread(LastRead record) -> Bytes {
    Bytes bytes;
    for (auto value : {record.user_crc, record.user_id, record.last, record.high}) {
        append32(bytes, value);
    }
    return bytes;
}
auto decode_subfields(ByteView bytes) -> Result<std::vector<Subfield>> {
    std::vector<Subfield> fields;
    while (!bytes.empty()) {
        if (bytes.size() < subfield_header_size) {
            return failure(ErrorKind::corrupt, "Truncated JAM subfield header");
        }
        const auto low = take16(bytes);
        const auto high = take16(bytes);
        const auto length = take32(bytes);
        if (length > bytes.size()) {
            return failure(ErrorKind::corrupt, "Truncated JAM subfield contents");
        }
        const auto contents = bytes.first(length);
        fields.push_back(Subfield{
            .low = low,
            .high = high,
            .data = Bytes{contents.begin(), contents.end()},
        });
        bytes = bytes.subspan(length);
    }
    return fields;
}
auto encode_subfields(std::span<const Subfield> fields) -> Result<Bytes> {
    Bytes bytes;
    constexpr auto limit = std::numeric_limits<std::uint32_t>::max();
    for (const auto &field : fields) {
        if (bytes.size() > limit - subfield_header_size ||
            field.data.size() > limit - subfield_header_size - bytes.size()) {
            return failure(ErrorKind::input, "JAM subfields exceed format limit");
        }
        append16(bytes, field.low);
        append16(bytes, field.high);
        append32(bytes, static_cast<std::uint32_t>(field.data.size()));
        bytes.insert(bytes.end(), field.data.begin(), field.data.end());
    }
    return bytes;
}
auto crc32(std::string_view text) noexcept -> std::uint32_t {
    std::uint32_t value = no_crc;
    for (char letter : text) {
        value ^= static_cast<unsigned char>(ascii_lower(letter));
        for (unsigned int bit = 0; bit < CHAR_BIT; ++bit) {
            value = (value >> 1U) ^ ((value & 1U) != 0 ? crc_polynomial : 0U);
        }
    }
    return value;
}
auto bytes_from(std::string_view text) -> Bytes {
    Bytes bytes;
    bytes.reserve(text.size());
    for (char value : text) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    return bytes;
}
auto text_from(ByteView bytes) -> std::string {
    std::string text;
    text.reserve(bytes.size());
    for (auto value : bytes) {
        text.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
    }
    return text;
}
} // namespace jamnntpd::jam
