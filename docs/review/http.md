# HTTP findings

Scope: `lighter/http/*`, `lighter/async/runtime/oneshot.h`.

Strengths worth keeping: the `InflightRequestState` hub with a weak
`retain_inflight_request`; curl callbacks never run user code (only
`Event::set`, which defers); pause-based backpressure with water marks; the SSE
parser handles CR/LF/CRLF across chunks, BOM and multi-line data; the server
runs llhttp in strict mode and enforces `max_body_bytes` for both
Content-Length and chunked bodies.

## Bugs

### HTTP-1 `Server::stop()` mutates `connections` while iterating it

- Severity: high · Verification: verified
- Where: `http/server.cpp:411-422` (`Server::stop`), `:188` (`~ServerConnection`)
- Chain: `stream.stop()` → `reader->complete()` inline (`io/stream.cpp:402`) →
  `read_chunk` / `with_timeout` (timer cancel is synchronous) → `read_next`
  returns CLOSE → `serve_connection` returns → TaskGroup reclaims the child
  frame eagerly (`runtime/node.cpp:468-477`) → `~ServerConnection` erases
  itself from `server->connections`, still inside the loop.
- Effect: with 3 idle connections one is skipped (stays open until
  `idle_timeout`); with 4+ a freed `ServerConnection` is read.
- Fix: copy the idle connections into a local list, then stop them.
- Test: `stop()` with several idle keep-alive connections.

### HTTP-2 Slowloris: per-chunk read timeout, no write timeout

- Severity: high · Verification: verified (read side), reported (write side)
- Where: `http/server.cpp:215` (`with_timeout` per `read_chunk`),
  `:267-270` (`send` has no timeout); doc at `http/server.h:56`
- Problem: `read_timeout` is documented as "how long a request may take to
  arrive once its first byte has" but resets each chunk; one byte every 29 s
  holds a slot forever, and 64 such connections block the server. A client
  that never reads a large response holds its slot and stalls `stop()`.
- Fix: a per-request deadline (compute remaining budget per read); a write
  timeout. Depends on IO-5 for write cancel to complete.

### HTTP-3 Streaming request + unfollowed 3xx (e.g. 304) fails

- Severity: medium · Verification: reported
- Where: `http/inflight_request.cpp:175-176`, `http/common.h:63` (follow
  defaults to true), `http/streaming.cpp:72-75`
- Problem: every 3xx is treated as intermediate when `follow` is set, but curl
  does not follow 304 or a 300 without `Location`. Headers never finalize and
  the stream fails with "stream transfer ended unexpectedly".
- Fix: treat a 3xx as intermediate only if curl will actually follow it.

### HTTP-4 `StreamingResponse` move-assignment skips the abort path

- Severity: medium · Verification: reported
- Where: `http/streaming.h:53` (`= default`); `EventStream`, `TextStream` inherit it
- Problem: the old state is dropped without `detach_from_multi()`;
  `active_requests` drifts and `header_lines` is freed before the easy handle.

### HTTP-5 A completion can land before `await_suspend` attaches

- Severity: medium (low reachability) · Verification: reported
- Where: `RequestAwaiter` constructor publishes `state->awaiter` early;
  `manager.cpp:200-201` drain loop; `execute.cpp:128-131`
- Problem: during `start()`, the drain loop resumes other coroutines inline;
  one may start a request whose `socket_action` completes A, whose parent is
  still null, so `IoOp::complete()` dereferences null.
- Fix: guard on `get_parent() != nullptr`, or publish the awaiter only after attach.

### HTTP-6 Shared `CURLSH` without lock callbacks

- Severity: medium · Verification: reported
- Where: `http/client.cpp:37-46`; `Client` is copyable (`client.h:25`)
- Problem: cookies, DNS and SSL sessions are shared across managers (one per
  loop, i.e. per thread) with no `CURLSHOPT_LOCKFUNC`: a data race in libcurl.
- Fix: add lock/unlock callbacks, or make the share per loop.

### HTTP-7 Detached oneshot sender vs loop teardown

- Same root cause as RT-7. The `.detach()` example in `oneshot.h:21` is the
  trigger; tests always `join()`.

## Design

- **HTTP-D1 Re-entrancy.** `consume()` calls `drive_timeout()`
  (`streaming.cpp:157`) and `start()` drains too, so unrelated coroutines can
  resume inline inside a consumer.
- **HTTP-D2 `InflightRequestState::mgr` is a raw pointer.** A
  `StreamingResponse` or frame outliving its `EventLoop` dereferences a dead Manager.
- **HTTP-D3 `CURLOPT_TIMEOUT_MS` keeps running while paused**, so a slow SSE
  consumer gets killed by the total timeout.
- **HTTP-D4 Chunk-view stability depends on `reserve(256K+32K)`**
  (`inflight_request.cpp:114`); a user `CURLOPT_BUFFERSIZE` breaks it.
- **HTTP-D5 `consume()` erases from the front every call** (O(n) memmove);
  `read_offset` is effectively dead.
- **HTTP-D6 SSE line/event size is unbounded** (`sse.cpp:55`): a hostile server
  grows memory without limit.
- **HTTP-D7 Server header limit counts token bytes**, not wire bytes or header count.
- **HTTP-D8 Chunked trailers are merged into `request.headers`.**
- **HTTP-D9 No CR/LF check on response header names/values**
  (`server.cpp:250`): a handler echoing input allows response splitting.
- **HTTP-D10 413/431/400 close immediately with unread input**, so a TCP RST
  may swallow the response. Drain or half-close first.
- **HTTP-D11 Upgrade/CONNECT.** The 501 path (`server.cpp:238`) is probably
  unreachable because `on_message_complete` always returns `HPE_PAUSED`;
  such requests may reach the handler as normal ones. Low confidence, untested.
- **HTTP-D12 `server.acceptor = {}` runs inside `Acceptor::stop()`'s stack**
  (`server.cpp:351`); safe only because `stop()` touches nothing after `deliver`.

## Nits

- `sse.cpp:92`: `last_event_id()` updates before dispatch; per spec it changes on dispatch.
- Out-of-range `retry` silently dropped; invalid UTF-8 in SSE data passed through.
- curl timeout 0 rounded up to 1 ms (`manager.cpp:283-285`).
- `ensure_socket` failure stalls the transfer silently until timeout (`manager.cpp:229-232`).
- `Oneshot::wait()` twice returns a moved-from `T` (`oneshot.h:96`); `wait()` on
  a moved-from Oneshot dereferences null.
- `Sender::abandon()` is `noexcept` but allocates; a Sender never sent nor
  destroyed keeps the loop alive forever.
- HEAD with an empty handler body sends `Content-Length: 0`.
