# Test audit (lighter 0.4.0, 2026-10-12)

Each test function was judged on whether it would fail for a plausible real
regression in lighter's own code. Temporary working notes; bug IDs refer to the
other files in this directory. The audit was done by a reviewer and applied
selectively: a test that would catch a mistake in lighter's wrapper around a
third-party library (glaze, llhttp) is kept even if the heavy lifting is
third-party.

## Applied

| test                                      | change  | why                                                                                                                                                                  |
| ----------------------------------------- | ------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `test-jsonl` `test_heterogeneous_records` | removed | passes whatever glaze does with unknown keys (both branches fall through), so it only re-runs `append` + `lines`                                                     |
| `test-mock` `test_automatic_port`         | removed | same `calls` / `returns()` / move-only `then_returns` on the same generated handle as `test_ordered_and_move_only_results` and `test_behavior_and_verification`      |
| `test-timer` `sleeps_at_least`            | removed | wall-clock assertion (`measured >= 15ms`) against libuv's cached loop time, which the style guide forbids; `test-timeout` `value_wins` already shows `sleep()` waits |
| `test-sse` OpenAI / Anthropic streams     | trimmed | dropped glaze decoding of provider payloads (liminal's concern); now compares `data` byte-for-byte, which is stricter                                                |
| `test-http-stream` scenario 1             | trimmed | same: compares reassembled payloads instead of decoding them                                                                                                         |
| `test-terminal` `check_terminal_basics`   | trimmed | dropped a check of aggregate initialisation of `TerminalEvent`                                                                                                       |
| `test-control` `check_support_table`      | trimmed | dropped the `enum_name` check (covered by `test-enum-name-reflection`)                                                                                               |
| `test-fs`                                 | trimmed | dropped `static_assert`s on move-constructibility; the test body moves the handle and would not compile otherwise                                                    |

## Not applied (reviewer suggestions kept on purpose)

- `test-json` `test_prettify_minify` and the mistyped-JSON case: they exercise
  lighter's wrappers and options (`prettify` vs `minify` wiring, error
  reporting), so they can fail for a lighter bug.
- `test-jsonl` trailing-newline assertion: pins the output format lighter
  exposes from `to_string`; consumers concatenate it.
- `test-control` per-platform support rows: they pin which controls each
  platform supports, a contract consumers rely on.
- `test-flat-map` relocatability `static_assert`, `test-outcome`
  `test_error` / `test_cancellation`, `test-timeout` `native_clock_duration`:
  small, cheap, and test public API; merging them would only shuffle code.

## Findings that need a decision

- **Root task failures are swallowed (RT-11).** `require()` throws inside
  rvalue-scheduled tasks in test-task-group, test-timeout, test-timer and
  test-control are dropped by the runtime; those tests pass only because they
  check flags after `run()`. Fix the runtime policy first, then let tests rely on it.
- **`test-outcome` `--violate` branch is dead.** `xmake.lua` never passes the
  argument. Either register it as an expected-failure test or delete it.
- **Names that claim more than they check.** oneshot "cancelled waiter" never
  parks; `yields_after_pending_work` never checks order; control
  `wait_keeps_loop_alive` releases the semaphore before `next()` suspends.
- `tests.md` said FlatMap had no throwing-type tests; it does
  (`preserves_storage_when_copy_growth_fails`).

## Missing tests

"fails today: X" means the test would fail now because of known bug X.

### P1: runtime, cancellation, memory safety

1. **WhenAll error.** The first child Error cancels running siblings, the
   result carries it, and every sibling is terminal before the parent resumes.
2. **Error vs Cancellation precedence.** A child fails in the same turn as an
   outer cancel or after a WhenAny winner: the Error is reported.
3. **WhenAny basics.** Winner index, losers cancelled, void children,
   range overloads, empty `WhenAll` ready, empty `WhenAny(range)` throws.
4. **Synchronous completion during arming.** Later children are never
   resumed and nothing is delivered twice.
5. **`catch_cancel` inside aggregates.** Self-cancelling child counts as the
   winner; WhenAll reports the first cancelled child's payload.
6. **Mutex.** FIFO hand-off, a cancelled queued waiter is skipped, and the
   abandon-grant path releases the lock to the next waiter.
7. **Semaphore and ConditionVariable.** `release(n)`, cancelled-after-grant
   returns the permit, `acquire()` inside WhenAll, `notify_one` / `notify_all`,
   cancelled `cv.wait` under `catch_cancel` resumes without the lock.
