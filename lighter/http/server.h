#pragma once

#include <chrono>
#include <contracts>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lighter/types.hpp>
#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/error.h>
#include <lighter/http/types.h>

namespace lighter::http {

/// One parsed request; the handler owns it.
struct ServerRequest {
    std::string method;
    /// The request target as sent, e.g. "/v3/embeddings?verbose=1".
    std::string target;
    /// `target` up to the first '?'.
    std::string path;
    /// `target` after the first '?', not decoded; empty when absent.
    std::string query;
    std::vector<Header> headers;
    std::string body;

    /// The first header with this name (ASCII case-insensitive).
    std::optional<std::string_view> header_value(std::string_view name) const;
};

struct ServerResponse {
    i32 status = 200;
    /// Content-Length, Connection and Transfer-Encoding are written by the server.
    std::vector<Header> headers;
    std::string body;

    static ServerResponse text(i32 status, std::string body, std::string_view content_type = "text/plain; charset=utf-8");

    /// `body` is serialized JSON.
    static ServerResponse json(i32 status, std::string body);
};

struct ServerOptions {
    /// Request line plus header fields.
    usize max_header_bytes = 16 * 1024;
    usize max_body_bytes = 16 * 1024 * 1024;
    /// Connections beyond this are closed as soon as they are accepted.
    usize max_connections = 64;
    /// How long a kept-alive connection may wait for its next request.
    std::chrono::milliseconds idle_timeout{60'000};
    /// How long a request may take to arrive once its first byte has: one
    /// deadline for the whole request (body included), not a per-read limit.
    std::chrono::milliseconds read_timeout{30'000};
    /// How long sending one response may take. A client that stops reading is
    /// disconnected once it runs out, so it cannot hold a slot or stall stop().
    std::chrono::milliseconds write_timeout{30'000};
    bool tcp_nodelay = true;
};

/// Called once per request.
using Handler = std::move_only_function<Task<ServerResponse>(ServerRequest)>;

/// HTTP/1.1 server on the Event loop: keep-alive, Content-Length and chunked
/// request bodies, Expect: 100-continue. Each response is sent whole with a
/// Content-Length; there is no TLS, response streaming or upgrade.
///
/// Requests on one connection are answered in order; connections are served
/// concurrently as children of serve(). Cancelling serve() cancels every
/// handler. stop() is the graceful path: it stops accepting, closes idle
/// connections and lets requests in flight finish; once serve() returns the
/// listening socket is closed. (After a cancelled serve() the socket stays
/// open until the Server is destroyed.)
///
/// The Server must outlive serve().
struct Server {
    Server() noexcept;
    Server(Server &&) noexcept;
    Server &operator=(Server &&) noexcept;
    ~Server();

    /// Binds and listens; `host` must be a numeric address.
    static Result<Server> listen(std::string_view host, i32 port, ServerOptions options = {}, EventLoop &loop = EventLoop::current());

    /// The bound port, useful after listening on port 0.
    i32 port() const noexcept;

    /// Accepts and serves connections until stop() or cancellation, then
    /// returns once every connection has closed. Fails only when accepting does.
    Task<void, lighter::Error> serve(Handler handler) pre(self != nullptr);

    /// Graceful shutdown; safe to call before, during or after serve().
    void stop();

    struct Self;

private:
    explicit Server(std::unique_ptr<Self> self) noexcept;

    std::unique_ptr<Self> self;
};

} // namespace lighter::http
