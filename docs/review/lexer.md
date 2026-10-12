# Lexer findings

Scope: `lighter/lexer/**`. This file moves with the lexer to its new repo.

Architecture as reviewed: each language is a value type (`CppLexer{.dialect = ...}`)
satisfying `LexerImplementation`, wrapped in a `pro::proxy<LexerFacade>`.
`registry.cpp` is an explicit constexpr table of fence names → factory.
Reflection is used in `role.h` to build a compile-time Style → TokenRole table
from `[[=token_role(...)]]` annotations, with consteval validation (unique IDs,
exactly one role, style 0 is DEFAULT). `Document` keeps one `u32` state per
line and supports streamed `append()`, re-lexing from the last unfinished line.
Ported from Lexilla: `THIRD_PARTY_NOTICES.md` must move with the code.

## Bugs

### LX-1 Python raw strings end at `\"`

- Severity: medium · Verification: verified
- Where: `lexer/language/python.cpp:133-144` (`string_end` skips escape
  handling entirely when `raw`)
- Problem: in Python a backslash still prevents the quote from closing a raw
  string: `r"a\"b"` is one string. The lexer closes it at `\"`.
- Fix: when `raw`, skip the character after a backslash without decoding it.

### LX-2 Python octal escapes accept hex digits

- Severity: low · Verification: verified
- Where: `lexer/language/python.cpp:126-129`
- Problem: for octal `kind`, the loop condition is `ascii_hex_digit(c) || (...)`,
  so `\18` or `\1a` consume hex digits.

### LX-3 C++ raw string: newline on the opening line painted DEFAULT

- Severity: low · Verification: reported
- Where: `lexer/language/cpp.cpp:441-447`
- Problem: `state == k_raw_string` is compared with the payload bits still set.

### LX-4 Markup issues

- Severity: low · Verification: reported (not re-read)
- Where: `lexer/language/markup.cpp:96, 131, 182, 243, 255`
  - an attribute value spanning lines loses its quote state and the tag never closes;
  - entities are painted inside comments and CDATA;
  - multi-line `<!-- -->` in Markdown is ignored.

None of these hang or read out of bounds; they only produce wrong colours.

## Design

- **LX-D1 The driver loop is duplicated** across the 17 language files.
- **LX-D2 `Scanner` (`scanner.h`) may be unused.** Reported; check with a grep.
- **LX-D3 Append-only documents**: no mid-document edit, and no early stop once
  line states converge after an edit. Fine for streaming output; a limit for
  an editor.

## Missing tests

- empty input, a lone `\r`, bytes >= 0x80, unterminated constructs at EOF;
- chunked `append()` vs whole-document `assign()` gives identical styles and
  line states (the core promise of streaming), ideally fuzzed over split points;
- per-language regression cases for LX-1..LX-4.
