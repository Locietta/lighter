#pragma once

#include <contracts>
#include <memory>
#include <optional>
#include <utility>

#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/sync.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/error.h>

namespace lighter {

/// Hands one value from any thread to one coroutine on an Event loop.
///
/// Create it on the loop thread, give the Sender to the producer (a worker
/// thread or a native callback) and co_await wait() on the loop:
///
///   Oneshot<int> result;
///   std::thread([sender = result.sender()]() mutable { std::move(sender).send(42); }).detach();
///   int value = co_await result.wait().or_fail();
///
/// The Sender keeps the loop alive until it sends or is destroyed. A Sender
/// destroyed without sending completes wait() with Error::k_broken_pipe. Both
/// ends share the state, so a waiter that was cancelled, or a Oneshot that was
/// destroyed, never leaves the producer's delivery dangling.
template <typename T>
struct Oneshot {
    struct State {
        std::optional<T> value;
        Event ready;
    };

    struct Sender {
        Sender() noexcept = default;

        Sender(const Sender &) = delete;
        Sender &operator=(const Sender &) = delete;

        Sender(Sender &&other) noexcept = default;

        Sender &operator=(Sender &&other) noexcept {
            if (this != &other) {
                abandon();
                state = std::move(other.state);
                relay = std::move(other.relay);
            }
            return *this;
        }

        ~Sender() { abandon(); }

        /// Delivers the value on the loop thread. Thread-safe; consumes the Sender.
        void send(T value) && pre(state != nullptr) {
            relay.send([state = std::move(state), value = std::move(value)]() mutable {
                state->value = std::move(value);
                state->ready.set();
            });
            relay = {};
        }

    private:
        friend struct Oneshot;

        Sender(std::shared_ptr<State> state, Relay relay) noexcept : state(std::move(state)), relay(std::move(relay)) {}

        /// Wakes the waiter without a value, so wait() reports the broken promise.
        void abandon() noexcept {
            if (state) {
                relay.send([state = std::move(state)] { state->ready.set(); });
                relay = {};
            }
        }

        std::shared_ptr<State> state;
        Relay relay;
    };

    explicit Oneshot(EventLoop &loop = EventLoop::current()) : state(std::make_shared<State>()), loop(&loop) {}

    /// The producing end. Loop thread only; call it at most once.
    Sender sender() pre(!handed_out) {
        handed_out = true;
        return Sender(state, loop->create_relay());
    }

    /// Completes with the sent value, or Error::k_broken_pipe if the Sender was
    /// dropped without sending. Loop thread only.
    Task<T, Error> wait() {
        auto shared = state;
        co_await shared->ready.wait();
        if (!shared->value) {
            co_await fail(Error::k_broken_pipe);
        }
        co_return std::move(*shared->value);
    }

private:
    std::shared_ptr<State> state;
    EventLoop *loop;
    bool handed_out = false;
};

} // namespace lighter