8. **Event::interrupt and reset.**
9. **TaskGroup failure.** Siblings cancelled, `join()` returns every Error,
   Errors survive a racing `cancel()`, exceptions rethrown from `join()`.
10. **TaskGroup lifecycle.** `spawn()` after cancel/failure returns false;
    `join()` from a cancelled parent; 10k sequential children stay bounded.
11. **`or_fail` / `fail`.** Two-level propagation without running parent code;
    throwing child; `or_fail(Outcome)`.
12. **A cancelled task that then fails ends FAILED**, not CANCELLED.
13. **Cancellation checkpoint.** A task cancelled during its synchronous
    segment never starts its next child, aggregate or I/O op.
14. **`with_token`.** Several tokens; forwarding of value and Error; source
    destruction cancels tokens (pins RT-2); pending `token.wait()` after all
    tokens die (fails today under ASan: RT-8).
15. **Cross-thread cancel** (fails today: RT-1; decide semantics first).
16. **Oneshot.** Detached sender racing loop teardown under ASan (fails today:
    RT-7); a waiter that actually parks, then is cancelled; move-assigning over
    a live Sender.
17. **Relay and `yield()`.** FIFO, moved-from `send()` is a no-op, `KeepAlive::NO`
    does not keep `run()` alive; `yield()` ordering against deferred resumes and
    a 0 ms timer.
18. **A repeating timer survives a lost WhenAny race** (fails today: IO-1).
19. **Unobserved root task failure is reported** (fails today: RT-11).

### P2: I/O and HTTP

20. **TCP Stream.** Echo round trip over every read API and `try_write`;
    `stop()` aborts a read; concurrent read rejected (fails today: IO-7); peer
    reset between reads is an error (fails today: IO-10).
21. **Write cancel** against a peer that never reads returns (fails today: IO-5).
22. **Acceptor** `stop()` semantics and concurrent accept rejection.
23. **Owner destroyed while awaited** (fails today: IO-4).
24. **UDP** round trip; `stop_recv()` completes a pending recv (fails today: IO-8).
25. **Process.** stdout pipe, exit code/signal, `kill_tree` reaches a
    grandchild, cancelling `wait()` kills the child.
26. **Idle/Prepare/Check watchers and `queue()`.**
27. **fs** round trip, `read_file` with `max_bytes`, `mkstemp`; cancel race
    does not leak fds (fails today: IO-3).
28. **Terminal decoder livelock** on `"\xC3\r"` / `"\xC3\x1b"` (fails today: IO-2).
29. **Control / InterruptSource.** Second source for a watched kind rejected;
    two SIGINTs coalesce in `next()` but count twice; overlapping `next()` busy;
    sticky token.
30. **Server `stop()` with 4+ idle connections** (fails today: HTTP-1).
31. **Server timeouts and limits.** `idle_timeout`, `max_connections`,
    whole-request `read_timeout` against a trickle (fails today: HTTP-2).
32. **Server lifecycle.** `stop()` before `serve()`; cancelling `serve()`;
    `Connection: close` and HTTP/1.0; Upgrade/CONNECT (HTTP-D11).
33. **Client.** Cancel a buffered request; connection refused; `.timeout()`;
    redirects; 10+ concurrent transfers on one Manager.
34. **Streaming.** 304 / 300 without Location (fails today: HTTP-3);
    move-assign over a live stream (fails today: HTTP-4); deterministic
    pause/resume at the high-water mark.
35. **EventStream / SSE.** `last_event_id()` / `retry_ms()`, unterminated last
    event dropped at EOF, `id:` applied at dispatch, BOM only at start.
36. **Shared Client across threads** under TSan (fails today: HTTP-6).

### P3: utilities

37. **SmallVector** UT-1..UT-5 (all fail today); basic SmallString/StringRef
    tests, or delete them (UT-D11).
38. **Transcoder.** Output over 4096 bytes, `to_utf8`, UTF-16/GBK error
    alignment (fails today: UT-6), stateful targets (fails today: UT-7).
39. **Mock.** Swallowed unexpected call (fails today: UT-8); action added after
    a call (fails today: UT-9).
40. **Smaller utilities.** `enum_name` duplicate values and flags; FlatMap
    throwing constructor in `try_emplace`; panic hook runs once (death test);
    smoke tests for `dump_dot` and `lighter::run`.
