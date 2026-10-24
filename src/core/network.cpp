#include "network.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <limits>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace jamnntpd {
namespace {
auto socket_address(sockaddr_in &address) noexcept -> sockaddr * {
    // POSIX and Winsock specify a borrowed sockaddr pointer for this IPv4 object.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<sockaddr *>(&address);
}
auto socket_error() noexcept -> int {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
auto network_error(std::string_view message) -> std::unexpected<Error> {
    const auto code = socket_error();
    return failure(ErrorKind::io, std::string{message}, code);
}
auto retryable(int code) noexcept -> bool {
#ifdef _WIN32
    return code == WSAEWOULDBLOCK || code == WSAEINTR;
#else
    return code == EAGAIN || code == EWOULDBLOCK || code == EINTR;
#endif
}
auto wait_socket(NativeSocket socket, bool writing, const std::stop_token &stop,
                 std::chrono::milliseconds timeout) -> Result<void> {
    constexpr int poll_interval_ms = 100;
    if (socket == invalid_socket) {
        return failure(ErrorKind::io, "Socket is closed");
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!stop.stop_requested()) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            return failure(ErrorKind::timeout, "Socket timeout");
        }
        const int interval =
            static_cast<int>(std::min<std::int64_t>(poll_interval_ms, remaining.count()));
#ifdef _WIN32
        WSAPOLLFD descriptor{socket, static_cast<short>(writing ? POLLWRNORM : POLLRDNORM), 0};
        const auto ready = WSAPoll(&descriptor, 1, interval);
#else
        pollfd descriptor{
            .fd = socket,
            .events = static_cast<short>(writing ? POLLOUT : POLLIN),
            .revents = 0,
        };
        const auto ready = poll(&descriptor, 1, interval);
#endif
        if (ready > 0) {
            return {};
        }
        if (ready < 0) {
            const auto code = socket_error();
            if (!retryable(code)) {
                return failure(ErrorKind::io, "Socket wait failed", code);
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return failure(ErrorKind::timeout, "Socket timeout");
        }
    }
    return failure(ErrorKind::stopped, "Socket operation stopped");
}
auto configure_socket(NativeSocket socket) -> Result<void> {
#ifdef _WIN32
    u_long enabled = 1;
    if (ioctlsocket(socket, FIONBIO, &enabled) != 0) {
#else
    // F_GETFL/F_SETFL specify integer flag arguments, not formatted varargs.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const auto flags = fcntl(socket, F_GETFL, 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (flags == -1 || fcntl(socket, F_SETFL,
                             static_cast<int>(static_cast<unsigned int>(flags) |
                                              static_cast<unsigned int>(O_NONBLOCK))) == -1) {
#endif
        return network_error("Cannot configure nonblocking socket");
    }
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        return network_error("Cannot suppress socket SIGPIPE");
    }
#endif
    return {};
}
} // namespace

auto write_all(std::string_view text, const Writer &writer, const std::stop_token &stop)
    -> Result<void> {
    while (!text.empty()) {
        if (stop.stop_requested()) {
            return failure(ErrorKind::stopped, "Write stopped");
        }
        auto sent = writer(text);
        if (!sent) {
            return std::unexpected(sent.error());
        }
        if (*sent == 0 || *sent > text.size()) {
            return failure(ErrorKind::io, "Invalid short write result");
        }
        text.remove_prefix(*sent);
    }
    return {};
}
auto NetworkRuntime::create() -> Result<NetworkRuntime> {
#ifdef _WIN32
    WSADATA data{};
    const auto code = WSAStartup(MAKEWORD(2, 2), &data);
    if (code != 0) {
        return failure(ErrorKind::io, "Winsock initialization failed", code);
    }
#endif
    return NetworkRuntime{true};
}
NetworkRuntime::~NetworkRuntime() {
#ifdef _WIN32
    if (active_) {
        WSACleanup();
    }
#endif
}
NetworkRuntime::NetworkRuntime(NetworkRuntime &&other) noexcept
    : active_(std::exchange(other.active_, false)) {}
