# lighter

lighter is the small runtime that [liminal](https://github.com/Locietta/liminal)
grew on top of, pulled out into its own repository so other projects (so far,
the QQ bot lilia) can use it too.

Most of it is an async runtime: C++20 coroutines driven by a single libuv loop,
with structured concurrency (`Task`, `TaskGroup`, `WhenAny`, `with_timeout`,
cancellation tokens) and the usual I/O on top: TCP, UDP, pipes, child
processes, files, timers, signals and terminals. Around that sit an HTTP client
built on libcurl's multi interface, which can stream server-sent events; glaze
helpers for JSON and JSONL; UTF-8 and iconv utilities; syntax-highlighting
lexers; and the containers and helpers liminal kept needing (`small_vector`,
`flat_map`, `check` / `panic`).

The catch is the compiler. lighter uses C++26 static reflection and contracts,
and today only GCC 16.2 has both, so it builds with GCC on Linux and MinGW GCC
on Windows, nothing else. [pixi](https://pixi.sh) supplies that toolchain along
with libuv, libcurl and libiconv. glaze and ngcpp-proxy come in as xmake
packages.

## Using it from another project

The xmake recipe lives in [Locietta/xmake-repo](https://github.com/Locietta/xmake-repo),
so point your project at that repository (liminal and lilia keep it as their
`xmake` submodule) and require a version:

```lua
add_requires("lighter 0.2.0")

target("app")
    add_packages("lighter")
```

Your project has to build inside a pixi environment with the same GCC and C
libraries (copy the dependencies from this repo's `pixi.toml`), and it has to
compile with `-freflection -fcontracts`.

If you are changing lighter and a consumer at the same time, you don't need
to publish a release to try it out. Point the package at your local checkout
and rebuild it:

```powershell
$env:LIGHTER_SOURCE_DIR = "D:\.Project\lighter"
pixi run xmake require -f -y lighter
pixi run build
```

Unset the variable when you're done and the next build goes back to the pinned
version.

## Working on lighter itself

```powershell
pixi run configure     # releasedbg, which is what we develop and test with
pixi run build
pixi run test-all
```

To run the same tests on a Linux machine you can reach over SSH, use
`pixi run test-remote-linux --host <host>`. Add `-sanitize` to the task name
(`test-remote-linux-sanitize`) for an ASan/UBSan build.

Code style is described in [docs/dev/code-style-guide.md](docs/dev/code-style-guide.md).
The GCC 16 bugs we work around, and where, are in
[docs/dev/compiler-workarounds.md](docs/dev/compiler-workarounds.md).

## License

MIT.
