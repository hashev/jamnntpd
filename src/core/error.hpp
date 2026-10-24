#ifndef JAMNNTPD_CORE_ERROR_HPP
#define JAMNNTPD_CORE_ERROR_HPP

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

namespace jamnntpd {
enum class ErrorKind : std::uint8_t { input, encoding, io, corrupt, permission, timeout, stopped };

struct Error {
    ErrorKind kind;
    std::string message;
    int native_code{};
};

template <class Value> using Result = std::expected<Value, Error>;

inline auto failure(ErrorKind kind, std::string message, int native_code = 0) {
    return std::unexpected(
        Error{.kind = kind, .message = std::move(message), .native_code = native_code});
}
} // namespace jamnntpd

#endif
