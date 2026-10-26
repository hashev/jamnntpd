#include "core/storage.hpp"
#include "temp_directory.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace jamnntpd;
namespace {
auto copy_base(const TempDirectory &directory) -> std::filesystem::path {
    for (auto extension : std::array{".jhr", ".jdt", ".jdx", ".jlr"}) {
        std::filesystem::copy_file(std::filesystem::path{JAM_FIXTURE_DIR} / "jam" /
                                       (std::string{"sample"} + extension),
                                   directory.file(std::string{"base"} + extension));
    }
    return directory.file("base");
}
auto contents(const TempDirectory &directory, std::string_view extension) -> std::string {
    std::ifstream input{directory.file(std::string{"base"} + std::string{extension}),
                        std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
auto message() -> jam::Message {
    return {.header = {.attributes = 0x01000001U},
            .fields = {{2, 0, jam::bytes_from("Alice")},
                       {3, 0, jam::bytes_from("All")},
                       {6, 0, jam::bytes_from("Test")}},
            .text = "New text\r"};
}
} // namespace
TEST_CASE("Storage opens existing four-file JAM base without native structure sizes") {
    TempDirectory directory;
    auto storage = jam::Storage::open(copy_base(directory));
    REQUIRE(storage);
    CHECK(storage->info()->slots == 2);
    CHECK(storage->info()->header.active == 1);
    auto article = storage->read(1);
    REQUIRE(article);
    CHECK(article->text == "Hello\r.World\r");
    CHECK(jam::text_from(article->fields.front().data) == "Alice");
    CHECK_FALSE(storage->read(0));
    CHECK_FALSE(storage->read(2));
    CHECK_FALSE(storage->read(3));
    CHECK_FALSE(jam::Storage::open(directory.file("missing")));
}
TEST_CASE("Storage appends replies and preserves original message bytes except thread links") {
    TempDirectory directory;
    const auto base = copy_base(directory);
    auto storage = jam::Storage::open(base);
    REQUIRE(storage);
    const auto old_header = contents(directory, ".jhr");
    auto article = message();
    auto number = storage->append(article);
    REQUIRE(number);
    CHECK(*number == 3);
    CHECK(storage->info()->slots == 3);
    CHECK(storage->info()->header.active == 2);
    CHECK(contents(directory, ".jhr").substr(jam::base_size, old_header.size() - jam::base_size) ==
          old_header.substr(jam::base_size));
    article.header.reply_to = 1;
    auto reply = storage->append(article);
    REQUIRE(reply);
    CHECK(*reply == 4);
    CHECK(storage->read(1)->header.first_reply == 4);
    auto second_reply = storage->append(article);
    REQUIRE(second_reply);
    CHECK(storage->read(4)->header.next_reply == *second_reply);
    CHECK(storage->read(*second_reply)->header.next_reply == 0);
    CHECK(storage->read(1)->header.first_reply == 4);
    REQUIRE(storage->cancel(3));
    CHECK_FALSE(storage->read(3));
    CHECK_FALSE(storage->cancel(3));
    CHECK(storage->info()->header.active == 3);
    article.header.reply_to = 999;
    CHECK_FALSE(storage->append(article));
    auto reopened = jam::Storage::open(base);
    REQUIRE(reopened);
    CHECK(reopened->read(4)->text == "New text\r");
}
TEST_CASE("Last-read uses sixteen-byte records and updates by user id") {
    TempDirectory directory;
    auto storage = jam::Storage::open(copy_base(directory));
    REQUIRE(storage);
    CHECK(storage->lastread(0xffffffffU)->high == 0x80000000U);
    CHECK_FALSE(storage->lastread(42));
    REQUIRE(storage->set_lastread({0x80000000U, 42, 3, 5}));
    CHECK(contents(directory, ".jlr").size() == 2 * jam::lastread_size);
    REQUIRE(storage->set_lastread({0x80000000U, 42, 8, 9}));
    CHECK(contents(directory, ".jlr").size() == 2 * jam::lastread_size);
    CHECK(storage->lastread(42)->last == 8);
    CHECK(storage->lastread(42)->high == 9);
}
TEST_CASE("Short writes and flush errors rollback every modified JAM file") {
    for (const auto event : {jam::IoEvent::write, jam::IoEvent::flush}) {
        for (std::size_t failure_index = 0; failure_index < 5; ++failure_index) {
            DYNAMIC_SECTION(static_cast<int>(event) << ":" << failure_index) {
                TempDirectory directory;
                const auto base = copy_base(directory);
                bool armed = false;
                std::size_t counter{};
                auto storage = jam::Storage::open(base, {.fail_io = [&](jam::IoEvent current) {
                                                      return armed && current == event &&
                                                             counter++ == failure_index;
                                                  }});
                REQUIRE(storage);
                std::vector<std::string> originals;
                for (auto extension : std::array{".jhr", ".jdt", ".jdx", ".jlr"}) {
                    originals.push_back(contents(directory, extension));
                }
                armed = true;
                auto article = message();
                article.header.reply_to = 1;
                auto posted = storage->append(article);
                const auto attempts = counter;
                armed = false;
                if (attempts > failure_index) {
                    CHECK_FALSE(posted);
                    std::size_t index{};
                    for (auto extension : std::array{".jhr", ".jdt", ".jdx", ".jlr"}) {
                        CHECK(contents(directory, extension) == originals.at(index++));
                    }
                    CHECK(storage->read(1)->header.first_reply == 0);
                } else {
                    REQUIRE(posted);
                }
            }
        }
    }
}
TEST_CASE("Permanent I/O failure disables the base instead of claiming successful rollback",
          "[quarantine]") {
    TempDirectory directory;
    const auto base = copy_base(directory);
    bool armed = false;
    auto storage = jam::Storage::open(base, {.fail_io = [&](jam::IoEvent event) {
                                          return armed && (event == jam::IoEvent::write ||
                                                           event == jam::IoEvent::resize);
                                      }});
    REQUIRE(storage);
    armed = true;
    auto posted = storage->append(message());
    REQUIRE_FALSE(posted);
    CHECK(posted.error().kind == ErrorKind::corrupt);
    armed = false;
    CHECK_FALSE(storage->append(message()));
    CHECK_FALSE(storage->info());
    CHECK_FALSE(jam::Storage::open(base));
}
TEST_CASE("Lock and read failures never mutate storage") {
    TempDirectory directory;
    bool armed = false;
    auto storage = jam::Storage::open(copy_base(directory), {.fail_io = [&](jam::IoEvent event) {
                                          return armed && (event == jam::IoEvent::lock ||
                                                           event == jam::IoEvent::read);
                                      }});
    REQUIRE(storage);
    auto previous = contents(directory, ".jhr");
    armed = true;
    CHECK_FALSE(storage->append(message()));
    armed = false;
    CHECK(contents(directory, ".jhr") == previous);
    std::stop_source stopped;
    stopped.request_stop();
    CHECK_FALSE(storage->info(stopped.get_token()));
    CHECK(storage->info());
}
TEST_CASE("Contended appends retain unique records across repeated worker starts") {
    TempDirectory directory;
    const auto base = copy_base(directory);
    auto first = jam::Storage::open(base);
    auto second = jam::Storage::open(base);
    REQUIRE(first);
    REQUIRE(second);
    std::atomic<bool> failed{false};
    for (int iteration = 0; iteration < 100; ++iteration) {
        std::barrier start{3};
        std::jthread writer_one{[&] {
            start.arrive_and_wait();
            if (!first->append(message())) {
                failed = true;
            }
        }};
        std::jthread writer_two{[&] {
            start.arrive_and_wait();
            if (!second->append(message())) {
                failed = true;
            }
        }};
        start.arrive_and_wait();
        writer_one.join();
        writer_two.join();
        REQUIRE_FALSE(failed.load());
        CHECK(first->info()->slots == static_cast<std::uint32_t>(4 + iteration * 2));
    }
    for (std::uint32_t number = 3; number <= 202; ++number) {
        const auto stored = first->read(number);
        REQUIRE(stored);
        CHECK(stored->header.number == number);
        CHECK(stored->text == "New text\r");
    }
}
TEST_CASE("Corrupt base extents are rejected before allocation") {
    TempDirectory directory;
    const auto base = copy_base(directory);
    auto storage = jam::Storage::open(base);
    REQUIRE(storage);
    auto header_bytes = contents(directory, ".jhr");
    header_bytes.at(1024 + 64) = '\xff';
    header_bytes.at(1024 + 65) = '\xff';
    header_bytes.at(1024 + 66) = '\xff';
    header_bytes.at(1024 + 67) = '\xff';
    directory.write("base.jhr", header_bytes);
    CHECK_FALSE(storage->read(1));
    directory.write("base.jdx", "truncated");
    CHECK_FALSE(storage->info());
}

TEST_CASE("Registry validates complete file identity through hard-link aliases") {
    TempDirectory directory;
    const auto base = copy_base(directory);
    auto original = jam::Storage::open(base);
    REQUIRE(original);
    for (auto extension : std::array{".jhr", ".jdt", ".jdx", ".jlr"}) {
        std::filesystem::create_hard_link(directory.file(std::string{"base"} + extension),
                                          directory.file(std::string{"alias"} + extension));
    }
    auto alias = jam::Storage::open(directory.file("alias"));
    REQUIRE(alias);
    auto appended = alias->append(message());
    REQUIRE(appended);
    CHECK(original->read(*appended)->text == "New text\r");
    std::filesystem::remove(directory.file("alias.jdt"));
    directory.write("alias.jdt", "different physical text file");
    CHECK_FALSE(jam::Storage::open(directory.file("alias")));
}
TEST_CASE("Partially opened storage releases every previously acquired file") {
    for (auto missing : std::array{".jdt", ".jdx", ".jlr"}) {
        TempDirectory directory;
        const auto base = copy_base(directory);
        const auto path = directory.file(std::string{"base"} + missing);
        std::filesystem::remove(path);
        CHECK_FALSE(jam::Storage::open(base));
        std::filesystem::copy_file(std::filesystem::path{JAM_FIXTURE_DIR} / "jam" /
                                       (std::string{"sample"} + missing),
                                   path);
        CHECK(jam::Storage::open(base));
    }
}

TEST_CASE("The last representable JAM article number is accepted without wrapping") {
    TempDirectory directory;
    const auto base = directory.file("base");
    directory.write("base.jhr", jam::text_from(jam::encode_base({.first_number = 0xffffffffU})));
    directory.write("base.jdt", "");
    directory.write("base.jdx", "");
    directory.write("base.jlr", "");
    const auto storage = jam::Storage::open(base);
    REQUIRE(storage);
    const auto posted = storage->append(message());
    REQUIRE(posted);
    CHECK(*posted == 0xffffffffU);
    CHECK(storage->read(*posted)->header.number == *posted);
    CHECK_FALSE(storage->append(message()));
}
TEST_CASE("Unexpected allocation failures rollback mutations before propagating") {
    TempDirectory directory;
    const auto base = copy_base(directory);
    bool armed = false;
    unsigned int writes{};
    const auto storage =
        jam::Storage::open(base, {.fail_io = [&](jam::IoEvent event) -> bool {
                               if (armed && event == jam::IoEvent::write && writes++ == 1) {
                                   throw std::bad_alloc{};
                               }
                               return false;
                           }});
    REQUIRE(storage);
    const auto old_header = contents(directory, ".jhr");
    const auto old_text = contents(directory, ".jdt");
    armed = true;
    CHECK_THROWS_AS(storage->append(message()), std::bad_alloc);
    armed = false;
    CHECK(contents(directory, ".jhr") == old_header);
    CHECK(contents(directory, ".jdt") == old_text);
    CHECK(storage->info()->slots == 2);
}

TEST_CASE("Cancellation and lastread failures restore every original record") {
    for (const bool cancelling : {false, true}) {
        for (const auto event : {jam::IoEvent::write, jam::IoEvent::flush}) {
            for (std::size_t failing = 0; failing < 5; ++failing) {
                TempDirectory directory;
                const auto base = copy_base(directory);
                bool armed = false;
                std::size_t counter{};
                const auto storage =
                    jam::Storage::open(base, {.fail_io = [&](jam::IoEvent current) {
                                           return armed && current == event && counter++ == failing;
                                       }});
                REQUIRE(storage);
                std::uint32_t cancel_number = 1;
                if (cancelling) {
                    auto reply = message();
                    reply.header.reply_to = 1;
                    const auto posted = storage->append(reply);
                    REQUIRE(posted);
                    cancel_number = *posted;
                }
                std::vector<std::string> originals;
                for (auto extension : std::array{".jhr", ".jdt", ".jdx", ".jlr"}) {
                    originals.push_back(contents(directory, extension));
                }
                armed = true;
                const auto changed = cancelling ? storage->cancel(cancel_number)
                                                : storage->set_lastread({0, 42, 1, 1});
                const auto attempts = counter;
                armed = false;
                if (attempts > failing) {
                    CHECK_FALSE(changed);
                    std::size_t index{};
                    for (auto extension : std::array{".jhr", ".jdt", ".jdx", ".jlr"}) {
                        CHECK(contents(directory, extension) == originals.at(index++));
                    }
                } else {
                    REQUIRE(changed);
                }
            }
        }
    }
}

TEST_CASE("An unrecoverable base cannot be reopened after its last handle closes", "[quarantine]") {
    TempDirectory directory;
    const auto base = copy_base(directory);
    bool fail = false;
    {
        const auto storage = jam::Storage::open(base, {.fail_io = [&](jam::IoEvent event) {
                                                    return fail && (event == jam::IoEvent::write ||
                                                                    event == jam::IoEvent::resize);
                                                }});
        REQUIRE(storage);
        fail = true;
        CHECK_FALSE(storage->append(message()));
    }
    fail = false;
    CHECK_FALSE(jam::Storage::open(base));
}

TEST_CASE("Cancelling first middle and last replies preserves chronological forward links") {
    TempDirectory directory;
    const auto storage = jam::Storage::open(copy_base(directory));
    REQUIRE(storage);
    auto reply = message();
    reply.header.reply_to = 1;
    REQUIRE(storage->append(reply) == 3);
    REQUIRE(storage->append(reply) == 4);
    REQUIRE(storage->append(reply) == 5);
    REQUIRE(storage->cancel(4));
    CHECK(storage->read(1)->header.first_reply == 3);
    CHECK(storage->read(3)->header.next_reply == 5);
    REQUIRE(storage->append(reply) == 6);
    CHECK(storage->read(5)->header.next_reply == 6);
    REQUIRE(storage->cancel(3));
    CHECK(storage->read(1)->header.first_reply == 5);
    REQUIRE(storage->cancel(5));
    REQUIRE(storage->cancel(6));
    CHECK(storage->read(1)->header.first_reply == 0);
}
TEST_CASE("Reply cycles are rejected without publishing a new record") {
    TempDirectory directory;
    const auto storage = jam::Storage::open(copy_base(directory));
    REQUIRE(storage);
    auto reply = message();
    reply.header.reply_to = 1;
    REQUIRE(storage->append(reply) == 3);
    const auto indices = jam::bytes_from(contents(directory, ".jdx"));
    const auto index =
        jam::decode_index(jam::ByteView{indices}.subspan(2 * jam::index_size, jam::index_size));
    REQUIRE(index);
    auto bytes = contents(directory, ".jhr");
    auto header = storage->read(3)->header;
    header.next_reply = 3;
    bytes.replace(index->header_offset, jam::message_size,
                  jam::text_from(jam::encode_message(header)));
    directory.write("base.jhr", bytes);
    CHECK_FALSE(storage->append(reply));
    CHECK(storage->info()->slots == 3);
    CHECK(contents(directory, ".jhr") == bytes);
}

TEST_CASE("Opening an existing JAM base observes cancellation before file and record waits") {
    TempDirectory directory;
    const auto base = copy_base(directory);
    std::stop_source stop;
    stop.request_stop();
    const auto storage = jam::Storage::open(base, {}, stop.get_token());
    REQUIRE_FALSE(storage);
    CHECK(storage.error().kind == ErrorKind::stopped);
}
TEST_CASE("Cancellation rechecks every authorized message byte while holding the base lock") {
    TempDirectory directory;
    const auto storage = jam::Storage::open(copy_base(directory));
    REQUIRE(storage);
    auto snapshot = storage->read(1);
    REQUIRE(snapshot);
    auto changed_header = *snapshot;
    ++changed_header.header.times_read;
    CHECK_FALSE(storage->cancel_checked(1, changed_header));
    auto changed_field = *snapshot;
    changed_field.fields.front().data.push_back(std::byte{1});
    CHECK_FALSE(storage->cancel_checked(1, changed_field));
    auto changed_body = *snapshot;
    changed_body.text += "changed";
    CHECK_FALSE(storage->cancel_checked(1, changed_body));
    CHECK(storage->read(1));
    CHECK(storage->cancel_checked(1, *snapshot));
    CHECK_FALSE(storage->read(1));
}
TEST_CASE("Every injected read failure preserves all JAM files across public operations") {
    for (int operation = 0; operation < 6; ++operation) {
        for (std::size_t failure_index = 0; failure_index < 32; ++failure_index) {
            TempDirectory directory;
            const auto base = copy_base(directory);
            bool armed = false;
            std::size_t attempts = 0;
            const auto storage = jam::Storage::open(
                base, {.fail_io = [&](jam::IoEvent event) -> bool {
                    return armed && event == jam::IoEvent::read && attempts++ == failure_index;
                }});
            REQUIRE(storage);
            std::vector<std::string> originals;
            for (const auto *extension : {".jhr", ".jdt", ".jdx", ".jlr"}) {
                originals.push_back(contents(directory, extension));
            }
            armed = true;
            bool succeeded = false;
            switch (operation) {
            case 0:
                succeeded = storage->info().has_value();
                break;
            case 1:
                succeeded = storage->read(1).has_value();
                break;
            case 2: {
                auto article = message();
                article.header.reply_to = 1;
                succeeded = storage->append(article).has_value();
                break;
            }
            case 3:
                succeeded = storage->cancel(1).has_value();
                break;
            case 4:
                succeeded = storage->lastread(0xffffffffU).has_value();
                break;
            case 5:
                succeeded = storage->set_lastread({0, 0xffffffffU, 2, 3}).has_value();
                break;
            default:
                FAIL("Unknown operation");
            }
            armed = false;
            if (attempts <= failure_index) {
                REQUIRE(succeeded);
                break;
            }
            INFO("operation " << operation << " read " << failure_index);
            CHECK_FALSE(succeeded);
            std::size_t index = 0;
            for (const auto *extension : {".jhr", ".jdt", ".jdx", ".jlr"}) {
                CHECK(contents(directory, extension) == originals.at(index++));
            }
            REQUIRE(storage->read(1));
        }
    }
}
TEST_CASE("Storage rejects invalid publication headers and corrupt companion extents") {
    TempDirectory directory;
    const auto base = copy_base(directory);
    const auto storage = jam::Storage::open(base);
    REQUIRE(storage);
    for (int kind = 0; kind < 3; ++kind) {
        auto article = message();
        if (kind == 0)
            article.header.revision = 2;
        if (kind == 1)
            article.header.first_reply = 1;
        if (kind == 2)
            article.header.next_reply = 1;
        CHECK_FALSE(storage->append(article));
    }
    auto deleted = message();
    deleted.header.attributes |= 0x80000000U;
    REQUIRE(storage->append(deleted));
    CHECK(storage->info()->header.active == 1);
    CHECK_FALSE(storage->read(3));
    std::stop_source stopped;
    stopped.request_stop();
    CHECK_FALSE(storage->read(1, stopped.get_token()));
    CHECK_FALSE(storage->append(message(), stopped.get_token()));
    CHECK_FALSE(storage->cancel(1, stopped.get_token()));
    CHECK_FALSE(storage->lastread(1, stopped.get_token()));
    CHECK_FALSE(storage->set_lastread({}, stopped.get_token()));
    directory.write("base.jlr", "truncated");
    CHECK_FALSE(storage->lastread(1));
    CHECK_FALSE(storage->set_lastread({}));
    auto bytes = contents(directory, ".jhr");
    bytes.at(12) = 0;
    bytes.at(13) = 0;
    bytes.at(14) = 0;
    bytes.at(15) = 0;
    directory.write("base.jhr", bytes);
    CHECK_FALSE(storage->cancel(1));
}
TEST_CASE("Independent bases keep concurrent subfields and long receivers value owned") {
    TempDirectory first_directory;
    TempDirectory second_directory;
    const auto first = jam::Storage::open(copy_base(first_directory));
    const auto second = jam::Storage::open(copy_base(second_directory));
    REQUIRE(first);
    REQUIRE(second);
    std::barrier ready{2};
    std::atomic<bool> successful{true};
    const auto append = [&](const jam::Storage &storage, std::string marker) -> void {
        ready.arrive_and_wait();
        for (int index = 0; index < 20; ++index) {
            auto article = message();
            article.fields.at(1).data = jam::bytes_from(marker);
            article.text = marker;
            if (!storage.append(article))
                successful = false;
        }
    };
    std::jthread left{[&]() -> void { append(*first, "First base"); }};
    std::jthread right{[&]() -> void { append(*second, "Second base"); }};
    left.join();
    right.join();
    REQUIRE(successful.load());
    for (std::uint32_t number = 3; number < 23; ++number) {
        const auto a = first->read(number);
        const auto b = second->read(number);
        REQUIRE(a);
        REQUIRE(b);
        CHECK(a->text == "First base");
        CHECK(b->text == "Second base");
        CHECK(jam::text_from(a->fields.at(1).data) == "First base");
        CHECK(jam::text_from(b->fields.at(1).data) == "Second base");
    }
    const std::string receiver(32000, 'R');
    auto article = message();
    article.fields.at(1).data = jam::bytes_from(receiver);
    const auto number = first->append(article);
    REQUIRE(number);
    const auto stored = first->read(*number);
    REQUIRE(stored);
    CHECK(jam::text_from(stored->fields.at(1).data) == receiver);
}

TEST_CASE("Append refuses dangling published extents before changing any companion file") {
    for (const auto *extension : {".jdt", ".jhr"}) {
        for (const bool partial : {false, true}) {
            TempDirectory directory;
            const auto base = copy_base(directory);
            const auto storage = jam::Storage::open(base);
            REQUIRE(storage);
            const auto path = directory.file(std::string{"base"} + extension);
            const auto length = std::filesystem::file_size(path);
            std::filesystem::resize_file(
                path, partial ? length - 1
                              : (std::string_view{extension} == ".jhr" ? jam::base_size : 0));
            std::vector<std::string> before;
            for (const auto *suffix : {".jhr", ".jdt", ".jdx", ".jlr"}) {
                before.push_back(contents(directory, suffix));
            }
            CHECK_FALSE(storage->read(1));
            const auto appended = storage->append(message());
            REQUIRE_FALSE(appended);
            CHECK(appended.error().kind == ErrorKind::corrupt);
            std::size_t index = 0;
            for (const auto *suffix : {".jhr", ".jdt", ".jdx", ".jlr"}) {
                CHECK(contents(directory, suffix) == before.at(index++));
            }
            CHECK_FALSE(storage->read(1));
        }
    }
}

TEST_CASE("Append accepts empty and deleted-only bases without reviving removed articles") {
    for (const bool empty : {false, true}) {
        TempDirectory directory;
        const auto base = copy_base(directory);
        if (empty) {
            directory.write("base.jhr", jam::text_from(jam::encode_base(jam::BaseHeader{})));
            std::filesystem::resize_file(directory.file("base.jdx"), 0);
            std::filesystem::resize_file(directory.file("base.jdt"), 0);
        }
        const auto storage = jam::Storage::open(base);
        REQUIRE(storage);
        if (!empty) {
            REQUIRE(storage->cancel(1));
            std::filesystem::resize_file(directory.file("base.jdt"), 0);
        }
        const auto expected = empty ? 1U : 3U;
        REQUIRE(storage->append(message()) == expected);
        if (!empty) {
            CHECK_FALSE(storage->read(1));
            CHECK_FALSE(storage->read(2));
        }
        CHECK(storage->read(expected)->text == "New text\r");
        CHECK(storage->info()->header.active == 1);
    }
}

TEST_CASE("Published extent validation observes stop between records before any mutation") {
    TempDirectory directory;
    const auto base = copy_base(directory);
    bool armed = false;
    std::size_t reads{};
    std::stop_source stop;
    const auto storage =
        jam::Storage::open(base, {.fail_io = [&](jam::IoEvent event) -> bool {
                               if (armed && event == jam::IoEvent::read && ++reads == 3) {
                                   stop.request_stop();
                               }
                               return false;
                           }});
    REQUIRE(storage);
    std::vector<std::string> before;
    for (const auto *suffix : {".jhr", ".jdt", ".jdx", ".jlr"}) {
        before.push_back(contents(directory, suffix));
    }
    armed = true;
    const auto result = storage->append(message(), stop.get_token());
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == ErrorKind::stopped);
    std::size_t index = 0;
    for (const auto *suffix : {".jhr", ".jdt", ".jdx", ".jlr"}) {
        CHECK(contents(directory, suffix) == before.at(index++));
    }
}
