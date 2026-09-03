# Contributing to zepto

Thanks for your interest in contributing! This document covers how to get started.

## Getting Started

1. Fork the repository
2. Clone your fork: `git clone https://github.com/<your-username>/zepto.git`
3. Create a branch: `git checkout -b feature/your-feature`

## Prerequisites

- GCC with C++20 support
- SSE 4.2 capable CPU (x86_64)
- GNU Make
- Python 3 (for Python bindings and benchmarks)

## Building

```sh
make          # build everything
make lib      # library only
make test     # build and run tests
```

## Making Changes

- Keep changes focused—one feature or fix per PR
- Follow the existing code style (see `zepto.h` / `zepto.cpp`)
- Add tests for new functionality in a new `*_test.cpp` file or extend existing ones
- Run `make test` before submitting

## Testing

```sh
make test
```

This runs: `quick_test`, `corrupt_test`, `rs_test`, `lz4_test`, `delta_test`.

## Pull Requests

- Write a clear description of what changed and why
- Reference any related issues
- Keep commits focused and well-described

## Reporting Issues

Open an issue on GitHub or email fundiman.dev@gmail.com.

## Code of Conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md). By participating, you agree to its terms.
