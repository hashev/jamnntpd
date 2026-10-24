#ifndef JAMNNTPD_CORE_STORAGE_HPP
#define JAMNNTPD_CORE_STORAGE_HPP

#include "jam_format.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace jamnntpd::jam {
enum class IoEvent : std::uint8_t { read, write, flush, resize, lock };
inline constexpr auto default_lock_timeout = std::chrono::seconds{5};
struct StorageOptions {
    std::chrono::milliseconds lock_timeout{default_lock_timeout};
    // Optional deterministic failure seam; absent in normal application operation.
    std::function<bool(IoEvent)> fail_io;
};
struct BaseInfo {
    BaseHeader header;
    std::uint32_t slots{};
};
struct Message {
    MessageHeader header;
    std::vector<Subfield> fields;
    std::string text;
};
struct BaseState;
class Storage {
  public:
    [[nodiscard]] static auto open(const std::filesystem::path &base,
                                   StorageOptions options = StorageOptions(),
                                   const std::stop_token &stop = {}) -> Result<Storage>;
    [[nodiscard]] auto info(const std::stop_token &stop = {}) const -> Result<BaseInfo>;
    [[nodiscard]] auto read(std::uint32_t number, const std::stop_token &stop = {}) const
        -> Result<Message>;
    [[nodiscard]] auto append(Message message, const std::stop_token &stop = {}) const
        -> Result<std::uint32_t>;
    [[nodiscard]] auto cancel(std::uint32_t number, const std::stop_token &stop = {}) const
        -> Result<void>;
    [[nodiscard]] auto cancel_checked(std::uint32_t number, const Message &authorized_snapshot,
                                      const std::stop_token &stop = {}) const -> Result<void>;
    [[nodiscard]] auto lastread(std::uint32_t user, const std::stop_token &stop = {}) const
        -> Result<LastRead>;
    [[nodiscard]] auto set_lastread(LastRead record, const std::stop_token &stop = {}) const
        -> Result<void>;

  private:
    [[nodiscard]] auto cancel_impl(std::uint32_t number,
                                   std::optional<std::reference_wrapper<const Message>> snapshot,
                                   const std::stop_token &stop) const -> Result<void>;
    explicit Storage(std::shared_ptr<BaseState> state) : state_(std::move(state)) {}
    std::shared_ptr<BaseState> state_;
};
} // namespace jamnntpd::jam
#endif
