#include <concepts>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <lighter/async/io/loop.h>
#include <lighter/async/runtime/task.h>
#include <lighter/async/vocab/outcome.h>

namespace {

using namespace lighter;

using TestOutcome = Outcome<std::string, int, bool>;

static_assert(std::same_as<decltype(std::declval<TestOutcome &>().value()), std::string &>);
static_assert(std::same_as<decltype(std::declval<const TestOutcome &>().value()), const std::string &>);
static_assert(std::same_as<decltype(std::declval<TestOutcome &&>().value()), std::string &&>);
static_assert(std::same_as<decltype(std::declval<const TestOutcome &&>().value()), const std::string &&>);
static_assert(std::same_as<decltype(std::declval<TestOutcome &>().error()), int &>);
static_assert(std::same_as<decltype(std::declval<const TestOutcome &>().error()), const int &>);
static_assert(std::same_as<decltype(std::declval<TestOutcome &&>().error()), int &&>);
static_assert(std::same_as<decltype(std::declval<const TestOutcome &&>().error()), const int &&>);
static_assert(std::same_as<decltype(std::declval<TestOutcome &>().cancellation()), bool &>);
static_assert(std::same_as<decltype(std::declval<const TestOutcome &>().cancellation()), const bool &>);
static_assert(std::same_as<decltype(std::declval<TestOutcome &&>().cancellation()), bool &&>);
static_assert(std::same_as<decltype(std::declval<const TestOutcome &&>().cancellation()), const bool &&>);

// The value-only specialization (Task<T>'s result) keeps the same contract.
using ValueOutcome = Outcome<std::string, void, void>;

static_assert(std::same_as<decltype(std::declval<ValueOutcome &>().value()), std::string &>);
static_assert(std::same_as<decltype(std::declval<const ValueOutcome &>().value()), const std::string &>);
static_assert(std::same_as<decltype(std::declval<ValueOutcome &&>().value()), std::string &&>);
static_assert(std::same_as<decltype(std::declval<const ValueOutcome &&>().value()), const std::string &&>);
static_assert(std::same_as<decltype(*std::declval<ValueOutcome &>()), std::string &>);
static_assert(std::same_as<decltype(*std::declval<ValueOutcome &&>()), std::string &&>);

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

void test_value() {
    Outcome<std::string, int, bool> outcome("value");
    require(outcome.has_value(), "value outcome has the wrong state");
    require(outcome.value() == "value", "value accessor returned the wrong value");
    require(*outcome == "value", "dereference returned the wrong value");
    require(outcome->size() == 5, "arrow returned the wrong value");
}

void test_error() {
    Outcome<std::string, int, bool> outcome(outcome_error(42));
    require(outcome.has_error(), "error outcome has the wrong state");
    require(outcome.error() == 42, "error accessor returned the wrong error");
}

void test_cancellation() {
    Outcome<std::string, int, bool> outcome(outcome_cancel(true));
    require(outcome.is_cancelled(), "cancelled outcome has the wrong state");
    require(outcome.cancellation(), "cancellation accessor returned the wrong value");
}

void test_value_only_move_only() {
    Outcome<std::unique_ptr<int>, void, void> outcome(std::make_unique<int>(7));
    require(*outcome.value() == 7, "lvalue access must not move the value out");
    require(outcome.value() != nullptr, "lvalue access left the value in place");
    require(*outcome->get() == 7, "arrow must reach the stored value");
    auto moved = std::move(outcome).value();
    require(moved && *moved == 7, "rvalue access must move the value out");
}

Task<std::unique_ptr<int>> make_box(int value) { co_return std::make_unique<int>(value); }

Task<> await_box(int &seen) {
    auto box = co_await make_box(41);
    seen = *box + 1;
}

/// Awaiting a Task<T> with no error channel must move T, so move-only
/// results such as std::unique_ptr (or lighter::Tcp) compile.
void test_task_move_only_result() {
    int seen = 0;
    auto task = await_box(seen);
    auto boxed = make_box(5);
    EventLoop loop;
    loop.schedule(task);
    loop.schedule(boxed);
    loop.run();
    require(seen == 42, "awaited move-only result has the wrong value");
    auto result = boxed.result();
    require(result && *result == 5, "Task::result must move a move-only value out");
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--violate") {
        TestOutcome outcome(outcome_error(42));
        std::ignore = outcome.value();
        return 1;
    }

    test_value();
    test_error();
    test_cancellation();
    test_value_only_move_only();
    test_task_move_only_result();
    return 0;
}
