# Contributing

Thanks for your interest in contributing to convex-cpp.

convex-cpp is a community C++ client for [Convex](https://convex.dev),
maintained by Potionify. It is not an official Convex product. For questions
about the Convex platform itself, the
[Convex Discord Community](https://convex.dev/community) is the right place.

## Questions and feature requests

Please open a GitHub issue on this repository.

## Building and testing

Requirements: CMake ≥ 3.21 and a C++20 compiler (MSVC 2022, clang, gcc).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The live integration tests (optional, need Docker) run against the
self-hosted open-source backend. See [integration/README.md](integration/README.md).
CI runs both suites on every pull request.

## Pull requests

Community PRs are welcome. A few things to be aware of:

- Small, focused PRs (bug fixes, documentation, test coverage) are the
  easiest to review and integrate.
- For anything larger, open an issue first to check the direction before you
  put in too much work.
- convex-cpp is a port of the official
  [convex-rs](https://github.com/get-convex/convex-rs) client (with wire
  shapes cross-checked against
  [convex-js](https://github.com/get-convex/convex-js)). Staying faithful
  to the official clients is a design goal, so behavioral changes to the
  protocol or sync state machine should reference the corresponding
  upstream behavior.
- New code should come with tests. The unit suite runs without any
  network, and protocol-level changes usually deserve an integration test
  too.
- There is no enforced formatter. Match the style of the surrounding code.
