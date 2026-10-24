#ifndef JAMNNTPD_CORE_JAM_FORMAT_HPP
#define JAMNNTPD_CORE_JAM_FORMAT_HPP

// JAM format implementation, derived from JAMLIB by Bjorn Stenberg (1999)
// and structrw/CRC32 by Johan Billing (1999-2003). LGPL-2.1-or-later.
// Original notices and full license are retained in src/jamlib/.
#include "error.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <string>

namespace jamnntpd::jam {
using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;
inline constexpr std::size_t base_size = 1024;
inline constexpr std::size_t message_size = 76;
inline constexpr std::size_t index_size = 8;
inline constexpr std::size_t lastread_size = 16;
inline constexpr std::uint32_t no_crc = 0xffffffffU;
inline constexpr std::uint32_t deleted = 0x80000000U;

inline constexpr std::size_t base_reserved_size = 1000;
struct BaseHeader {
    std::uint32_t created{};
    std::uint32_t modifications{};
    std::uint32_t active{};
    std::uint32_t password_crc{no_crc};
    std::uint32_t first_number{1};
    std::array<std::byte, base_reserved_size> reserved{};
};

struct MessageHeader {
    std::uint16_t revision{1};
    std::uint16_t reserved{};
    std::uint32_t subfield_length{};
    std::uint32_t times_read{};
    std::uint32_t message_crc{no_crc};
    std::uint32_t reply_crc{no_crc};
    std::uint32_t reply_to{};
    std::uint32_t first_reply{};
    std::uint32_t next_reply{};
    std::uint32_t written{};
    std::uint32_t received{};
    std::uint32_t processed{};
    std::uint32_t number{};
    std::uint32_t attributes{};
    std::uint32_t attributes2{};
    std::uint32_t text_offset{};
    std::uint32_t text_length{};
    std::uint32_t password_crc{no_crc};
    std::uint32_t cost{};
};

struct Index {
    std::uint32_t user_crc{no_crc};
    std::uint32_t header_offset{no_crc};
};
struct LastRead {
    std::uint32_t user_crc{};
    std::uint32_t user_id{};
    std::uint32_t last{};
    std::uint32_t high{};
};
struct Subfield {
    std::uint16_t low{};
    std::uint16_t high{};
    Bytes data;
};
[[nodiscard]] auto decode_base(ByteView bytes) -> Result<BaseHeader>;
[[nodiscard]] auto decode_message(ByteView bytes) -> Result<MessageHeader>;
[[nodiscard]] auto decode_index(ByteView bytes) -> Result<Index>;
[[nodiscard]] auto decode_lastread(ByteView bytes) -> Result<LastRead>;
[[nodiscard]] auto decode_subfields(ByteView bytes) -> Result<std::vector<Subfield>>;
[[nodiscard]] auto encode_base(const BaseHeader &header) -> Bytes;
[[nodiscard]] auto encode_message(const MessageHeader &header) -> Bytes;
[[nodiscard]] auto encode_index(Index index) -> Bytes;
[[nodiscard]] auto encode_lastread(LastRead record) -> Bytes;
[[nodiscard]] auto encode_subfields(std::span<const Subfield> fields) -> Result<Bytes>;
[[nodiscard]] auto crc32(std::string_view text) noexcept -> std::uint32_t;
[[nodiscard]] auto bytes_from(std::string_view text) -> Bytes;
[[nodiscard]] auto text_from(ByteView bytes) -> std::string;
} // namespace jamnntpd::jam

#endif
