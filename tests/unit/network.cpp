#include "core/network.hpp"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <thread>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
using namespace jamnntpd;

TEST_CASE("Write-all handles deterministic short writes and refuses zero progress") {
    const std::string input{"A\0B\r\n.C\r\n", 10};
    std::string output;
    REQUIRE(write_all(input, [&](std::string_view remaining) -> Result<std::size_t> {
        const auto count = std::min<std::size_t>(2, remaining.size());
        output.append(remaining.substr(0, count));
        return count;
    }));
    CHECK(output == input);
    CHECK_FALSE(write_all("hello", [](std::string_view) -> Result<std::size_t> { return 0; }));
    CHECK_FALSE(write_all("hi", [](std::string_view) -> Result<std::size_t> { return 3; }));
    CHECK_FALSE(write_all("hi", [](std::string_view) -> Result<std::size_t> {
        return failure(ErrorKind::io, "injected");
    }));
    std::stop_source stopped;
    stopped.request_stop();
    CHECK_FALSE(write_all(
        "hi", [](std::string_view) -> Result<std::size_t> { return 1; }, stopped.get_token()));
}
TEST_CASE("Socket ownership and cancellation release native resources") {
    auto runtime = NetworkRuntime::create();
    REQUIRE(runtime);
    auto listener = Socket::listen(0);
    REQUIRE(listener);
    const auto port = listener->local_port();
    REQUIRE(port);
    CHECK(*port != 0);
    auto duplicate = Socket::listen(*port);
    CHECK_FALSE(duplicate);
    CHECK_FALSE(Socket::adopt(invalid_socket));
    CHECK_FALSE(Socket::adopt(static_cast<NativeSocket>(invalid_socket - 1)));
    CHECK_FALSE(listener->peer());
    std::stop_source stopped;
    stopped.request_stop();
    CHECK_FALSE(listener->accept(stopped.get_token()));
    auto moved = std::move(*listener);
    CHECK(listener->native_handle() == invalid_socket);
    const auto moved_port = moved.local_port();
    REQUIRE(moved_port);
    CHECK(*moved_port == *port);
    moved.close();
    moved.close();
    CHECK_FALSE(moved.local_port());
}
TEST_CASE("Fragmented input preserves framing and closes oversized sessions") {
    auto runtime = NetworkRuntime::create();
    REQUIRE(runtime);
    auto listener = Socket::listen(0);
    REQUIRE(listener);
    const auto port = listener->local_port();
    REQUIRE(port);
    auto raw = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(raw != invalid_socket);
    // The native connect is restricted to a local test listener; adopt immediately afterwards.
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(*port);
    const auto connected =
        ::connect(raw, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
    auto client = Socket::adopt(raw);
    REQUIRE(client);
    REQUIRE(connected == 0);
    auto server = listener->accept();
    REQUIRE(server);
    CHECK(server->peer()->address == "127.0.0.1");
    REQUIRE(client->send("GR"));
    REQUIRE(client->send("OUP\r\nNEXT\r\n"));
    LineReader reader{*server};
    CHECK(reader.line(20) == "GROUP\r\n");
    CHECK(reader.line(20) == "NEXT\r\n");
    REQUIRE(client->send("toolongcommand\r\nNEXT\r\n"));
    CHECK_FALSE(reader.line(8));
    CHECK(server->native_handle() == invalid_socket);
    CHECK_FALSE(server->peer());
}
TEST_CASE("A waiting read observes stop without waiting for the idle timeout") {
    auto runtime = NetworkRuntime::create();
    REQUIRE(runtime);
    auto listener = Socket::listen(0);
    REQUIRE(listener);
    std::stop_source stop;
    std::atomic<bool> finished{false};
    std::jthread waiter{[&] {
        auto accepted = listener->accept(stop.get_token());
        finished = !accepted && accepted.error().kind == ErrorKind::stopped;
    }};
    stop.request_stop();
    waiter.join();
    CHECK(finished.load());
}

TEST_CASE("Closed sockets fail promptly and native accept waits honor explicit deadlines") {
    auto runtime = NetworkRuntime::create();
    REQUIRE(runtime);
    auto listener = Socket::listen(0);
    REQUIRE(listener);
    const auto result = listener->accept({}, std::chrono::milliseconds{5});
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == ErrorKind::timeout);
    listener->close();
    CHECK_FALSE(listener->accept());
    CHECK_FALSE(listener->receive());
    CHECK_FALSE(listener->send("x"));
}

