#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/oneshot.h>
#include <lighter/async/runtime/sync.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/cancellation.h>

namespace {

using namespace lighter;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

/// A worker thread sends; the waiter resumes on the loop with the value.
Task<> delivered_from_thread() {
    Oneshot<std::string> result;
    std::thread worker([sender = result.sender()]() mutable { std::move(sender).send("from the worker"); });
    auto value = co_await result.wait();
    worker.join();
    require(value.has_value() && *value == "from the worker", "the value did not arrive");
}

/// A value sent before anyone waits is kept.
Task<> sent_before_wait() {
    Oneshot<int> result;
    std::thread worker([sender = result.sender()]() mutable { std::move(sender).send(42); });
    worker.join();
    auto value = co_await result.wait();
    require(value.has_value() && *value == 42, "an early value was lost");
}

/// A Sender dropped without sending reports a broken promise.
Task<> dropped_sender() {
    Oneshot<int> result;
    std::thread worker([sender = result.sender()]() mutable { std::ignore = std::move(sender); });
    worker.join();
    auto value = co_await result.wait();
    require(!value.has_value() && value.error() == Error::k_broken_pipe, "a dropped Sender was not reported");
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
    scenario("delivered from a thread", delivered_from_thread);
    scenario("sent before wait", sent_before_wait);
    scenario("dropped sender", dropped_sender);

    scenario("cancelled waiter", []() -> Task<> {
        Oneshot<int>::Sender late;
        {
            Oneshot<int> result;
            late = result.sender();
            CancellationSource source;
            source.cancel();
            auto cancelled = co_await with_token(result.wait(), source.token()).catch_cancel();
            require(cancelled.is_cancelled(), "the waiter was not cancelled");
        }
        // The Oneshot and its waiter are gone; delivering now must be harmless.
        std::thread worker([sender = std::move(late)]() mutable { std::move(sender).send(7); });
        worker.join();
    });

    return 0;
}
