#include "backend.hpp"
#include "formatting.hpp"
#include "message_values.hpp"
#include "text.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>
#include <utility>

namespace jamnntpd {
namespace {
struct Reference {
    std::uint32_t number{};
    std::string_view group;
};
auto parse_reference(std::string_view text) -> Result<Reference> {
    constexpr std::string_view suffix{"@JamNNTPd>"};
    if (const auto begin = text.rfind('<'); begin != std::string_view::npos) {
        text.remove_prefix(begin);
    }
    const auto dollar = text.find('$');
    if (!text.starts_with('<') || !text.ends_with(suffix) || dollar == std::string_view::npos ||
        dollar < 2 || dollar >= text.size() - suffix.size()) {
        return failure(ErrorKind::input, "Invalid article reference");
    }
    const auto number =
        parse_number(text.substr(1, dollar - 1), std::numeric_limits<std::uint32_t>::max());
    if (!number || *number == 0) {
        return failure(ErrorKind::input, "Invalid reference number");
    }
    return Reference{
        .number = *number,
        .group = text.substr(dollar + 1, text.size() - suffix.size() - dollar - 1),
    };
}
auto header(const RawMimeArticle &raw, std::string_view name) -> std::string_view {
    const auto found = raw.headers.find(std::string{name});
    return found == raw.headers.end() ? std::string_view{} : std::string_view{found->second};
}
auto index_visible(const jam::Message &message, const Group &group, const Identity &identity,
                   const CharsetConfig &charsets, ConversionPolicy policy) -> Result<bool> {
    if (!group.netmail) {
        return true;
    }
    const auto text = read_message_text(message, group, charsets, policy);
    if (!text) {
        return std::unexpected{text.error()};
    }
    return visible_message(group, identity, *text);
}
} // namespace
JamBackend::JamBackend(Config config, CharsetConfig charsets, std::vector<Group> groups,
                       std::function<void(std::string_view)> report)
    : config_(std::move(config)), charsets_(std::move(charsets)), groups_(std::move(groups)),
      report_(std::move(report)) {}
void JamBackend::loss(bool replaced) const {
    if (replaced && report_) {
        report_("Text conversion replaced unrepresentable or malformed characters");
    }
}
auto JamBackend::services() -> SessionServices {
    return {
        .index = [this](const Group &group, const Identity &identity, const std::stop_token &stop)
            -> Result<GroupIndex> { return index(group, identity, stop); },
        .article = [this](const Group &group, std::uint32_t number, const Identity &identity,
                          const UserOptions &options,
                          const std::stop_token &stop) -> Result<ProtocolArticle> {
            return article(group, number, identity, options, stop);
        },
        .submit = [this](std::string_view raw, const Identity &identity, const UserOptions &options,
                         const std::stop_token &stop) -> Result<std::string> {
            return submit(raw, identity, options, stop);
        },
        .login = [this](Credentials credentials) -> Result<Identity> {
            return authenticate(config_.users_file, credentials);
        },
    };
}
auto JamBackend::index(const Group &group, const Identity &identity,
                       const std::stop_token &stop) const -> Result<GroupIndex> {
    if (!match_group(identity.permissions.read, group.access)) {
        return failure(ErrorKind::permission, "Group access denied");
    }
    auto storage = jam::Storage::open(group.base, jam::StorageOptions(), stop);
    if (!storage) {
        return std::unexpected{storage.error()};
    }
    const auto info = storage->info(stop);
    if (!info) {
        return std::unexpected{info.error()};
    }
    GroupIndex result;
    if (info->slots == 0) {
        return result;
    }
    result.count = info->slots;
    result.first = info->header.first_number;
    result.last = result.first + info->slots - 1;
    for (std::uint64_t number = result.first; number <= result.last; ++number) {
        if (stop.stop_requested()) {
            return failure(ErrorKind::stopped, "Index stopped");
        }
        const auto message = storage->read(static_cast<std::uint32_t>(number), stop);
        if (!message) {
            if (message.error().kind == ErrorKind::input) {
                continue;
            }
            return std::unexpected{message.error()};
        }
        auto visible =
            index_visible(*message, group, identity, charsets_, config_.conversion_policy);
        if (!visible) {
            return std::unexpected{visible.error()};
        }
        if (*visible) {
            result.visible.push_back(static_cast<std::uint32_t>(number));
        }
    }
    if (group.netmail && config_.strict_netmail) {
        result.count = static_cast<std::uint32_t>(result.visible.size());
        result.first = result.visible.empty() ? 0 : result.visible.front();
        result.last = result.visible.empty() ? 0 : result.visible.back();
    }
    return result;
}
auto JamBackend::article(const Group &group, std::uint32_t number, const Identity &identity,
                         const UserOptions &options, const std::stop_token &stop) const
    -> Result<ProtocolArticle> {
    if (!match_group(identity.permissions.read, group.access)) {
        return failure(ErrorKind::permission, "Group access denied");
    }
    auto storage = jam::Storage::open(group.base, jam::StorageOptions(), stop);
    if (!storage) {
        return std::unexpected{storage.error()};
    }
    auto message = storage->read(number, stop);
    if (!message) {
        return std::unexpected{message.error()};
    }
    auto text = read_message_text(*message, group, charsets_, config_.conversion_policy);
    if (!text) {
        return std::unexpected{text.error()};
    }
    if (!visible_message(group, identity, *text)) {
        return failure(ErrorKind::permission, "Article not visible");
    }
    loss(text->replaced);
    return render_message(*message, *text, group, options, config_);
}
auto JamBackend::parent(std::string_view identifier, const Group &target, const Identity &identity,
                        const std::stop_token &stop) const -> Result<ReplyTarget> {
    const auto reference = parse_reference(identifier);
    if (!reference) {
        return std::unexpected{reference.error()};
    }
    const auto found = std::ranges::find_if(groups_, [&](const Group &group) -> bool {
        return equal_ascii(group.name, reference->group);
    });
    if (found == groups_.end() || !match_group(identity.permissions.read, found->access)) {
        return failure(ErrorKind::permission, "Parent access denied");
    }
    auto storage = jam::Storage::open(found->base, jam::StorageOptions(), stop);
    if (!storage) {
        return std::unexpected{storage.error()};
    }
    auto message = storage->read(reference->number, stop);
    if (!message) {
        return std::unexpected{message.error()};
    }
    auto text = read_message_text(*message, *found, charsets_, config_.conversion_policy);
    if (!text) {
        return std::unexpected{text.error()};
    }
    if (!visible_message(*found, identity, *text)) {
        return failure(ErrorKind::permission, "Parent not visible");
    }
    auto name = text->sender;
    if (text->opaque) {
        auto decoded = transcode(name, {"ISO-8859-1"}, {"UTF-8"});
        if (!decoded) {
            return std::unexpected{decoded.error()};
        }
        name = std::move(decoded->bytes);
    }
    auto address = text->origin;
    if (target.netmail) {
        if (auto origin = origin_address(message->text); origin) {
            address = origin->text();
        }
    }
    return ReplyTarget{
        .name = std::move(name),
        .address = std::move(address),
        .message_id = text->message_id,
        .number = equal_ascii(found->name, target.name) ? reference->number : 0,
    };
}
auto JamBackend::cancel(std::string_view identifier, const Identity &identity,
                        const std::stop_token &stop) const -> Result<std::string> {
    if (config_.no_cancel) {
        return failure(ErrorKind::permission, "Cancellation disabled");
    }
    const auto reference = parse_reference(identifier);
    if (!reference) {
        return std::unexpected{reference.error()};
    }
    const auto group = std::ranges::find_if(groups_, [&](const Group &entry) -> bool {
        return equal_ascii(entry.name, reference->group);
    });
    if (group == groups_.end() || !match_group(identity.permissions.read, group->access) ||
        !match_group(identity.permissions.post, group->access)) {
        return failure(ErrorKind::permission, "Cancellation access denied");
    }
    auto storage = jam::Storage::open(group->base, jam::StorageOptions(), stop);
    if (!storage) {
        return std::unexpected{storage.error()};
    }
    const auto snapshot = storage->read(reference->number, stop);
    if (!snapshot) {
        return std::unexpected{snapshot.error()};
    }
    const auto text = read_message_text(*snapshot, *group, charsets_, config_.conversion_policy);
    if (!text) {
        return std::unexpected{text.error()};
    }
    if (!visible_message(*group, identity, *text) ||
        !identity_matches(identity, text->sender, *text) ||
        (snapshot->header.attributes & attribute_local) == 0 ||
        (snapshot->header.attributes & (attribute_sent | attribute_locked)) != 0) {
        return failure(ErrorKind::permission, "Cannot cancel this article");
    }
    if (auto cancelled = storage->cancel_checked(reference->number, *snapshot, stop); !cancelled) {
        return std::unexpected{cancelled.error()};
    }
    return std::string{"Article cancelled"};
}
auto JamBackend::submit(std::string_view article, const Identity &identity,
                        const UserOptions &options, const std::stop_token &stop) const
    -> Result<std::string> {
    if (article.size() > maximum_post_bytes || stop.stop_requested()) {
        return failure(ErrorKind::input, "Post exceeds limit or stopped");
    }
    auto raw = parse_raw_article(article, {charsets_.default_post});
    if (!raw) {
        return std::unexpected{raw.error()};
    }
    const auto name = trim(header(*raw, "newsgroups"));
    if (name.empty() || name.contains(',')) {
        return failure(ErrorKind::input, "Exactly one newsgroup is required");
    }
    const auto group = std::ranges::find_if(
        groups_, [&](const Group &entry) -> bool { return equal_ascii(entry.name, name); });
    if (group == groups_.end() || !match_group(identity.permissions.post, group->access)) {
        return failure(ErrorKind::permission, "Posting access denied");
    }
    constexpr std::string_view cancel_prefix{"cancel "};
    if (const auto control = header(*raw, "control"); !control.empty()) {
        if (control.size() <= cancel_prefix.size() ||
            !equal_ascii(control.substr(0, cancel_prefix.size()), cancel_prefix)) {
            return failure(ErrorKind::input, "Unknown control message");
        }
        return cancel(trim(control.substr(cancel_prefix.size())), identity, stop);
    }
    auto plan = resolve_post(
        charsets_, {.source = raw->source_charset, .group_default = group->default_charset});
    if (!plan) {
        return std::unexpected{plan.error()};
    }
    ReplyTarget reply;
    if (!header(*raw, "references").empty()) {
        auto referenced = parent(header(*raw, "references"), *group, identity, stop);
        if (!referenced) {
            return std::unexpected{referenced.error()};
        }
        reply = std::move(*referenced);
    }
    auto built = build_post(*raw, *group, identity, options, *plan, charsets_, config_, reply);
    if (!built) {
        return std::unexpected{built.error()};
    }
    loss(built->replaced);
    auto storage = jam::Storage::open(group->base, jam::StorageOptions(), stop);
    if (!storage) {
        return std::unexpected{storage.error()};
    }
    auto number = storage->append(std::move(built->message), stop);
    if (!number) {
        return std::unexpected{number.error()};
    }
    if (!config_.echomail_file.empty()) {
        static std::mutex notify_mutex;
        std::scoped_lock lock{notify_mutex};
        std::ofstream output{config_.echomail_file, std::ios::app};
        output << group->base.string() << ' ' << *number << '\n';
        output.flush();
        if (!output && report_) {
            report_("Article committed but echomail notification failed");
        }
    }
    return std::string{"Article posted"};
}
} // namespace jamnntpd
