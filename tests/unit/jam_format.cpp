#include "core/jam_format.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace jamnntpd;
namespace {
jam::Bytes fixture(std::string_view name) {
    std::ifstream input{std::filesystem::path{JAM_FIXTURE_DIR} / "jam" / name, std::ios::binary};
    REQUIRE(input.good());
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return jam::bytes_from(text);
}
} // namespace
TEST_CASE("JAM fixed width records match independent fixtures on LP64") {
    const auto base_bytes = fixture("base-header.bin");
    const auto base = jam::decode_base(base_bytes);
    REQUIRE(base);
    CHECK(base->created == 0x80000000U);
    CHECK(base->first_number == 1);
    CHECK(jam::encode_base(*base) == base_bytes);
    const auto message_bytes = fixture("message-header.bin");
    const auto message = jam::decode_message(message_bytes);
    REQUIRE(message);
    CHECK(message->message_crc == 0x80000000U);
    CHECK(message->password_crc == 0xffffffffU);
    CHECK(message->attributes == 0x01000001U);
    CHECK(jam::encode_message(*message) == message_bytes);
    const auto index_bytes = fixture("index.bin");
    const auto index = jam::decode_index(jam::ByteView{index_bytes}.first(8));
    REQUIRE(index);
    CHECK(index->header_offset == 1024);
    CHECK(jam::encode_index(*index) == jam::Bytes(index_bytes.begin(), index_bytes.begin() + 8));
    const auto lastread_bytes = fixture("lastread.bin");
    const auto lastread = jam::decode_lastread(lastread_bytes);
    REQUIRE(lastread);
    CHECK(lastread->high == 0x80000000U);
    CHECK(jam::encode_lastread(*lastread) == lastread_bytes);
}
TEST_CASE("JAM rejects invalid extents before accessing memory") {
    CHECK_FALSE(jam::decode_base(fixture("bad-signature.bin")));
    CHECK_FALSE(jam::decode_base({}));
    CHECK_FALSE(jam::decode_message(fixture("truncated-header.bin")));
    CHECK_FALSE(jam::decode_index(fixture("truncated-index.bin")));
    CHECK_FALSE(jam::decode_lastread({}));
    CHECK_FALSE(jam::decode_subfields(fixture("overflow-subfield.bin")));
    CHECK_FALSE(jam::decode_subfields(jam::Bytes(7)));
    auto invalid_revision = fixture("message-header.bin");
    invalid_revision[4] = std::byte{2};
    CHECK_FALSE(jam::decode_message(invalid_revision));
}
TEST_CASE("Subfield values own their data and preserve arbitrary binary bytes") {
    const auto bytes = fixture("subfields.bin");
    auto fields = jam::decode_subfields(bytes);
    REQUIRE(fields);
    REQUIRE(fields->size() == 4);
    CHECK(jam::text_from((*fields)[0].data) == "Alice");
    CHECK(jam::encode_subfields(*fields) == bytes);
    fields->push_back(jam::Subfield{0xffff, 0xffff, {std::byte{0}, std::byte{255}}});
    const auto encoded = jam::encode_subfields(*fields);
    REQUIRE(encoded);
    const auto decoded = jam::decode_subfields(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->back().data == fields->back().data);
    CHECK(jam::decode_subfields({})->empty());
}
TEST_CASE("JAM CRC matches known vectors without signed overflow") {
    CHECK(jam::crc32("") == 0xffffffffU);
    CHECK(jam::crc32("123456789") == 0x340bc6d9U);
    CHECK(jam::crc32("ALICE") == jam::crc32("alice"));
    const std::string binary{"\0\xff\x80", 3};
    CHECK(jam::text_from(jam::bytes_from(binary)) == binary);
}
