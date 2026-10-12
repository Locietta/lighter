# I/O layer findings

Scope: `lighter/async/io/*`, `lighter/async/detail/*`, `lighter/async/vocab/ringbuffer.*`.

The core awaiter mechanics are sound: cancellation always waits for libuv's
completion instead of abandoning the request, and no use-after-free was found
while the owning handle object is alive. The problems are in owner lifetime,
cancel/stop semantics and a few unguarded paths.

## Bugs

### IO-1 Cancelling one `wait()` stops a shared Timer/Idle/Prepare/Check

- Severity: high · Verification: verified
- Where: `io/watcher.cpp:53-67` (`BasicTickAwait::on_cancel`)
- Problem: `on_cancel` calls `timer_stop` / `idle_stop` on the shared handle.
  In `loop { when_any(ticker.wait(), other) }`, the first time `other` wins
  the repeating ticker stops and every later `wait()` hangs.
- Fix: on cancel, only disarm the waiter; leave the handle running.

### IO-2 Terminal input decoder livelock

- Severity: high · Verification: reported
- Where: `detail/terminal_input_decoder.cpp:345-356`
- Problem: input starting with an incomplete UTF-8 lead byte followed by a
  control byte or ESC (e.g. `"\xC3\r"`): `complete_prefix_len` returns 0,
  nothing is consumed, later bytes only append, and `escape_pending()` is
  false so the timeout flush never fires.

### IO-3 fs cancel/complete race leaks resources

- Severity: high · Verification: verified
- Where: `io/fs.cpp:36-39` (`FsOp::on_cancel`), `:101-116` (`after_cb`)
- Problem: when `uv_cancel` fails because the request is already running, the
  operation completes successfully but is already CANCELLED; the parent
  finalizes and `out` is dropped. Leaks the fd from `fs::open`, the fd and temp
  file from `mkstemp`, the `uv_dir_t` from `opendir`. Trigger: `with_timeout`
  around `fs::open` on slow storage.
- Fix: in `after_cb`, if CANCELLED and the result owns a resource, release it.

### IO-4 Destroying a handle owner while a task awaits it

- Severity: high · Verification: reported (mechanism plausible)
- Where: `detail/libuv_helper.h:745` (`UniqueHandleImpl::destroy`);
  awaiters in `io/stream.cpp:45-55`, `io/acceptor.cpp:70-76`,
  `io/udp.cpp:117-127`, `io/process.cpp:119-125`
- Problem: `uv_close` never fires the pending read/accept/recv/exit callback,
  so the task hangs; cancelling it later dereferences the `Self*` already
  freed by the close callback.
- Fix: `destroy` aborts any armed waiter (complete with `k_operation_aborted`)
  before closing.

### IO-5 Cancelling a stream write can wait forever

- Severity: high · Verification: reported
- Where: `io/stream.cpp:238-245` (`StreamWriteAwait::on_cancel` is empty)
- Problem: cancel waits for `on_write`; if the peer never reads, timeouts and
  server shutdown never complete. Related to HTTP-2.
- Fix: close the handle on write cancel (the stream is unusable anyway).

### IO-6 libuv `UV_ECANCELED` becomes task cancellation

- Severity: medium · Verification: reported
- Where: `mark_cancelled_if` in `io/stream.cpp:80,182,252`, `io/udp.cpp:142,238`
- Problem: when libuv reports ECANCELED for reasons other than our cancel
  (handle closed with a pending write), the task is marked cancelled, which
  propagates upward and cancels TaskGroup siblings.
- Fix: surface it as an Error unless our own cancel caused it.

### IO-7 Two concurrent reads on one `Stream` orphan the first

- Severity: medium · Verification: verified
- Where: `io/stream.cpp:115, 206` (`reader.arm` with no `has_waiter()` check)
- Problem: the second read overwrites the waiter; the first hangs. `write`,
  `accept`, `recv` and `wait` all check.
- Fix: same busy check as the others.

### IO-8 `Udp::stop_recv()` leaves a pending `recv()` hung

- Severity: medium · Verification: reported
- Where: `io/udp.cpp:479-487`
- Fix: abort the waiter with `k_operation_aborted`, as `Stream::stop` does.

### IO-9 `Stream::write` truncates buffers over 4 GiB

- Severity: low · Verification: reported
- Where: `io/stream.cpp:276` (`static_cast<u32>(size)`)
- Fix: split into multiple `uv_buf_t`, or fail. `try_write` already guards.

### IO-10 Read errors dropped when no reader is armed

