#include "session.hpp"
#include "text.hpp"
#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <utility>

namespace jamnntpd {
namespace {
auto response(std::string_view text) -> ProtocolReply {
    return {.bytes = std::string{text} + "\r\n", .action = ProtocolAction::continue_session};
}
auto take_word(std::string_view &text) -> std::string_view {
    text = trim(text);
    const auto end = text.find_first_of(" \t");
    const auto word = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : trim(text.substr(end + 1));
    return word;
}
auto message_id(std::uint32_t number, std::string_view group) -> std::string {
    return std::format("<{}${}@JamNNTPd>", number, group);
}
auto article_number(std::string_view text) -> Result<std::uint32_t> {
    auto number = parse_number(text, std::numeric_limits<std::uint32_t>::max());
    if (!number || *number == 0) {
        return failure(ErrorKind::input, "501 Invalid article number specified");
    }
    return number;
}
auto local_error() -> ProtocolReply {
    return response("503 Local error: Could not access messagebase");
}
auto login_options(std::string_view text, UserOptions &options) -> Result<std::string> {
    const auto slash = text.find('/');
    std::string username{text.substr(0, slash)};
    if (slash == std::string_view::npos) {
        return username;
    }
    text.remove_prefix(slash + 1);
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto option = text.substr(0, comma);
        const auto equal = option.find('=');
        if (equal == std::string_view::npos) {
            return failure(ErrorKind::input, "482 Invalid option format, use option=on/off");
        }
        if (auto set = set_user_option(
                options, {.name = option.substr(0, equal), .value = option.substr(equal + 1)});
            !set) {
            return failure(ErrorKind::input, "482 Invalid user option or setting");
        }
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return username;
}
auto help_response() -> std::string_view;
} // namespace
Session::Session(std::vector<Group> groups, Identity identity, UserOptions options,
                 SessionServices services)
    : groups_(std::move(groups)), identity_(std::move(identity)), options_(options),
      services_(std::move(services)) {}
auto Session::selected_group() const -> std::string_view {
    return current_group_ ? std::string_view{groups_.at(*current_group_).name} : std::string_view{};
}
auto Session::select_article(std::string_view argument) -> Result<ArticleSelection> {
    if (argument.starts_with('<')) {
        const auto dollar = argument.find('$');
        constexpr std::string_view suffix{"@JamNNTPd>"};
        if (dollar == std::string_view::npos || !argument.ends_with(suffix) || dollar == 1 ||
            dollar >= argument.size() - suffix.size()) {
            return failure(ErrorKind::input, "430 No such article found");
        }
        const auto number = article_number(argument.substr(1, dollar - 1));
        const auto name = argument.substr(dollar + 1, argument.size() - suffix.size() - dollar - 1);
        for (std::size_t index = 0; index < groups_.size(); ++index) {
            const auto &group = groups_.at(index);
            if (number && equal_ascii(name, group.name) &&
                match_group(identity_.permissions.read, group.access)) {
                return ArticleSelection{.group = index, .number = *number, .by_id = true};
            }
        }
        return failure(ErrorKind::input, "430 No such article found");
    }
    if (!current_group_) {
        return failure(ErrorKind::input, "412 No newsgroup selected");
    }
    if (argument.empty()) {
        if (current_article_ == 0) {
            return failure(ErrorKind::input, "420 No current article has been selected");
        }
        return ArticleSelection{
            .group = *current_group_,
            .number = current_article_,
            .by_id = false,
        };
    }
    const auto number = article_number(argument);
    if (!number) {
        return std::unexpected{number.error()};
    }
    return ArticleSelection{.group = *current_group_, .number = *number, .by_id = false};
}
auto Session::list(std::string_view argument, const std::stop_token &stop) -> ProtocolReply {
    const auto kind = lower_ascii(take_word(argument));
    if (kind == "overview.fmt") {
        return {
            .bytes = "215 List of fields in XOVER result\r\nSubject:\r\nFrom:\r\nDate:\r\n"
                     "Message-ID:\r\nReferences:\r\nBytes:\r\nLines:\r\n.\r\n",
        };
    }
    if (kind == "newsgroups" && !argument.empty()) {
        return response("501 Patterns not supported for LIST NEWSGROUPS");
    }
    if (!kind.empty() && kind != "active" && kind != "newsgroups") {
        return response("501 Unknown argument for LIST command");
    }
    std::string output{"215 List of newsgroups follows\r\n"};
    for (const auto &group : groups_) {
        if (!match_group(identity_.permissions.read, group.access)) {
            continue;
        }
        if (stop.stop_requested()) {
            return local_error();
        }
        if (kind == "newsgroups") {
            output += group.name + "\t\r\n";
        } else {
            const auto index = services_.index(group, identity_, stop);
            output += std::format(
                "{} {} {} {}\r\n", group.name, index ? index->last : 0, index ? index->first : 0,
                match_group(identity_.permissions.post, group.access) ? 'y' : 'n');
        }
    }
    return {.bytes = output + ".\r\n"};
}
auto Session::group(std::string_view argument, const std::stop_token &stop) -> ProtocolReply {
    const auto name = take_word(argument);
    if (name.empty()) {
        return response("501 No group specified");
    }
    for (std::size_t index = 0; index < groups_.size(); ++index) {
        const auto &group = groups_.at(index);
        if (!equal_ascii(name, group.name) ||
            !match_group(identity_.permissions.read, group.access)) {
            continue;
        }
        const auto information = services_.index(group, identity_, stop);
        if (!information) {
            return response("503 Local error: Could not get size of messagebase");
        }
        current_group_ = index;
        current_article_ = information->first;
        return response(std::format("211 {} {} {} {} Group selected", information->count,
                                    information->first, information->last, group.name));
    }
    return response("411 No such newsgroup");
}
auto Session::article(ArticlePart part, std::string_view argument, const std::stop_token &stop)
    -> ProtocolReply {
    const auto selection = select_article(take_word(argument));
    if (!selection) {
        return response(selection.error().message);
    }
    const auto &group = groups_.at(selection->group);
    const auto article = services_.article(group, selection->number, identity_, options_, stop);
    if (!article) {
        if (article.error().kind == ErrorKind::io || article.error().kind == ErrorKind::corrupt) {
            return local_error();
        }
        return response(selection->by_id ? "430 No such article found"
                                         : "423 No article with that number");
    }
    if (!selection->by_id) {
        current_article_ = selection->number;
    }
    const auto identifier = message_id(selection->number, group.name);
    if (part == ArticlePart::status) {
        return response(std::format("223 {} {} Article retrieved", selection->number, identifier));
    }
    const auto code = static_cast<unsigned>(part);
    std::string_view description{"Head and body follow"};
    if (part == ArticlePart::head) {
        description = "Head follows";
    } else if (part == ArticlePart::body) {
        description = "Body follows";
    }
    std::string output = std::format("{} {} {} Article retrieved - {}\r\n", code, selection->number,
                                     identifier, description);
    if (part != ArticlePart::body) {
        output += article->headers;
    }
    if (part == ArticlePart::whole) {
        output += "\r\n";
    }
    if (part != ArticlePart::head) {
        output += article->body;
    }
    return {.bytes = output + ".\r\n"};
}
auto Session::navigate(bool next, const std::stop_token &stop) -> ProtocolReply {
    if (!current_group_) {
        return response("412 No newsgroup selected");
    }
    if (current_article_ == 0) {
        return response("420 No current article has been selected");
    }
    const auto &group = groups_.at(*current_group_);
    auto index = services_.index(group, identity_, stop);
    if (!index) {
        return local_error();
    }
    std::ranges::sort(index->visible);
    const auto found = next ? std::ranges::upper_bound(index->visible, current_article_)
                            : std::ranges::lower_bound(index->visible, current_article_);
    if ((next && found == index->visible.end()) || (!next && found == index->visible.begin())) {
        return response(next ? "421 No next article in this group"
                             : "422 No previous article in this group");
    }
    current_article_ = next ? *found : *std::prev(found);
    return response(std::format("223 {} {} Article retrieved", current_article_,
                                message_id(current_article_, group.name)));
}
auto Session::overview(std::string_view argument, const std::stop_token &stop) -> ProtocolReply {
    if (!current_group_) {
        return response("412 No newsgroup selected");
    }
    const auto &group = groups_.at(*current_group_);
    const auto index = services_.index(group, identity_, stop);
    if (!index) {
        return local_error();
    }
    std::uint32_t first = current_article_;
    std::uint32_t last = current_article_;
    const auto range = take_word(argument);
    if (range.empty() && current_article_ == 0) {
        return response("420 No current article has been selected");
    }
    if (!range.empty()) {
        const auto dash = range.find('-');
        const auto lower = article_number(range.substr(0, dash));
        auto upper = lower;
        if (dash != std::string_view::npos) {
            upper = range.ends_with('-') ? Result<std::uint32_t>{index->last}
                                         : article_number(range.substr(dash + 1));
        }
        if (!lower || !upper) {
            return response("501 Invalid article range specified");
        }
        first = *lower;
        last = *upper;
    }
    if (first > last || index->count == 0 || last < index->first || first > index->last) {
        return response("420 No articles found in this range");
    }
    std::string output{"224 Overview information follows\r\n"};
    for (const auto number : index->visible) {
        if (stop.stop_requested()) {
            return {.bytes = {}, .action = ProtocolAction::close};
        }
        if (number < first || number > last) {
            continue;
        }
        const auto article = services_.article(group, number, identity_, options_, stop);
        if (article) {
            output += std::to_string(number) + "\t" + article->overview + "\r\n";
        } else if (article.error().kind == ErrorKind::io ||
                   article.error().kind == ErrorKind::corrupt) {
            return local_error();
        }
    }
    return {.bytes = output + ".\r\n"};
}
auto Session::auth(std::string_view argument) -> ProtocolReply {
    const auto kind = lower_ascii(take_word(argument));
    if (kind != "user" && kind != "pass") {
        return response("501 Only AUTHINFO USER or AUTHINFO pass are understood");
    }
    const auto value = take_word(argument);
    if (kind == "user") {
        if (value.empty()) {
            return response("482 No user specified for AUTHINFO USER");
        }
        pending_username_ = value;
        return response("381 Received login name, now send password");
    }
    if (pending_username_.empty()) {
        return response("482 Use AUTHINFO USER before AUTHINFO pass");
    }
    if (value.empty()) {
        return response("482 No password specified for AUTHINFO PASS");
    }
    auto candidate = options_;
    const auto username = login_options(pending_username_, candidate);
    if (!username) {
        return response(username.error().message);
    }
    if (!username->empty()) {
        auto identity = services_.login({.username = *username, .password = value});
        if (!identity) {
            return response("481 Authentication rejected");
        }
        identity_ = std::move(*identity);
    }
    options_ = candidate;
    if (current_group_ &&
        !match_group(identity_.permissions.read, groups_.at(*current_group_).access)) {
        current_group_.reset();
        current_article_ = 0;
    }
    return response(username->empty() ? "281 Authentication accepted (options set, no login)"
                                      : "281 Authentication accepted");
}
auto Session::post(std::string_view article, const std::stop_token &stop) -> ProtocolReply {
    if (identity_.permissions.post.empty() || identity_.permissions.post == "-") {
        return response("440 Posting not permitted");
    }
    const auto result = services_.submit(article, identity_, options_, stop);
    return result ? response("240 " + *result) : response("441 Posting failed");
}
auto Session::command(std::string_view line, const std::stop_token &stop) -> ProtocolReply {
    if (stop.stop_requested()) {
        return {.bytes = {}, .action = ProtocolAction::close};
    }
    constexpr std::size_t maximum_command = 998;
    if (line.size() > maximum_command || line.contains('\0') ||
        line.find_first_of("\r\n") != std::string_view::npos) {
        return {.bytes = "500 Invalid command line\r\n", .action = ProtocolAction::close};
    }
    const auto command = lower_ascii(take_word(line));
    if (command.empty()) {
        return {};
    }
    if (command == "group") {
        return group(line, stop);
    }
    if (command == "list") {
        return list(line, stop);
    }
    if (command == "authinfo") {
        return auth(line);
    }
    constexpr std::array parts{
        std::pair{"article", ArticlePart::whole},
        std::pair{"head", ArticlePart::head},
        std::pair{"body", ArticlePart::body},
        std::pair{"stat", ArticlePart::status},
    };
    const auto part = static_cast<std::size_t>(std::ranges::distance(
        parts.begin(), std::ranges::find(parts, command, [](const auto &entry) -> std::string_view {
            return entry.first;
        })));
    if (part != parts.size()) {
        return article(parts.at(part).second, line, stop);
    }
    if (command == "next" || command == "last") {
        return navigate(command == "next", stop);
    }
    if (command == "xover") {
        return overview(line, stop);
    }
    if (command == "help") {
        return {.bytes = std::string{help_response()}};
    }
    if (command == "quit") {
        return {.bytes = "205 Goodbye\r\n", .action = ProtocolAction::close};
    }
    if (command == "post") {
        if (identity_.permissions.post.empty() || identity_.permissions.post == "-") {
            return response("440 Posting not permitted");
        }
        return {
            .bytes = "340 Send article to be posted. End with <CR-LF>.<CR-LF>\r\n",
            .action = ProtocolAction::receive_post,
        };
    }
    constexpr std::array stubs{
        std::pair{"ihave", "435 Article not wanted - do not send it"},
        std::pair{"newgroups", "231 Warning: NEWGROUPS not implemented, returning empty list\r\n."},
        std::pair{"newnews", "230 Warning: NEWNEWS not implemented, returning empty list\r\n."},
        std::pair{"slave", "202 Slave status noted (but ignored)"},
        std::pair{"mode", "200 Server ready - posting allowed"},
    };
    const auto stub = static_cast<std::size_t>(std::ranges::distance(
        stubs.begin(), std::ranges::find(stubs, command, [](const auto &entry) -> std::string_view {
            return entry.first;
        })));
    return stub == stubs.size() ? response("500 Unknown command") : response(stubs.at(stub).second);
}
namespace {
auto help_response() -> std::string_view {
    return "100 Help text follows\r\n"
           "Recognized commands:\r\n"
           "\r\n"
           "ARTICLE\r\n"
           "AUTHINFO\r\n"
           "BODY\r\n"
           "GROUP\r\n"
           "HEAD\r\n"
           "HELP\r\n"
           "IHAVE (not implemented, messages are always rejected)\r\n"
           "LAST\r\n"
           "LIST\r\n"
           "NEWGROUPS (not implemented, always returns an empty list)\r\n"
           "NEWNEWS (not implemented, always returns an empty list)\r\n"
           "NEXT\r\n"
           "QUIT\r\n"
           "SLAVE (has no effect)\r\n"
           "STAT\r\n"
           "XOVER (partially implemented, byte count and line count are always empty)\r\n"
           "\r\n"
           "JamNNTPd supports most of RFC-977 and also has support for AUTHINFO and\r\n"
           "limited XOVER support (RFC-2980)\r\n"
           ".\r\n";
}
} // namespace
} // namespace jamnntpd