Socket::~Socket() { close(); }
Socket::Socket(Socket &&other) noexcept : socket_(std::exchange(other.socket_, invalid_socket)) {}
auto Socket::operator=(Socket &&other) noexcept -> Socket & {
    if (this != &other) {
        close();
        socket_ = std::exchange(other.socket_, invalid_socket);
    }
    return *this;
}
void Socket::close() noexcept {
    if (socket_ == invalid_socket) {
        return;
    }
#ifdef _WIN32
    closesocket(socket_);
#else
    ::close(socket_);
#endif
    socket_ = invalid_socket;
}
auto Socket::adopt(NativeSocket socket) -> Result<Socket> {
    Socket owner{socket};
    if (socket == invalid_socket) {
        return network_error("Cannot create socket");
    }
    const auto configured = configure_socket(socket);
    if (!configured) {
        return std::unexpected(configured.error());
    }
    return owner;
}
auto Socket::listen(std::uint16_t port) -> Result<Socket> {
    auto socket = adopt(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!socket) {
        return std::unexpected(socket.error());
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(socket->socket_, socket_address(address), sizeof(address)) != 0 ||
        ::listen(socket->socket_, SOMAXCONN) != 0) {
        return network_error("Cannot bind or listen on socket");
    }
    return socket;
}
auto Socket::accept(const std::stop_token &stop, std::chrono::milliseconds timeout) const
    -> Result<Socket> {
    while (!stop.stop_requested()) {
        auto ready = wait_socket(socket_, false, stop, timeout);
        if (!ready) {
            return std::unexpected(ready.error());
        }
        auto accepted = ::accept(socket_, nullptr, nullptr);
        if (accepted != invalid_socket) {
            return adopt(accepted);
        }
        const auto code = socket_error();
        if (!retryable(code)) {
            return failure(ErrorKind::io, "Socket accept failed", code);
        }
    }
    return failure(ErrorKind::stopped, "Accept stopped");
}
auto Socket::send(std::string_view text, const std::stop_token &stop,
                  std::chrono::milliseconds timeout) const -> Result<void> {
    return write_all(
        text,
        [&](std::string_view remaining) -> Result<std::size_t> {
            while (!stop.stop_requested()) {
                auto ready = wait_socket(socket_, true, stop, timeout);
                if (!ready) {
                    return std::unexpected(ready.error());
                }
                const auto count = static_cast<int>(
                    std::min<std::size_t>(remaining.size(), std::numeric_limits<int>::max()));
#ifdef MSG_NOSIGNAL
                constexpr int send_flags = MSG_NOSIGNAL;
#else
                constexpr int send_flags = 0;
#endif
#ifdef _WIN32
                const auto sent = ::send(socket_, remaining.data(), count, send_flags);
#else
                const auto sent =
                    ::send(socket_, remaining.data(), static_cast<std::size_t>(count), send_flags);
#endif
                if (sent > 0) {
                    return static_cast<std::size_t>(sent);
                }
                const auto code = socket_error();
                if (sent == 0 || !retryable(code)) {
                    return failure(ErrorKind::io, "Socket send failed", code);
                }
            }
            return failure(ErrorKind::stopped, "Send stopped");
        },
        stop);
}
auto Socket::receive(const std::stop_token &stop, std::chrono::milliseconds timeout) const
    -> Result<std::string> {
    constexpr std::size_t receive_size = 4096;
    std::array<char, receive_size> buffer{};
    while (!stop.stop_requested()) {
        auto ready = wait_socket(socket_, false, stop, timeout);
        if (!ready) {
            return std::unexpected(ready.error());
        }
#ifdef _WIN32
        const auto received = ::recv(socket_, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
        const auto received = ::recv(socket_, buffer.data(), buffer.size(), 0);
#endif
        if (received > 0) {
            return std::string{buffer.data(), static_cast<std::size_t>(received)};
        }
        const auto code = socket_error();
        if (received == 0) {
            return failure(ErrorKind::stopped, "Peer disconnected");
        }
        if (!retryable(code)) {
            return failure(ErrorKind::io, "Socket receive failed", code);
        }
    }
    return failure(ErrorKind::stopped, "Receive stopped");
}
auto Socket::local_port() const -> Result<std::uint16_t> {
    sockaddr_in address{};
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(socket_, socket_address(address), &length) != 0) {
        return network_error("Cannot read socket address");
    }
    return ntohs(address.sin_port);
}
auto Socket::peer() const -> Result<Peer> {
    sockaddr_in address{};
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getpeername(socket_, socket_address(address), &length) != 0) {
        return network_error("Cannot read peer address");
    }
    std::array<char, INET_ADDRSTRLEN> buffer{};
    if (inet_ntop(AF_INET, &address.sin_addr, buffer.data(), buffer.size()) == nullptr) {
        return network_error("Cannot format peer address");
    }
    return Peer{.address = std::string{buffer.data()}, .port = ntohs(address.sin_port)};
}
auto LineReader::line(std::size_t limit, const std::stop_token &stop,
                      std::chrono::milliseconds timeout) -> Result<std::string> {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const auto newline = pending_.find('\n');
        if ((newline == std::string::npos && pending_.size() >= limit) ||
            (newline != std::string::npos && newline + 1 > limit)) {
            socket_.get().close();
            return failure(ErrorKind::input, "Protocol line exceeds limit");
        }
        if (newline != std::string::npos) {
            auto result = pending_.substr(0, newline + 1);
            pending_.erase(0, newline + 1);
            return result;
        }
        auto input = socket_.get().receive(stop, std::chrono::ceil<std::chrono::milliseconds>(
                                                     deadline - std::chrono::steady_clock::now()));
        if (!input) {
            return std::unexpected(input.error());
        }
        pending_ += *input;
    }
}
} // namespace jamnntpd