- Severity: medium · Verification: reported
- Where: `io/stream.cpp:75-85`
- Problem: `error_code` is only latched inside `has_waiter()`. A reset between
  reads becomes EOF (POSIX) or `ENOTCONN` (Windows).
- Fix: latch the error unconditionally.

### IO-11 Windows HANGUP (`CTRL_CLOSE_EVENT`) unusable

- Severity: medium · Verification: reported
- Where: `io/control.cpp:125-143, 370`
- Problem: Windows kills the process as soon as the handler returns, and the
  handler returns right after `SetEvent`.
- Fix: block in the handler until the app acknowledges (with a cap).

### IO-12 `Udp::create()` with no arguments is ambiguous

- Severity: low · Verification: reported
- Where: `io/udp.h:83-85` (both overloads fully defaulted; no callers yet)

### IO-13 Signal handler vs unpublish ordering

- Severity: low · Verification: reported
- Where: `io/control.cpp:101-107` vs `126-141, 227-244`;
  `io/terminal.cpp:508-517` vs `956-978`
- Problem: release-store then acquire-load needs `seq_cst` (StoreLoad). A
  handler can use a deleted `Self`, or write to a closed (maybe reused) fd.

### IO-14 ConPTY input worker can block in `ReadFile` (Windows, unconfirmed)

- Severity: low · Verification: reported
- Where: `io/terminal.cpp:370-396`
- Problem: with only character-less key events queued, `ReadFile` blocks and
  `stop_worker()`'s `join` deadlocks until a key is typed.

### IO-15 Job-object race on Windows (unconfirmed)

- Severity: low · Verification: reported
- Where: `io/process.cpp:283-290`
- Problem: the job is assigned after the child starts, so grandchildren it
  spawns in that window escape `kill_tree`. Fix: `CREATE_SUSPENDED`, assign, resume.

## Design

- **IO-D1 Owner/awaiter lifetime contract.** Awaiters hold raw `Self*`. Either
  document "do not destroy while awaited" or make destroy abort waiters (IO-4).
- **IO-D2 Inconsistent "busy" errors.** `k_invalid_argument` (`Stream::write`),
  `k_connection_already_in_progress` elsewhere, nothing for reads (IO-7).
- **IO-D3 Stop/cancel semantics differ per type.** `Stream::stop` aborts the
  read; `Udp::stop_recv` does not; `Acceptor::stop` errors but keeps
  listening; cancelling a watcher stops its handle; cancelling
  `Process::wait` SIGKILLs only the child, not `kill_tree`.
- **IO-D4 Unbounded queues.** Acceptor pending connections, UDP datagrams
  (receive never pauses), idle tick counts, the terminal paste buffer.
- **IO-D5 No DNS.** `resolve_addr` (`detail/libuv_helper.h:639-658`) only
  parses IP literals, but `Tcp::connect` / `listen` say "host".
- **IO-D6 libuv leaks into the public API.** fs flags need `UV_FS_O_*`;
  SIGKILL on Windows comes from `uv.h`; sockets are `i32`
  (`io/stream.h:170`, `io/udp.h:87`) though a Windows `SOCKET` is pointer-sized.
- **IO-D7 Detail headers are installed.** `lighter/xmake.lua:22` installs every
  `**.h`, including `detail/libuv_helper.h` and `io/awaiter.h`, which include
  `<uv.h>` (and the Windows SDK). No public header includes them, but it goes
  against AGENTS.md and libuv is not a public package. Exclude `detail/**`.
- **IO-D8 Performance.** `Stream::write` copies the payload every call; UDP
  receive copies each datagram into a `std::string` and formats the address
  per packet.
- **IO-D9 Thread pool.** `queue()` shares libuv's 4-thread pool with fs; an
  exception thrown by `fn` on a pool thread calls `std::terminate`.
- **IO-D10 Terminal.** Writes block the loop thread; the session uses a
  keep-alive Relay (control uses a background one); no Alt+key decoding on
  POSIX; SIGTSTP/SIGCONT left to the app.

## Nits

- `io/udp.cpp:245-251`: dead `await_ready` pending path; if it ever ran it would
  return the previous send's result without sending.
- `vocab/ringbuffer.cpp:59-71`: head/tail not reset when empty, so reads fragment.
- `io/fs.h:103`: `DirHandle` has no destructor (dropping it leaks); concurrent
  `readdir` calls clobber the shared `dir->dirents`.
- `detail/libuv_helper.h:737`: `contract_assert(false); return;` silently leaks in release.
- `Tcp::local_port(Acceptor&)` as a static function on `Tcp` is an odd place.
