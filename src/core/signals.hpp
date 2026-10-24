#ifndef JAMNNTPD_CORE_SIGNALS_HPP
#define JAMNNTPD_CORE_SIGNALS_HPP
#include "error.hpp"
#include <memory>
#include <stop_token>

namespace jamnntpd {
// Create on the control thread before starting workers; destroy there after joins.
class SignalStop {
  public:
    [[nodiscard]] static auto create(std::stop_source source) -> Result<SignalStop>;
    ~SignalStop();
    SignalStop(const SignalStop &) = delete;
    auto operator=(const SignalStop &) -> SignalStop & = delete;
    SignalStop(SignalStop &&) noexcept;
    auto operator=(SignalStop &&) -> SignalStop & = delete;

  private:
    struct Impl;
    explicit SignalStop(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace jamnntpd
#endif
