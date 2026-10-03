# lighter

A C++26 infrastructure library, extracted from the
[liminal](https://github.com/Locietta/liminal) agent CLI:

- **async**: structured-concurrency coroutine runtime on libuv. It provides
  `Task`, `TaskGroup`, `WhenAny` / `WhenAll`, timeouts, cancellation, and the
  sync primitives `Mutex`, `Event`, `Semaphore` and `ConditionVariable`. I/O
  covers TCP, UDP, pipes, processes, the file system, timers, signals and
  terminals.
- **http**: an asynchronous libcurl client with SSE / streaming responses.
- **codec**: JSON and JSONL helpers over glaze.
- **encoding**: UTF-8 utilities and iconv transcoding.
- **lexer**: syntax-highlighting lexers.
- **utils**: `small_vector`, `flat_map`, `string_ref`, `panic` / `check`, and
  more.

It targets GCC 16.2, the only toolchain shipping both P2996 static reflection
(`-freflection`) and P2900 contracts (`-fcontracts`), on Linux and on Windows
through MinGW. The toolchain and the C dependencies (libuv, libcurl, libiconv)
come from [pixi](https://pixi.sh); the C++ dependencies (glaze, ngcpp-proxy)
come from xmake.

## Use it as a package

The recipe lives in [Locietta/xmake-repo](https://github.com/Locietta/xmake-repo),
which consumers add as their `xmake` submodule and package repository.

```lua
add_requires("lighter 2026.10.03")

target("app")
    add_packages("lighter")
```

The consumer must build inside a Pixi environment that provides the same GCC
and C dependencies (see this repository's `pixi.toml`), and must compile with
`-freflection -fcontracts`.

### Developing lighter alongside a consumer

Point the package at a local checkout and force a rebuild of the package:

```powershell
$env:LIGHTER_SOURCE_DIR = "D:\.Project\lighter"
pixi run xmake require -f -y lighter
pixi run build
```

Unset the variable to go back to the pinned version.

## Build and test this repository

```powershell
pixi run configure     # releasedbg
pixi run build
pixi run test-all
pixi run test-remote-linux --host <linux-host>            # optional remote Linux lane
pixi run test-remote-linux-sanitize --host <linux-host>   # ASan/UBSan
```

## Known issues

- Awaiting a `Task<T>` whose error channel is `void` copies `T`, because
  `Outcome<T, void, void>::value()` returns by value for lvalues. A move-only
  `T`, such as `lighter::Tcp`, therefore does not compile there. Workarounds:
  give the task an error type, or return a `std::shared_ptr`.

## License

MIT
