#include "server.hpp"
#include "backend.hpp"
#include "logging.hpp"
#include "network.hpp"
#include "signals.hpp"
#include "text.hpp"
#include "workers.hpp"
#include <format>
#include <memory>

namespace jamnntpd {
namespace {
constexpr std::size_t maximum_protocol_line = 1000;
auto receive_post(LineReader &reader, const std::stop_token &stop) -> Result<std::string> {
    std::string article;
    while (!stop.stop_requested()) {
        auto line = reader.line(maximum_protocol_line, stop);
        if (!line) {
            return std::unexpected{line.error()};
        }
        if (*line == ".\r\n" || *line == ".\n") {
            return article;
        }
        if (line->starts_with("..")) {
            line->erase(0, 1);
        }
        if (line->size() > maximum_post_bytes - article.size()) {
            return failure(ErrorKind::input, "Article exceeds limit");
        }
        article += *line;
    }
    return failure(ErrorKind::stopped, "Post stopped");
}
void reject(Socket &socket, std::string_view response, const std::stop_token &stop) {
    if (!socket.send(response, stop)) {
        socket.close();
    }
}
auto process_post(Socket &socket, LineReader &reader, Session &session, ServiceLogger &logger,
                  std::string_view context, const std::stop_token &stop) -> Result<void> {
    const auto article = receive_post(reader, stop);
    if (!article) {
        if (article.error().kind == ErrorKind::input) {
            reject(socket, "441 Posting failed (Article exceeds limit)\r\n", stop);
        }
        return std::unexpected{article.error()};
    }
    const auto result = session.post(*article, stop);
    if (auto written = socket.send(result.bytes, stop); !written) {
        return written;
    }
    logger.write(spdlog::level::info, context,
                 result.bytes.starts_with("240") ? "POST committed" : "POST rejected");
    return {};
}
auto converse(Socket &socket, Session &session, ServiceLogger &logger, const Config &config,
              std::string_view context, const std::stop_token &stop) -> Result<void> {
    auto sent = socket.send(
        "200 Welcome to JamNNTPd 2.0.0 (posting may or may not be allowed, try your luck)\r\n",
        stop);
    if (!sent) {
        return sent;
    }
    LineReader reader{socket};
    while (!stop.stop_requested()) {
        auto line = reader.line(maximum_protocol_line, stop);
        if (!line) {
            return std::unexpected{line.error()};
        }
        if (line->ends_with('\n')) {
            line->pop_back();
        }
        if (line->ends_with('\r')) {
            line->pop_back();
        }
        logger.protocol_input(context, *line);
        const std::string previous_group{session.selected_group()};
        auto reply = session.command(*line, stop);
        if (config.log_groups && session.selected_group() != previous_group &&
            !session.selected_group().empty()) {
            logger.write(spdlog::level::info, context,
                         "Group selected: " + std::string{session.selected_group()});
        }
        if (auto written = socket.send(reply.bytes, stop); !written) {
            return written;
        }
        if (reply.action == ProtocolAction::close) {
            return {};
        }
        if (reply.action == ProtocolAction::receive_post) {
            if (auto posted = process_post(socket, reader, session, logger, context, stop);
                !posted) {
                return posted;
            }
        }
    }
    return {};
}
void connection(Socket &socket, const Config &config, ServiceLogger &logger,
                const std::stop_token &stop) {
    const auto peer = socket.peer();
    if (!peer) {
        logger.write(spdlog::level::warn, "server", "Cannot identify peer");
        return;
    }
    const auto context = std::format("{}:{}", peer->address, peer->port);
    logger.write(spdlog::level::info, context, "Connection established");
    const auto permissions = guest_permissions(config.allow_file, peer->address);
    if (!permissions) {
        reject(socket, "502 Access denied.\r\n", stop);
        logger.write(spdlog::level::warn, context, "Connection access denied");
        return;
    }
    auto groups = load_groups(config.groups_file);
    auto charsets = load_charset_config(config.xlat_file);
    if (!groups || !charsets) {
        reject(socket,
               !groups ? "503 Failed to read group configuration file\r\n"
                       : "503 Failed to read xlat configuration file\r\n",
               stop);
        logger.write(spdlog::level::err, context, "Session configuration rejected");
        return;
    }
    JamBackend backend{config, std::move(*charsets), *groups, [&](std::string_view text) -> void {
                           logger.write(spdlog::level::warn, context, text);
                       }};
    Session session{std::move(*groups),
                    {.username = "", .permissions = *permissions, .realnames = ""},
                    config.defaults,
                    backend.services()};
    const auto result = converse(socket, session, logger, config, context, stop);
    if (!result && result.error().kind != ErrorKind::stopped) {
        logger.write(spdlog::level::warn, context, "Connection protocol or I/O failure");
    }
    logger.write(spdlog::level::info, context, "Connection closed");
}
} // namespace

auto run_server(const Config &config) -> Result<void> {
    std::stop_source stop;
    auto signals = SignalStop::create(stop);
    if (!signals) {
        return std::unexpected{signals.error()};
    }
    auto runtime = NetworkRuntime::create();
    if (!runtime) {
        return std::unexpected{runtime.error()};
    }
    auto logger = ServiceLogger::create(config);
    if (!logger) {
        return std::unexpected{logger.error()};
    }
    auto listener = Socket::listen(config.port);
    if (!listener) {
        return std::unexpected{listener.error()};
    }
    auto port = listener->local_port();
    if (!port) {
        return std::unexpected{port.error()};
    }
    WorkerPool workers{config.maximum_connections, [&](std::string_view text) -> void {
                           (*logger)->write(spdlog::level::err, "server", text);
                       }};
    (*logger)->write(spdlog::level::info, "server", "Listening on port " + std::to_string(*port));
    (*logger)->flush();
    while (!stop.stop_requested()) {
        auto accepted = listener->accept(stop.get_token());
        if (!accepted) {
            if (accepted.error().kind == ErrorKind::stopped) {
                break;
            }
            if (accepted.error().kind == ErrorKind::timeout) {
                workers.reap();
                continue;
            }
            workers.stop_join();
            (*logger)->flush();
            return std::unexpected{accepted.error()};
        }
        auto socket = std::make_shared<Socket>(std::move(*accepted));
        auto started =
            workers.start([socket, &config, &logger](const std::stop_token &token) -> void {
                connection(*socket, config, **logger, token);
            });
        if (!started) {
            reject(*socket, "502 Maximum number of connections reached, please try again later\r\n",
                   stop.get_token());
            (*logger)->write(spdlog::level::warn, "server",
                             "Connection admission or worker startup failed");
        }
    }
    workers.stop_join();
    (*logger)->write(spdlog::level::info, "server", "All workers joined; server stopped");
    (*logger)->flush();
    return {};
}
auto command_help() -> std::string_view {
    return "Usage: jamnntpd [<options>]\n"
           "-p[ort] <port>  -m[ax] <connections>  -g[roups] <file>  -a[llow] <file>\n"
           "-u[sers] <file>  -x[lat] <file>  -l[ogfile] <file>\n"
           "-config <file>  -create <file>  -debug  -noecholog  -loggroups\n"
           "-readorigin  -noencode  -strictnetmail  -nostripre  -notearline\n"
           "-note  -noreplyaddr  -notzutc  -nocancel  -smartquote\n"
           "-def_flowed on/off  -def_showto on/off  -def_nonbsp on/off\n"
           "-def_delssq on/off  -def_addcr on/off  -origin <text>\n"
           "-guestsuffix <text>  -echomailjam <file>  -conversion_errors strict/replace\n"
           "No arguments: read the default configuration file if present.\n"
           "JAM(mbp) - Copyright 1993 Joaquim Homrighausen, Andrew Milner, Mats Birch, Mats "
           "Wallin. ALL RIGHTS RESERVED.\n";
}
} // namespace jamnntpd
