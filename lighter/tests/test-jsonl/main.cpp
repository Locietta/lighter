#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <lighter/codec/json/json.h>
#include <lighter/codec/jsonl/jsonl.h>
#include <lighter/types.hpp>

namespace {

using namespace lighter;

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

enum struct EventKind {
    MESSAGE,
    TOOL_CALL,
};

struct Event {
    EventKind kind = EventKind::MESSAGE;
    std::string payload;
    u64 sequence = 0;

    bool operator==(const Event &) const = default;
};

void test_container_round_trip() {
    std::vector<Event> events{
        {.kind = EventKind::MESSAGE, .payload = "hello", .sequence = 1},
        {.kind = EventKind::TOOL_CALL, .payload = R"({"nested": "json"})", .sequence = 2},
        {.kind = EventKind::MESSAGE, .payload = "multi\nline\ttext", .sequence = 3},
    };

    auto encoded = codec::jsonl::to_string(events);
    require(static_cast<bool>(encoded), "jsonl encode failed");
    // records are separated by exactly one newline, with none trailing
    require(!encoded->ends_with('\n'), "glaze ndjson unexpectedly emits a trailing newline");

    auto decoded = codec::jsonl::parse<std::vector<Event>>(*encoded);
    require(static_cast<bool>(decoded), decoded ? std::string() : std::string(decoded.error().message()));
    require(*decoded == events, "jsonl round trip mismatch");
}

void test_append_and_lines() {
    std::string log;
    auto first = codec::jsonl::append(log, Event{.payload = "one", .sequence = 1});
    require(static_cast<bool>(first), "append failed");
    auto second = codec::jsonl::append(log, Event{.kind = EventKind::TOOL_CALL, .payload = "two", .sequence = 2});
    require(static_cast<bool>(second), "append failed");
    require(log.ends_with('\n'), "append must leave a newline-terminated log");

    // appended log parses as a container
    auto parsed = codec::jsonl::parse<std::vector<Event>>(log);
    require(static_cast<bool>(parsed), "appended log failed to parse");
    require(parsed->size() == 2, "appended log has wrong record count");
    require((*parsed)[1].payload == "two", "appended record mismatch");

    // line iteration decodes record-by-record (the transcript access pattern)
    usize count = 0;
    for (auto line : codec::jsonl::lines(log)) {
        auto event = codec::json::parse<Event>(line);
        require(static_cast<bool>(event), "line failed to decode");
        require(event->sequence == count + 1, "line decoded out of order");
        ++count;
    }
    require(count == 2, "line iteration count mismatch");
}

void test_lines_edge_cases() {
    // blank lines and CRLF endings are tolerated, content is unaffected
    const std::string_view messy = "\r\n{\"kind\":\"MESSAGE\",\"payload\":\"a\",\"sequence\":1}\r\n\n"
                                   "{\"kind\":\"MESSAGE\",\"payload\":\"b\",\"sequence\":2}";
    std::vector<Event> events;
    for (auto line : codec::jsonl::lines(messy)) {
        auto event = codec::json::parse<Event>(line);
        require(static_cast<bool>(event), "messy line failed to decode: " + std::string(line));
        events.push_back(*std::move(event));
    }
    require(events.size() == 2, "messy input line count mismatch");
    require(events[0].payload == "a" && events[1].payload == "b", "messy input payload mismatch");

    usize none = 0;
    for ([[maybe_unused]] auto line : codec::jsonl::lines("\n\r\n\n")) {
        ++none;
    }
    require(none == 0, "blank-only input should yield no lines");
}

void test_decode_errors() {
    auto broken = codec::jsonl::parse<std::vector<Event>>("{\"kind\":\"MESSAGE\"}\n{oops");
    require(!broken, "malformed jsonl unexpectedly parsed");
    require(!broken.error().message().empty(), "jsonl decode error carries no message");
}

} // namespace

int main() {
    test_container_round_trip();
    test_append_and_lines();
    test_lines_edge_cases();
    test_decode_errors();
    return 0;
}