TEST_CASE("Backpressured native writes time out and observe stop after making progress") {
    auto runtime = NetworkRuntime::create();
    REQUIRE(runtime);
    const std::string payload(32U * 1024U * 1024U, 'x');
    for (const int mode : {0, 1, 2}) {
        auto listener = Socket::listen(0);
        REQUIRE(listener);
        auto client = Socket::adopt(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        REQUIRE(client);
        const int receive_buffer = 1024;
#ifdef _WIN32
        REQUIRE(setsockopt(client->native_handle(), SOL_SOCKET, SO_RCVBUF,
                           reinterpret_cast<const char *>(&receive_buffer),
                           sizeof(receive_buffer)) == 0);
#else
        REQUIRE(setsockopt(client->native_handle(), SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                           sizeof(receive_buffer)) == 0);
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const auto port = listener->local_port();
        REQUIRE(port);
        address.sin_port = htons(*port);
        // Nonblocking connect completion is established by accepting the local peer below.
        static_cast<void>(::connect(client->native_handle(),
                                    reinterpret_cast<const sockaddr *>(&address), sizeof(address)));
        auto server = listener->accept({}, std::chrono::seconds{2});
        REQUIRE(server);
        // Bound both queues: Winsock can otherwise buffer this entire payload locally.
        const int send_buffer = 1024;
#ifdef _WIN32
        REQUIRE(setsockopt(server->native_handle(), SOL_SOCKET, SO_SNDBUF,
                           reinterpret_cast<const char *>(&send_buffer), sizeof(send_buffer)) == 0);
#else
        REQUIRE(setsockopt(server->native_handle(), SOL_SOCKET, SO_SNDBUF, &send_buffer,
                           sizeof(send_buffer)) == 0);
#endif
        if (mode != 2) {
            // Establish actual backpressure instead of assuming one large send must block.
            const auto chunk = std::string_view{payload}.substr(0, 64U * 1024U);
            Result<void> primed;
            std::size_t primed_bytes{};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (primed_bytes < 64U * 1024U * 1024U &&
                   std::chrono::steady_clock::now() < deadline) {
                primed = server->send(chunk, {}, std::chrono::milliseconds{20});
                if (!primed) {
                    break;
                }
                primed_bytes += chunk.size();
            }
            INFO("Primed bytes: " << primed_bytes);
            REQUIRE_FALSE(primed);
            REQUIRE(primed.error().kind == ErrorKind::timeout);
        }
        if (mode == 1) {
            std::stop_source stop;
            Result<void> written;
            std::jthread writer{[&] {
                written = write_all(
                    payload,
                    [&](std::string_view remaining) -> Result<std::size_t> {
                        const auto part = remaining.substr(0, 4096);
                        auto result = server->send(part, stop.get_token(), std::chrono::seconds{2});
                        if (!result) {
                            return std::unexpected{result.error()};
                        }
                        return part.size();
                    },
                    stop.get_token());
            }};
            const auto progress = client->receive({}, std::chrono::seconds{2});
            REQUIRE(progress);
            CHECK_FALSE(progress->empty());
            const auto beginning = std::chrono::steady_clock::now();
            stop.request_stop();
            writer.join();
            REQUIRE_FALSE(written);
            CHECK(written.error().kind == ErrorKind::stopped);
            CHECK(std::chrono::steady_clock::now() - beginning < std::chrono::seconds{1});
        } else if (mode == 2) {
            const auto small = std::string_view{payload}.substr(0, 64U * 1024U);
            Result<void> written;
            std::jthread writer{
                [&] { written = server->send(small, {}, std::chrono::seconds{2}); }};
            std::string received;
            while (received.size() < small.size()) {
                auto chunk = client->receive({}, std::chrono::seconds{2});
                REQUIRE(chunk);
                received += *chunk;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            writer.join();
            REQUIRE(written);
            CHECK(received == small);
        } else {
            const auto beginning = std::chrono::steady_clock::now();
            const auto result = server->send(payload, {}, std::chrono::milliseconds{20});
            REQUIRE_FALSE(result);
            CHECK(result.error().kind == ErrorKind::timeout);
            CHECK(std::chrono::steady_clock::now() - beginning < std::chrono::seconds{2});
        }
    }
}
