#include <charconv>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <lighter/async/io/loop.h>
#include <lighter/async/io/stream.h>
#include <lighter/async/runtime/sync.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/runtime/when.h>
#include <lighter/http/http.h>
#include <lighter/http/server.h>

namespace {

using namespace lighter;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

std::string url(const http::Server &server, std::string_view path) {
    return "http://127.0.0.1:" + std::to_string(server.port()) + std::string(path);
}

/// Echoes the method, path, query and body; "/fail" throws, "/x-header"
/// returns the X-Test header.
Task<http::ServerResponse> echo(http::ServerRequest request) {
    if (request.path == "/fail") {
        throw std::runtime_error("handler failure");
    }
    if (request.path == "/x-header") {
        co_return http::ServerResponse::text(200, std::string(request.header_value("x-test").value_or("missing")));
    }
    co_return http::ServerResponse::text(200, request.method + " " + request.path + " " + request.query + " " + request.body);
}

struct RawResponse {
    i32 status = 0;
    std::string head;
    std::string body;
};

/// Reads one response from a raw connection; `buffer` keeps bytes that belong
/// to the next one.
Task<RawResponse> read_response(Tcp &connection, std::string &buffer) {
    for (;;) {
        const auto end = buffer.find("\r\n\r\n");
        if (end != std::string::npos) {
            RawResponse out;
            out.head = buffer.substr(0, end);
            std::from_chars(out.head.data() + 9, out.head.data() + 12, out.status);
            usize length = 0;
            if (const auto at = out.head.find("Content-Length: "); at != std::string::npos) {
                std::from_chars(out.head.data() + at + 16, out.head.data() + out.head.size(), length);
            }
            if (buffer.size() >= end + 4 + length) {
                out.body = buffer.substr(end + 4, length);
                buffer.erase(0, end + 4 + length);
                co_return out;
            }
        }
        auto more = co_await connection.read();
        require(more.has_value(), "the server closed the connection mid-response");
        buffer += *more;
    }
}

Task<Tcp> connect(const http::Server &server) {
    auto connection = co_await Tcp::connect("127.0.0.1", server.port());
    require(connection.has_value(), "failed to connect to the test server");
    co_return std::move(*connection);
}

Task<> write(Tcp &connection, std::string_view bytes) {
    auto written = co_await connection.write(std::span(bytes.data(), bytes.size()));
    require(written.has_value(), "failed to write to the test server");
}

/// Waits until the server closes the connection.
Task<> expect_closed(Tcp &connection) {
    for (;;) {
        auto more = co_await connection.read();
        if (!more) {
            co_return;
        }
    }
}

Task<> run_and_stop(http::Server &server, Task<> client) {
    co_await std::move(client);
    server.stop();
}

/// Serves `server` while `client` runs, then stops it gracefully.
Task<> exercise(http::Server &server, http::Handler handler, Task<> client) {
    auto served = co_await when_all(server.serve(std::move(handler)), run_and_stop(server, std::move(client)));
    require(served.has_value(), "serve() failed");
}

http::Server listen(http::ServerOptions options = {}) {
    auto server = http::Server::listen("127.0.0.1", 0, options);
    require(server.has_value(), "failed to listen");
    return std::move(*server);
}

Task<> client_basics(http::Server &server) {
    http::Client client;
    auto get = client.on().get(url(server, "/hello?name=lighter"));
    auto got = co_await std::move(get).send();
    require(got.has_value(), "GET failed");
    require(got->status == 200 && got->text() == "GET /hello name=lighter ", "GET echoed the wrong request");

    auto post = client.on().post(url(server, "/echo"));
    post.json_text(R"({"x":1})");
    auto posted = co_await std::move(post).send();
    require(posted.has_value() && posted->text() == R"(POST /echo  {"x":1})", "POST echoed the wrong body");

    auto header = client.on().get(url(server, "/x-header"));
    header.header("X-Test", "Value");
    auto headed = co_await std::move(header).send();
    require(headed.has_value() && headed->text() == "Value", "headers are not looked up case-insensitively");

    auto failing = client.on().get(url(server, "/fail"));
    auto failed = co_await std::move(failing).send();
    require(failed.has_value() && failed->status == 500, "a throwing handler did not produce 500");
}

Task<> pipelined_keep_alive(http::Server &server) {
    auto connection = co_await connect(server);
    co_await write(connection, "GET /one HTTP/1.1\r\nHost: x\r\n\r\n"
                               "POST /two HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc"
                               "HEAD /three HTTP/1.1\r\nHost: x\r\n\r\n");
    std::string buffer;
    auto first = co_await read_response(connection, buffer);
    auto second = co_await read_response(connection, buffer);
    require(first.status == 200 && first.body == "GET /one  ", "first pipelined response is wrong");
    require(second.status == 200 && second.body == "POST /two  abc", "second pipelined response is wrong");

    // HEAD gets the length of the body it would have had, and no body.
    for (;;) {
        if (buffer.find("\r\n\r\n") != std::string::npos) {
            break;
        }
        auto more = co_await connection.read();
        require(more.has_value(), "no HEAD response");
        buffer += *more;
    }
    require(buffer.starts_with("HTTP/1.1 200 OK\r\n") && buffer.contains("Content-Length: 13\r\n") && buffer.ends_with("\r\n\r\n"),
            "HEAD response is wrong");
}

Task<> chunked_and_continue(http::Server &server) {
    auto connection = co_await connect(server);
    co_await write(connection, "POST /chunked HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                               "4\r\nwiki\r\n5\r\npedia\r\n0\r\n\r\n");
    std::string buffer;
    auto chunked = co_await read_response(connection, buffer);
    require(chunked.body == "POST /chunked  wikipedia", "chunked body was not reassembled");

    const std::string body(2 * 1024 * 1024, 'z');
    co_await write(connection,
                   "POST /big HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n");
    auto interim = co_await read_response(connection, buffer);
    require(interim.status == 100, "no 100 Continue before the body");
    co_await write(connection, body);
    auto big = co_await read_response(connection, buffer);
    require(big.status == 200 && big.body.size() == std::string_view("POST /big  ").size() + body.size(), "large body was not received");
}

Task<> limits(http::Server &server) {
    {
        auto connection = co_await connect(server);
        co_await write(connection, "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 4096\r\n\r\n");
        std::string buffer;
        auto response = co_await read_response(connection, buffer);
        require(response.status == 413 && response.head.contains("Connection: close"), "oversized body was not refused");
        co_await expect_closed(connection);
    }
    {
        auto connection = co_await connect(server);
        co_await write(connection, "GET /x HTTP/1.1\r\nHost: x\r\nX-Big: " + std::string(2048, 'h') + "\r\n\r\n");
        std::string buffer;
        auto response = co_await read_response(connection, buffer);
        require(response.status == 431, "oversized headers were not refused");
    }
    {
        auto connection = co_await connect(server);
        co_await write(connection, "NOT HTTP AT ALL\r\n\r\n");
        std::string buffer;
        auto response = co_await read_response(connection, buffer);
        require(response.status == 400, "garbage was not refused");
    }
}

/// The first request waits until the second has been handled: the two
/// connections must be served concurrently.
Task<> concurrent(http::Server &server) {
    http::Client client;
    auto slow = [&]() -> Task<> {
        auto request = client.on().get(url(server, "/slow"));
        auto response = co_await std::move(request).send();
        require(response.has_value() && response->text() == "slow", "slow request failed");
    };
    auto fast = [&]() -> Task<> {
        auto request = client.on().get(url(server, "/fast"));
        auto response = co_await std::move(request).send();
        require(response.has_value() && response->text() == "fast", "fast request failed");
    };
    co_await when_all(slow(), fast());
}

/// stop() lets the request in flight finish (with Connection: close), closes
/// the idle connection, and serve() returns.
Task<> graceful_stop(http::Server &server, Event &entered, Event &release) {
    auto idle = co_await connect(server);
    auto busy = co_await connect(server);
    co_await write(busy, "GET /block HTTP/1.1\r\nHost: x\r\n\r\n");
    co_await entered.wait();

    server.stop();
    co_await expect_closed(idle);
    release.set();

    std::string buffer;
    auto response = co_await read_response(busy, buffer);
    require(response.status == 200 && response.body == "unblocked" && response.head.contains("Connection: close"),
            "the request in flight was not finished");
    co_await expect_closed(busy);
}

template <typename Body>
void scenario(const char *name, Body body) {
    std::printf("scenario %s\n", name);
    EventLoop loop;
    auto task = body();
    loop.schedule(task);
    loop.run();
    std::ignore = task.value();
}

} // namespace

int main() {
    scenario("client basics", []() -> Task<> {
        auto server = listen();
        co_await exercise(server, echo, client_basics(server));
    });

    scenario("pipelined keep-alive", []() -> Task<> {
        auto server = listen();
        co_await exercise(server, echo, pipelined_keep_alive(server));
    });

    scenario("chunked and 100-continue", []() -> Task<> {
        auto server = listen({.max_body_bytes = 4 * 1024 * 1024});
        co_await exercise(server, echo, chunked_and_continue(server));
    });

    scenario("limits", []() -> Task<> {
        auto server = listen({.max_header_bytes = 1024, .max_body_bytes = 1024});
        co_await exercise(server, echo, limits(server));
    });

    scenario("concurrent connections", []() -> Task<> {
        auto server = listen();
        Event fast_done;
        auto handler = [&](http::ServerRequest request) -> Task<http::ServerResponse> {
            if (request.path == "/slow") {
                co_await fast_done.wait();
                co_return http::ServerResponse::text(200, "slow");
            }
            fast_done.set();
            co_return http::ServerResponse::text(200, "fast");
        };
        co_await exercise(server, handler, concurrent(server));
    });

    scenario("graceful stop", []() -> Task<> {
        auto server = listen();
        Event entered;
        Event release;
        auto handler = [&](http::ServerRequest) -> Task<http::ServerResponse> {
            entered.set();
            co_await release.wait();
            co_return http::ServerResponse::text(200, "unblocked");
        };
        auto served = co_await when_all(server.serve(handler), graceful_stop(server, entered, release));
        require(served.has_value(), "serve() failed");
    });

    return 0;
}
