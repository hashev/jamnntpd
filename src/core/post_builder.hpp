#ifndef JAMNNTPD_CORE_POST_BUILDER_HPP
#define JAMNNTPD_CORE_POST_BUILDER_HPP
#include "ftn.hpp"
#include "mime.hpp"
#include <cstdint>
#include <string>

namespace jamnntpd {
struct ReplyTarget {
    std::string name{"All"};
    std::string address;
    std::string message_id;
    std::uint32_t number{};
};
struct BuiltPost {
    jam::Message message;
    bool replaced{};
};
[[nodiscard]] auto build_post(const RawMimeArticle &raw, const Group &group,
                              const Identity &identity, const UserOptions &options,
                              const ConversionPlan &plan, const CharsetConfig &charsets,
                              const Config &config, const ReplyTarget &parent) -> Result<BuiltPost>;
} // namespace jamnntpd
#endif
