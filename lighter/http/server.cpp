#include "server.h"

#include <algorithm>
#include <format>
#include <span>
#include <tuple>
#include <utility>

#include <llhttp.h>

#include <lighter/utils/config.h>
#include <lighter/async/io/stream.h>
#include <lighter/async/runtime/task_group.h>
#include <lighter/async/runtime/timeout.h>
#include <lighter/http/util.h>

namespace lighter::http {

namespace detail {

struct ServerConnection;

} // namespace detail

struct Server::Self {
    EventLoop *loop = nullptr;
    Tcp::Acceptor acceptor;
    ServerOptions options;
    i32 port = 0;
    bool stopping = false;
    std::vector<detail::ServerConnection *> connections;
};

namespace detail {

/// The request being parsed, filled in by the llhttp callbacks.
struct ServerParse {
    /// Why llhttp_execute() stopped short of the end of its input.
    enum struct Stop { NONE, HEADERS, MESSAGE, HEADERS_TOO_LARGE, BODY_TOO_LARGE };

    ServerRequest request;
    std::string field;
    std::string value;
    usize header_bytes = 0;
    usize max_header_bytes = 0;
    usize max_body_bytes = 0;
    Stop stop = Stop::NONE;
    /// A byte of the next request has arrived.
    bool started = false;

    bool count_header_bytes(usize n) {
        header_bytes += n;
        if (header_bytes > max_header_bytes) {
            stop = Stop::HEADERS_TOO_LARGE;
            return false;
        }
        return true;
    }
};

} // namespace detail

namespace {

using Parse = detail::ServerParse;
using Connection = detail::ServerConnection;

Parse &parse_of(llhttp_t *parser) { return *static_cast<Parse *>(parser->data); }

int on_message_begin(llhttp_t *parser) {
    auto &parse = parse_of(parser);
    parse.request = {};
    parse.field.clear();
    parse.value.clear();
    parse.header_bytes = 0;
    parse.stop = Parse::Stop::NONE;
    parse.started = true;
    return 0;
}

int on_url(llhttp_t *parser, const char *at, usize length) {
    auto &parse = parse_of(parser);
    if (!parse.count_header_bytes(length)) {
        return -1;
    }
    parse.request.target.append(at, length);
    return 0;
}

int on_header_field(llhttp_t *parser, const char *at, usize length) {
    auto &parse = parse_of(parser);
    if (!parse.count_header_bytes(length)) {
        return -1;
    }
    parse.field.append(at, length);
    return 0;
}

int on_header_value(llhttp_t *parser, const char *at, usize length) {
    auto &parse = parse_of(parser);
    if (!parse.count_header_bytes(length)) {
        return -1;
    }
    parse.value.append(at, length);
    return 0;
}

int on_header_value_complete(llhttp_t *parser) {
    auto &parse = parse_of(parser);
    parse.request.headers.push_back({.name = std::move(parse.field), .value = std::move(parse.value)});
    parse.field.clear();
    parse.value.clear();
    return 0;
}

int on_headers_complete(llhttp_t *parser) {
    auto &parse = parse_of(parser);
    auto &request = parse.request;
    request.method = llhttp_method_name(static_cast<llhttp_method_t>(parser->method));
    const auto question = request.target.find('?');
    request.path = request.target.substr(0, question);
    if (question != std::string::npos) {
        request.query = request.target.substr(question + 1);
    }

    const bool has_length = (parser->flags & F_CONTENT_LENGTH) != 0;
    if (has_length && parser->content_length > parse.max_body_bytes) {
        parse.stop = Parse::Stop::BODY_TOO_LARGE;
        return -1;
    }

    // Answer "Expect: 100-continue" before the client sends its body.
    const bool has_body = (has_length && parser->content_length > 0) || (parser->flags & F_CHUNKED) != 0;
    const auto expect = request.header_value("expect");
    if (has_body && expect && detail::iequals(*expect, "100-continue")) {
        parse.stop = Parse::Stop::HEADERS;
        return HPE_PAUSED;
    }
    return 0;
}

int on_body(llhttp_t *parser, const char *at, usize length) {
    auto &parse = parse_of(parser);
    if (parse.request.body.size() + length > parse.max_body_bytes) {
        parse.stop = Parse::Stop::BODY_TOO_LARGE;
        return -1;
    }
    parse.request.body.append(at, length);
    return 0;
}

int on_message_complete(llhttp_t *parser) {
    parse_of(parser).stop = Parse::Stop::MESSAGE;
    return HPE_PAUSED;
}

const llhttp_settings_t &parser_settings() {
    static const llhttp_settings_t settings = [] {
        llhttp_settings_t out;
        llhttp_settings_init(&out);
        out.on_message_begin = on_message_begin;
        out.on_url = on_url;
        out.on_header_field = on_header_field;
        out.on_header_value = on_header_value;
        out.on_header_value_complete = on_header_value_complete;
        out.on_headers_complete = on_headers_complete;
        out.on_body = on_body;
        out.on_message_complete = on_message_complete;
        return out;
    }();
    return settings;
}

} // namespace

struct detail::ServerConnection {
    ServerConnection(Server::Self &server, Tcp stream) : server(&server), stream(std::move(stream)) {
        llhttp_init(&parser, HTTP_REQUEST, &parser_settings());
        parser.data = &parse;
        parse.max_header_bytes = server.options.max_header_bytes;
        parse.max_body_bytes = server.options.max_body_bytes;
        server.connections.push_back(this);
    }

