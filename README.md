# lighter

lighter is a small C++ infra library that I use for my personal projects.

## What it provides

Most of it is an async runtime: C++20 coroutines driven by a single libuv loop,
with structured concurrency (`Task`, `TaskGroup`, `WhenAny`, `with_timeout`,
cancellation tokens) and the usual I/O on top: TCP, UDP, pipes, child
processes, files, timers, signals and terminals. There is also a small HTTP client built
with libcurl which also shares the same async api shape.

Besides the async runtime, lighter also provides other utilities that can be useful for various purposes:

- serialization for JSON, JSONL with [glaze](https://github.com/stephenberry/glaze)
- UTF-8 and iconv utilities
- small containers and helpers (`small_vector`, `flat_map`, `check` / `panic`)
- Syntax highlighting lexers (ported from lexilla lexers, rewritten with C++ reflection)

## Using it from another project

This library is built by [xmake](https://xmake.io) and thus mainly meant to be used with it.

A xmake package for lighter is available in my xmake package repo ([Locietta/xmake-repo](https://github.com/Locietta/xmake-repo)).
Use the package just like any other xmake package after adding the custom repository:

```lua
-- add a custom xmake package repo
add_repositories("Locietta git@github.com:Locietta/xmake-repo.git")
-- (Or add the xmake repo as git submodule and then add it from the local path as what we do in this repo.)

add_requires("lighter 0.2.0")

target("app")
    add_packages("lighter")
```

Because lighter relies on C++26 static reflection and contracts, so for now, only GCC has good suport for this (with `-freflection -fcontracts`).
If you want to use it on windows, you will probably need to use mingw gcc.

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
The GCC 16 bugs we work around, are documented in
[docs/dev/compiler-workarounds.md](docs/dev/compiler-workarounds.md).

If you are changing lighter and a consumer at the same time, you don't need
to publish a release to try it out. Point the package at your local checkout
and rebuild it:

```powershell
$env:LIGHTER_SOURCE_DIR = "D:\.Project\lighter"
pixi run xmake require -f -y lighter
pixi run build
```

Unset the variable when you're done.

## License

MIT.
