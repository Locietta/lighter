# Test coverage findings

Input for the test audit (lighter + liminal). Status as of `584438d`.

## No direct tests at all

- **Runtime combinators**: `WhenAll` / `WhenAny` (variadic and range),
  including error-vs-cancel precedence, `catch_cancel` interception, a winner
  racing an external cancel, synchronous completion during arming.
- **Sync primitives**: `Mutex`, `Semaphore`, `ConditionVariable`, `Event::interrupt`;
  the abandon-grant path in `AsyncNode::resume` (waiter granted, then
  cancelled before the deferred resume runs).
- **TaskGroup**: eager child reclamation and tombstone compaction, `spawn`
  after decision, `join` from an already-cancelled parent.
  (`test-task-group` has 89 lines covering basics only.)
- **`or_fail` / `fail` error hooks** across nested Tasks with exceptions.
- **I/O**: TCP stream, UDP, process, watcher (idle/prepare/check), acceptor.
- **Containers**: SmallVector (only 3 relocation cases in `test-relocation`),
  SmallString, StringRef. UT-1..UT-5 would all be caught by basic tests.

## Thin coverage

- **test-http** (63 lines): a single GET. Missing: cancelling a buffered
  request, curl timeouts and error paths, many concurrent transfers on one
  Manager, POST bodies, redirects, multiple loops/threads.
- **test-http-stream**: no 3xx/304 (HTTP-3), no move-assignment (HTTP-4).
- **test-http-server**: only ever one idle connection. Missing: read/idle
  timeouts, `max_connections`, `stop()` with 2+ idle connections (HTTP-1),
  `Connection: close`, HTTP/1.0, Upgrade/CONNECT, body sent without waiting
  for 100-continue, cancelled `serve()`.
- **test-oneshot**: no detached sender racing loop teardown (RT-7), no repeated
  `wait()`, no Oneshot destroyed before the send arrives.
- **test-transcode**: no output > 4096 bytes (the E2BIG refill loop), no
  `to_utf8`, no UTF-16 errors (UT-6), no stateful targets (UT-7).
- **FlatMap**: no throwing types.
- **test-mock**: no swallowed unexpected call (UT-8), no action after a call (UT-9).
- **Lexers**: one smoke test per language; see lexer.md for the gaps.

## Audit questions (to answer per test, lighter and liminal)

1. Does it fail for a plausible real bug, or only check a value that cannot
   change (narrow, tautological, mirrors the implementation)? Remove or merge
   those.
2. Does it belong in lighter, the lexer library, or liminal? Infra behaviour
   tested only from liminal should move here; liminal-specific integration
   should stay there.
3. Which documented feature intentions (doc comments promising behaviour) have
   no test? The doc comments in `runtime/node.cpp`, `when.h`, `task_group.h`,
   `timeout.h` and `sync.h` are a good checklist.
