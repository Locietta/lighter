# Runtime findings

Scope: `lighter/async/runtime/*`, `lighter/async/io/loop.*`,
`lighter/async/vocab/{cancellation,outcome,error}.h`, `lighter/utils/config.h`.

## RT-1 `CancellationSource::cancel()` is not cross-thread safe

- Severity: high (if called off-loop) · Verification: verified
- Where: `vocab/cancellation.h:17-23` (`State::cancel`), `runtime/sync.cpp:40-49` (`resume_waiter`)
- Problem: the `atomic<bool>` suggests thread safety, but `cancel()` calls
  `Event::set()`, which resumes waiters through `EventLoop::current()`. On a
  thread with no loop that is a contract violation in dev builds and a null
  dereference in release; on a thread running a different loop it queues the
  resume on the wrong loop.
- Fix: either document `cancel()` as loop-affine (and drop the atomic), or
  capture a `Relay` in the state and post the `event.set()` to the owning loop.

## RT-2 Destroying a `CancellationSource` cancels its tokens

- Severity: design · Verification: verified
- Where: `vocab/cancellation.h:81`
- Problem: opposite of `std::stop_source` / .NET. A source that goes out of
  scope while its token is still in use cancels that work.
- Fix: decide, then document in the type's comment. If kept, say why.

## RT-3 No scoped lock guard; cancellation skips a manual `unlock()`

- Severity: medium · Verification: verified
- Where: `runtime/sync.h:72-130` (`Mutex`)
- Problem: cancellation finalizes a frame at its suspension point without
  running further user code. `co_await m.lock(); co_await io(); m.unlock();`
  never unlocks if `io()` is cancelled, so the mutex stays locked forever.
  Destructors do run when the frame is destroyed, but there is no guard type.
- Fix: add a scoped guard returned by `co_await m.scoped_lock()` whose
  destructor unlocks. Same for `Semaphore`.

## RT-4 `EventLoop::schedule(lvalue)` relies on the caller's Task lifetime

- Severity: low · Verification: verified
- Where: `io/loop.h:159-169`
- Problem: an lvalue Task is not made root; destroying it before `run()`
  returns destroys a live frame. `lighter::run()` uses this correctly, but
  nothing prevents misuse.
- Fix: document, or add a contract in `~Task` that the frame is not RUNNING.

## RT-5 `Error` is a bare `i32` with repurposed errno codes

- Severity: design · Verification: verified
- Where: `vocab/error.h`, `runtime/timeout.h:93`, `runtime/interrupt.cpp:180`
- Problem:
  - `with_timeout` reports every timeout as `k_connection_timed_out`.
  - `k_connection_already_in_progress` is used as a generic "busy".
  - `k_operation_aborted` is the magic value -114514; there is no named ECANCELED.
  - no room for context (message, source location, HTTP status).
- Fix: add lighter-specific codes (`k_timed_out`, `k_busy`) at least; consider
  an optional context payload once HTTP needs it.

## RT-6 Workarounds for compilers that cannot build lighter

- Severity: design · Verification: verified
- Where: `utils/config.h` (MSVC ASan / no_unique_address / Windows ASan),
  `runtime/node.cpp` (`LIGHTER_WORKAROUND_MSVC_COROUTINE_ASAN_UAF` paths),
  `node.h:88-90` (clang -O3 comment), `vocab/cancellation.h:120-123` and
  `runtime/timeout.h:79-82` (MSVC fall-through guards)
- Problem: every runtime header includes `<contracts>` and the build needs
  `-freflection`, so only GCC 16 can compile lighter. These are leftovers from
  liminal and make the code look like it supports compilers it does not.
- Fix: delete, or keep with a note saying they stay for a future clang port.

## RT-7 `Relay::release_hold` touches the loop after dropping its hold

- Severity: high (detached senders) · Verification: verified
- Where: `io/loop.cpp:86-93` (`release_hold`), `:113-122` (`release`)
- Problem: `count.fetch_sub` happens before `uv::async_send`. Between the two,
  `on_relay` can see `count == 0`, unref the async handle, let `uv_run` return
  and the owner destroy the `EventLoop`; the following `async_send` then hits
  freed memory. `oneshot.h:21` shows a `.detach()`ed sender, which is exactly
  this case. Tests always `join()` and hide it.
- Fix: `async_send` first, then `fetch_sub` as the last access to `self`.

## RT-8 `CancellationToken::wait()` does not keep its state alive

- Severity: low · Verification: verified
- Where: `vocab/cancellation.h:40-52, 65`
- Problem: the returned Task is a member coroutine of `State` and holds a raw
  `this`, not the `shared_ptr`. If every token and the source die while the
  Task is still pending, it dangles. `with_token` is safe because it copies
  the tokens into its own frame.
- Fix: make `wait()` a free coroutine taking the `shared_ptr` by value.

## RT-9 No async cleanup or shielding under cancellation

- Severity: design · Verification: verified
- Problem: a cancelled task gets no chance to run async cleanup (flush, send a
  close frame, release a remote lock). Trio has shielded cancel scopes for
  this. Today the only escape is `catch_cancel()` around the work.
- Fix: decide whether a `shield(task)` combinator is wanted.

## RT-11 Failures of detached root tasks are silently discarded

- Severity: medium · Verification: verified
- Where: `io/loop.h:159-169` (`schedule(rvalue)` sets `root`),
  `runtime/task.h:98-129` (`TransitionAwait`), `runtime/node.cpp:328-341` (`finalize`)
- Problem: a root task that throws stores the exception in its promise, ends
  FAILED, and `finalize()` destroys the frame with no parent to deliver to. The
  exception (or a structured Error from `Task<T, Error>`) is never reported. Found
  through the tests: `require()` failures inside rvalue-scheduled tasks in
  test-task-group, test-timeout, test-timer and test-control are swallowed; those
  tests only pass because they check flags after `run()`.
- Fix: decide a policy for unobserved root failures, e.g. rethrow from `run()`,
  call a loop-level handler, or `panic` in dev builds. At minimum log them.

## RT-10 `~EventLoop` never destroys suspended root task frames

- Severity: low · Verification: verified
- Where: `io/loop.cpp:258-296`
- Problem: handles are force-closed, but root frames still suspended are
  never resumed or destroyed, so their locals leak.
- Fix: decide whether loop teardown should cancel and drain the task tree.
