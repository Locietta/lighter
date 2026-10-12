# Utils, encoding, codec, mock findings

Scope: `lighter/utils/*`, `lighter/types.hpp`, `lighter/encoding/*`,
`lighter/codec/*`, `lighter/mock/*`.

Strengths: the relocation trait (`utils/relocation.h`) is well done
(annotations override, conservative rule-of-zero heuristic, recursion through
the public template). FlatMap orders throwing steps before bytewise
relocation; no unwind bugs found. UTF-8 validation (`encoding/utf8.cpp`) is
correct: overlongs, surrogates, > U+10FFFF and truncated tails all handled.

## Bugs

### UT-1 SmallVector move constructor leaks an empty source's heap buffer

- Severity: medium · Verification: verified + reproduced
- Where: `utils/small_vector.h:722-725` (`move_from_other` early-returns when
  empty), `:1195-1198` (`move_from_typed_small_vector` then `reset_to_small`),
  `:139-143` (`reset_to_small` does not free)
- Repro: `SmallVector<int,2> a;` push 10, `a.clear(); SmallVector<int,2> b(std::move(a));`

### UT-2 constexpr SmallVector fails under enforced contracts

- Severity: low · Verification: reproduced (compiled)
- Where: `utils/small_vector.h:113-115, 175-176`
- Problem: `m_begin` is null during constant evaluation, so `end()` / the
  destructor hit `counted_range(nullptr, 0)` and its `contract_assert`. The
  `is_constant_evaluated` branches are effectively dead.

### UT-3 Insert leaks an element when a move throws

- Severity: low · Verification: reproduced
- Where: `utils/small_vector.h:597-599` (`insert_fill_inplace`),
  `:663-664` (`insert_range_inplace`)
- Problem: elements built at `old_end` are not counted in `size` before the
  second `uninitialized_copy`; if that throws they are never destroyed.

### UT-4 Self-append is a use-after-free in release builds

- Severity: high · Verification: verified
- Where: `utils/small_vector.h:986-997` (`append(Range)`), `:1007`;
  `utils/small_string.h:60, 63-74, 104`
- Problem: aliasing is only checked by `assert_safe_to_add_range`
  (contract). With contracts ignored, `v.append(v)` or `s += s` reallocates
  and then copies from the freed buffer. This breaks the style guide's rule
  that production correctness must not depend on contracts. `insert` copies
  aliased values to a temporary, so the two paths are inconsistent.
- Fix: detect aliasing at runtime and copy to a temporary first, like `insert`.

### UT-5 Moving out loses inline capacity

- Severity: low · Verification: verified
- Where: `utils/small_vector.h:700-705` (`steal_allocation_from` calls
  `reset_to_small(0)`), also `:1225, :1249`
- Problem: the moved-from vector's capacity becomes 0 though it has inline
  storage; it re-grows on the heap, and a `noexcept` move-assign may allocate.

### UT-6 Transcoder skips one byte after an error for non-UTF-8 sources

- Severity: medium · Verification: verified
- Where: `encoding/transcode.cpp:200-207`
- Problem: UTF-16LE `00 DC 41 00` decodes to U+FFFD, U+41DC instead of
  U+FFFD, "A" (misalignment). GBK→CP1252 on `D6 D0 61 62` skips only `D6`, then
  `D0 61` is read as a new character and "a" is lost.
- Fix: skip by the source encoding's code unit; for valid-but-unconvertible
  input, ask iconv for the character length (or convert to UTF-32 first).

### UT-7 Replacement written in the wrong shift state for stateful targets

- Severity: medium · Verification: reported
- Where: `encoding/transcode.cpp:200`
- Problem: for ISO-2022-JP the `?` is written while still in JIS mode.
- Fix: `iconv(cd, nullptr, nullptr, &dst, &dst_left)` before appending.

### UT-8 Mock: an unexpected call goes unreported if the caller catches

- Severity: high (for test reliability) · Verification: verified
- Where: `mock/mock.h:222-224`
- Problem: the throwing path never sets `unexpected_call`, so `verify()` passes
  when the code under test swallows the exception or turns it into an Outcome.
- Fix: set `unexpected_call = true` before throwing.

### UT-9 Mock: adding an action after earlier calls miscounts

- Severity: medium · Verification: verified
- Where: `mock/mock.h:256-260` (`add_action`)
- Problem: `expected_calls = actions.size()` counts only unconsumed actions.
  `then_returns(1); call; then_returns(2); call;` fails with "expected 1, observed 2".
- Fix: track the number of actions ever added.

## Design

- **UT-D1 Growth with a throwing move gives the basic guarantee only**:
  elements are moved, not copied as `std::vector` does via `move_if_noexcept`.
- **UT-D2 Inline buffer layout** (`first_element()`, `small_vector.h:199-205`)
  has no `static_assert` on the offset and no `std::launder`.
- **UT-D3 Aliasing detection** only covers ranges yielding `T&`;
  `v.insert(v.begin(), v | views::transform(f))` reads moved-from elements.
- **UT-D4 `insert_input_range` is O(n·m).**
- **UT-D5 `StringRef`** hides base overloads (`compare(pos, n, sv)`,
  `find_last_not_of("xy")` no longer compile); its `substr` clamps instead of
  throwing; temporaries already converted to `string_view` slip through.
- **UT-D6 `enum_name`** is a linear search; combined flags return the fallback
  (documented); digits break word splitting (`Utf8Decoder` → `utf8decoder`).
- **UT-D7 `panic.cpp:107` defines the process-wide `handle_contract_violation`**,
  so consumers cannot install their own; it also uses up the one-shot panic
  hook under the `observe` semantic.
- **UT-D8 `json::prettify` / `minify` drop glaze errors** (`codec/json/json.h:74-80`).
- **UT-D9 JSONL has no splitter for records cut across stream chunks.**
- **UT-D10 Mock state is unsynchronized** although handles are refcounted atomically.
- **UT-D11 Scope**: SmallString and StringRef have no callers in lighter. Check
  liminal/lilia before keeping them.

## Nits

- `utils/memory.h:136-157` uses `NDEBUG` + exceptions instead of contracts; the
  build passes `-DNDEBUG` everywhere, so the check is dead.
- `Uninitialized` zero-fills on every `StackTemporary`.
- `utils/type_traits.h` pulls in `<format>` and `<meta>` for a few traits.
- `utils/function_traits.h` lacks `&&` and C-variadic specializations.
- FlatMap doc comment still says "Liminal's".
- `utf8::decode_one` (`encoding/utf8.h:41`) is public and `noexcept` but its
  non-empty precondition is only a contract: empty input reads out of bounds in release.
- A moved-from `Transcoder` or `Mock` dereferences null.
- `types.hpp` is the only `.hpp`; `utils/fs.*` sits confusingly next to `async/io/fs.*`.
