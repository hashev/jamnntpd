#ifndef JAMNNTPD_CORE_BACKEND_HPP
#define JAMNNTPD_CORE_BACKEND_HPP
#include "ftn.hpp"
#include "mime.hpp"
#include "post_builder.hpp"

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace jamnntpd {
class JamBackend {
  public:
    JamBackend(Config config, CharsetConfig charsets, std::vector<Group> groups,
               std::function<void(std::string_view)> report = {});
    [[nodiscard]] auto services() -> SessionServices;
    [[nodiscard]] auto index(const Group &group, const Identity &identity,
                             const std::stop_token &stop) const -> Result<GroupIndex>;
    [[nodiscard]] auto article(const Group &group, std::uint32_t number, const Identity &identity,
                               const UserOptions &options, const std::stop_token &stop) const
        -> Result<ProtocolArticle>;
    [[nodiscard]] auto submit(std::string_view article, const Identity &identity,
                              const UserOptions &options, const std::stop_token &stop) const
        -> Result<std::string>;

  private:
    [[nodiscard]] auto parent(std::string_view identifier, const Group &target,
                              const Identity &identity, const std::stop_token &stop) const
        -> Result<ReplyTarget>;
    [[nodiscard]] auto cancel(std::string_view identifier, const Identity &identity,
                              const std::stop_token &stop) const -> Result<std::string>;
    void loss(bool replaced) const;
    Config config_;
    CharsetConfig charsets_;
    std::vector<Group> groups_;
    std::function<void(std::string_view)> report_;
};
} // namespace jamnntpd
#endif
