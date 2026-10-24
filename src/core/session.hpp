#ifndef JAMNNTPD_CORE_SESSION_HPP
#define JAMNNTPD_CORE_SESSION_HPP
#include "config.hpp"
#include <functional>
#include <optional>
#include <stop_token>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jamnntpd {
struct GroupIndex {
    std::uint32_t count{};
    std::uint32_t first{};
    std::uint32_t last{};
    std::vector<std::uint32_t> visible;
};
struct ProtocolArticle {
    std::string headers;
    std::string body;
    std::string overview;
};
struct SessionServices {
    std::function<Result<GroupIndex>(const Group &, const Identity &, const std::stop_token &)>
        index;
    std::function<Result<ProtocolArticle>(const Group &, std::uint32_t, const Identity &,
                                          const UserOptions &, const std::stop_token &)>
        article;
    std::function<Result<std::string>(std::string_view, const Identity &, const UserOptions &,
                                      const std::stop_token &)>
        submit;
    std::function<Result<Identity>(Credentials)> login;
};
enum class ProtocolAction : std::uint8_t { continue_session, close, receive_post };
struct ProtocolReply {
    std::string bytes;
    ProtocolAction action{ProtocolAction::continue_session};
};
class Session {
  public:
    Session(std::vector<Group> groups, Identity identity, UserOptions options,
            SessionServices services);
    [[nodiscard]] auto command(std::string_view line, const std::stop_token &stop = {})
        -> ProtocolReply;
    [[nodiscard]] auto post(std::string_view article, const std::stop_token &stop = {})
        -> ProtocolReply;
    [[nodiscard]] auto identity() const -> const Identity & { return identity_; }
    [[nodiscard]] auto options() const -> const UserOptions & { return options_; }
    [[nodiscard]] auto selected_group() const -> std::string_view;

  private:
    enum class ArticlePart : std::uint8_t { whole = 220, head = 221, body = 222, status = 223 };
    struct ArticleSelection {
        std::size_t group{};
        std::uint32_t number{};
        bool by_id{};
    };
    [[nodiscard]] auto select_article(std::string_view argument) -> Result<ArticleSelection>;
    [[nodiscard]] auto list(std::string_view argument, const std::stop_token &stop)
        -> ProtocolReply;
    [[nodiscard]] auto group(std::string_view argument, const std::stop_token &stop)
        -> ProtocolReply;
    [[nodiscard]] auto article(ArticlePart part, std::string_view argument,
                               const std::stop_token &stop) -> ProtocolReply;
    [[nodiscard]] auto navigate(bool next, const std::stop_token &stop) -> ProtocolReply;
    [[nodiscard]] auto overview(std::string_view argument, const std::stop_token &stop)
        -> ProtocolReply;
    [[nodiscard]] auto auth(std::string_view argument) -> ProtocolReply;
    std::vector<Group> groups_;
    Identity identity_;
    UserOptions options_;
    SessionServices services_;
    std::optional<std::size_t> current_group_;
    std::uint32_t current_article_{};
    std::string pending_username_;
};
} // namespace jamnntpd
#endif
