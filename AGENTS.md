This is "lighter", a C++26 infrastructure library (coroutine runtime on libuv, libcurl HTTP, codecs, utilities). It was extracted from liminal and is consumed by liminal and lilia as an xmake package (recipe in `Locietta/xmake-repo`, `packages/l/lighter`).

This project is built with C++26 and xmake. Check the coding style guide at `docs/dev/code-style-guide.md`; known GCC 16 issues are in `docs/dev/compiler-workarounds.md`.

Compiler toolchain, C dependencies and python tools are managed by pixi:

- `pixi run configure`: development configure
- `pixi run build`: build the project
- `pixi run format <paths...>`: format given files
- `pixi run test-all`: run all tests
- `pixi run test-remote-linux --host cachy` (and `-sanitize`): Linux and ASan/UBSan lanes on the remote host

C++ dependencies should be managed by xmake. Headers and module interfaces must remain platform-neutral; include Windows SDK and POSIX headers only from implementation units.

Versions follow semver (0.x for now: breaking changes bump the minor). Consumers pin a released version. To release: bump `version` in `pixi.toml`, tag `vX.Y.Z`, update the recipe's `add_versions` in `Locietta/xmake-repo`, and bump the consumers. Consumers can build against a local checkout with `LIGHTER_SOURCE_DIR`.

Commit headlines follow `<type>(<scope>): <subject>`.
