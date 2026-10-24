#ifndef JAMNNTPD_CORE_NETWORK_HPP
#define JAMNNTPD_CORE_NETWORK_HPP

#include "error.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

namespace jamnntpd {
#ifdef _WIN32
using NativeSocket = SOCKET;
inline constexpr NativeSocket invalid_socket = INVALID_SOCKET;
#else
using NativeSocket = int;
inline constexpr NativeSocket invalid_socket = -1;
#endif
inline constexpr auto default_socket_timeout = std::chrono::minutes{5};
using Writer = std::function<Result<std::size_t>(std::string_view)>;
[[nodiscard]] auto write_all(std::string_view text, const Writer &writer,
                             const std::stop_token &stop = {}) -> Result<void>;
class NetworkRuntime {
  public:
    [[nodiscard]] static auto create() -> Result<NetworkRuntime>;
    ~NetworkRuntime();
    NetworkRuntime(const NetworkRuntime &) = delete;
    auto operator=(const NetworkRuntime &) -> NetworkRuntime & = delete;
    NetworkRuntime(NetworkRuntime &&other) noexcept;
    auto operator=(NetworkRuntime &&) -> NetworkRuntime & = delete;

  private:
    explicit NetworkRuntime(bool active) : active_(active) {}
    bool active_{};
};
struct Peer {
    std::string address;
    std::uint16_t port{};
};
class Socket {
  public:
    [[nodiscard]] static auto adopt(NativeSocket socket) -> Result<Socket>;
    [[nodiscard]] static auto listen(std::uint16_t port) -> Result<Socket>;
    ~Socket();
    Socket(const Socket &) = delete;
    auto operator=(const Socket &) -> Socket & = delete;
    Socket(Socket &&other) noexcept;
    auto operator=(Socket &&other) noexcept -> Socket &;
    [[nodiscard]] auto accept(const std::stop_token &stop = {},
                              std::chrono::milliseconds timeout = default_socket_timeout) const
        -> Result<Socket>;
    [[nodiscard]] auto send(std::string_view text, const std::stop_token &stop = {},
                            std::chrono::milliseconds timeout = default_socket_timeout) const
        -> Result<void>;
    [[nodiscard]] auto receive(const std::stop_token &stop = {},
                               std::chrono::milliseconds timeout = default_socket_timeout) const
        -> Result<std::string>;
    [[nodiscard]] auto local_port() const -> Result<std::uint16_t>;
    [[nodiscard]] auto peer() const -> Result<Peer>;
    [[nodiscard]] auto native_handle() const noexcept -> NativeSocket { return socket_; }
    void close() noexcept;

  private:
    explicit Socket(NativeSocket socket) : socket_(socket) {}
    NativeSocket socket_{invalid_socket};
};
class LineReader {
  public:
    explicit LineReader(Socket &socket) : socket_(socket) {}
    [[nodiscard]] auto line(std::size_t limit, const std::stop_token &stop = {},
                            std::chrono::milliseconds timeout = default_socket_timeout)
        -> Result<std::string>;

  private:
    std::reference_wrapper<Socket> socket_;
    std::string pending_;
};
} // namespace jamnntpd
#endif