    ServerConnection(const ServerConnection &) = delete;
    ServerConnection &operator=(const ServerConnection &) = delete;

    ~ServerConnection() { std::erase(server->connections, this); }

    Server::Self *server;
    Tcp stream;
    llhttp_t parser{};
    ServerParse parse;
    /// Waiting for the first byte of a request, so stop() may close it.
    bool idle = false;
};

namespace {

/// Where reading a connection got to.
enum struct Next { REQUEST, CONTINUE, CLOSE, TIMEOUT, BAD_REQUEST, HEADERS_TOO_LARGE, BODY_TOO_LARGE, NOT_IMPLEMENTED };

/// Parses buffered bytes, reading more as needed, until a request (or the
/// headers of one that expects 100-continue) is complete or the connection
/// has to end.
Task<Next> read_next(Connection &connection) {
    const auto &options = connection.server->options;
    for (;;) {
        const bool between = !connection.parse.started;
        if (between && connection.server->stopping) {
            co_return Next::CLOSE;
        }

        connection.idle = between;
        auto chunk = co_await with_timeout(connection.stream.read_chunk(), between ? options.idle_timeout : options.read_timeout);
        connection.idle = false;
        if (chunk.is_cancelled()) {
            co_await cancel();
        }
        if (chunk.has_error()) {
            co_return chunk.error() == lighter::Error::k_connection_timed_out && !between ? Next::TIMEOUT : Next::CLOSE;
        }

        const auto data = *chunk;
        const auto status = llhttp_execute(&connection.parser, data.data(), data.size());
        if (status == HPE_OK) {
            connection.stream.consume(data.size());
            continue;
        }
        if (status == HPE_PAUSED) {
            connection.stream.consume(static_cast<usize>(llhttp_get_error_pos(&connection.parser) - data.data()));
            co_return connection.parse.stop == Parse::Stop::HEADERS ? Next::CONTINUE : Next::REQUEST;
        }

        switch (connection.parse.stop) {
            case Parse::Stop::HEADERS_TOO_LARGE: co_return Next::HEADERS_TOO_LARGE;
            case Parse::Stop::BODY_TOO_LARGE: co_return Next::BODY_TOO_LARGE;
            default: co_return status == HPE_PAUSED_UPGRADE ? Next::NOT_IMPLEMENTED : Next::BAD_REQUEST;
        }
    }
}

std::string serialize(const ServerResponse &response, bool keep_alive, bool head_request) {
    auto out = std::format("HTTP/1.1 {} {}\r\n", response.status, status_reason(response.status));
    for (const auto &header : response.headers) {
        if (detail::iequals(header.name, "content-length") || detail::iequals(header.name, "connection") ||
            detail::iequals(header.name, "transfer-encoding")) {
            continue;
        }
        out += std::format("{}: {}\r\n", header.name, header.value);
    }

    const bool bodyless = response.status < 200 || response.status == 204 || response.status == 304;
    if (!bodyless) {
        out += std::format("Content-Length: {}\r\n", response.body.size());
    }
    if (!keep_alive) {
        out += "Connection: close\r\n";
    }
    out += "\r\n";
    if (!bodyless && !head_request) {
        out += response.body;
    }
    return out;
}

Task<bool> send(Connection &connection, std::string bytes) {
    auto written = co_await connection.stream.write(std::span(bytes.data(), bytes.size()));
    co_return written.has_value();
}

Task<ServerResponse> invoke(Handler &handler, ServerRequest request) {
#if LIGHTER_ENABLE_EXCEPTIONS
    try {
        co_return co_await handler(std::move(request));
    } catch (...) {
        co_return ServerResponse::text(500, "internal server error\n");
    }
#else
    co_return co_await handler(std::move(request));
#endif
}

ServerResponse error_response(Next next) {
    switch (next) {
        case Next::TIMEOUT: return ServerResponse::text(408, "request timeout\n");
        case Next::HEADERS_TOO_LARGE: return ServerResponse::text(431, "request header fields too large\n");
        case Next::BODY_TOO_LARGE: return ServerResponse::text(413, "content too large\n");
        case Next::NOT_IMPLEMENTED: return ServerResponse::text(501, "not implemented\n");
        default: return ServerResponse::text(400, "bad request\n");
    }
}

Task<> serve_connection(Server::Self &server, Tcp stream, Handler &handler) {
    Connection connection(server, std::move(stream));
    if (server.options.tcp_nodelay) {
        std::ignore = connection.stream.set_nodelay(true);
    }

    for (;;) {
        const auto next = co_await read_next(connection);
        if (next == Next::CLOSE) {
            co_return;
        }
        if (next == Next::CONTINUE) {
            if (!co_await send(connection, "HTTP/1.1 100 Continue\r\n\r\n")) {
                co_return;
            }
            llhttp_resume(&connection.parser);
            continue;
        }
        if (next != Next::REQUEST) {
            std::ignore = co_await send(connection, serialize(error_response(next), false, false));
            co_return;
        }

        const bool head_request = connection.parse.request.method == method::head;
        const bool wants_keep_alive = llhttp_should_keep_alive(&connection.parser) != 0;
        auto request = std::move(connection.parse.request);
        connection.parse.request = {};
        connection.parse.started = false;
        connection.parse.stop = Parse::Stop::NONE;

        auto response = co_await invoke(handler, std::move(request));
        const bool keep_alive = wants_keep_alive && !server.stopping;
        if (!co_await send(connection, serialize(response, keep_alive, head_request)) || !keep_alive) {
            co_return;
        }
        llhttp_resume(&connection.parser);
    }
}

Task<void, lighter::Error> accept_connections(Server::Self &server, TaskGroup<lighter::Error> &group, Handler &handler) {
    while (!server.stopping) {
        auto accepted = co_await server.acceptor.accept();
        if (!accepted) {
            if (server.stopping) {
                break;
            }
            server.acceptor = {};
            co_await fail(accepted.error());
        }
        // Dropping the stream closes it.
        if (server.connections.size() >= server.options.max_connections) {
            continue;
        }
        group.spawn(serve_connection(server, std::move(*accepted), handler));
    }
    // No accept is pending any more: close the listening socket, so a stopped
    // server neither queues new connections nor keeps the loop running.
    server.acceptor = {};
}

} // namespace

std::optional<std::string_view> ServerRequest::header_value(std::string_view name) const {
    for (const auto &header : headers) {
        if (detail::iequals(header.name, name)) {
            return std::string_view(header.value);
        }
    }
    return std::nullopt;
}

ServerResponse ServerResponse::text(i32 status, std::string body, std::string_view content_type) {
    return {.status = status, .headers = {{.name = "Content-Type", .value = std::string(content_type)}}, .body = std::move(body)};
}

ServerResponse ServerResponse::json(i32 status, std::string body) { return text(status, std::move(body), "application/json"); }

Server::Server() noexcept = default;

Server::Server(Server &&) noexcept = default;

Server &Server::operator=(Server &&) noexcept = default;

Server::~Server() = default;

Server::Server(std::unique_ptr<Self> self) noexcept : self(std::move(self)) {}

Result<Server> Server::listen(std::string_view host, i32 port, ServerOptions options, EventLoop &loop) {
    auto acceptor = Tcp::listen(host, port, {}, loop);
    if (!acceptor) {
        return outcome_error(acceptor.error());
    }
    auto bound = Tcp::local_port(*acceptor);
    if (!bound) {
        return outcome_error(bound.error());
    }

    auto self = std::make_unique<Self>();
    self->loop = &loop;
    self->acceptor = std::move(*acceptor);
    self->options = options;
    self->port = *bound;
    return Server(std::move(self));
}

i32 Server::port() const noexcept { return self ? self->port : 0; }

Task<void, lighter::Error> Server::serve(Handler handler) {
    auto &server = *self;
    TaskGroup<lighter::Error> group(*server.loop);
    group.spawn(accept_connections(server, group, handler));
    auto joined = co_await group.join();
    if (joined.has_error()) {
        co_await fail(joined.error().front());
    }
}

void Server::stop() {
    if (!self || self->stopping) {
        return;
    }
    self->stopping = true;
    std::ignore = self->acceptor.stop();
    for (auto *connection : self->connections) {
        if (connection->idle) {
            connection->stream.stop();
        }
    }
}

} // namespace lighter::http
