# Review findings (temporary working docs)

Findings from the 2026-10-12 review of lighter 0.3.1 (commit `584438d`). These
docs are a scratchpad to revisit after the lexer split and the test cleanup;
delete each file once its issues are resolved or moved to an issue tracker.

| File                     | Area                                                                  |
| ------------------------ | --------------------------------------------------------------------- |
| [runtime.md](runtime.md) | coroutine runtime, cancellation, loop, Relay                          |
| [io.md](io.md)           | libuv I/O layer: stream, udp, fs, process, watcher, terminal, control |
| [http.md](http.md)       | curl client, streaming/SSE, HTTP/1.1 server, oneshot                  |
| [utils.md](utils.md)     | containers, encoding, codecs, mock, panic                             |
| [lexer.md](lexer.md)     | syntax-highlighting lexers (moves with the lexer to its new repo)     |
| [tests.md](tests.md)     | coverage gaps, input for the test audit                               |

## Conventions

- IDs (`RT-3`, `HTTP-1`, ...) are stable; refer to them from commits.
- Line numbers are as of `584438d` and will drift. Function names are given too.
- **Verification** says how much to trust a finding:
  - `verified`: the code path was traced by hand during the review.
  - `reproduced`: a reviewer built and ran a repro program.
  - `reported`: a reviewer's code-reading claim, not re-checked. Confirm before fixing.
- **Severity**: `high` (memory safety, hang, data loss in normal use),
  `medium` (wrong behaviour in plausible use), `low` (edge case, cosmetic),
  `design` (no bug, but a decision worth revisiting).

## Priority order

1. HTTP-1 (server stop UAF), HTTP-2 (slowloris / no write timeout)
2. RT-7 (Relay release order), RT-1 (cross-thread cancel)
3. IO-1 (timer cancel), IO-3 (fs cancel leak), IO-4 (owner destroyed while awaited)
4. UT-1..UT-5 (SmallVector), with tests
5. Runtime test suite (see tests.md)
6. RT-6 (dead MSVC/clang workarounds), RT-5 (Error carries no context)
