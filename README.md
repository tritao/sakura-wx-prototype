# Sakura wx prototype

This is a deliberately small wxWidgets/libtsm terminal vertical slice for exploring a cross-platform Sakura frontend. It is separate from the GTK/VTE application in the parent repository.

The prototype currently provides:

- wxWidgets window and custom terminal-cell drawing;
- libtsm VT parsing, colors, cursor, scrollback, keyboard handling, and palette;
- drag, word, line, multiline, Unicode, and scrollback selection with automatic copy;
- scrollback auto-scroll, Shift-click extension, application mouse reporting, and Shift-forced local selection;
- clipboard copy/paste and core/transport telemetry;
- process lifecycle state, exit status, visible exit notices, and `Ctrl+Shift+R` restart;
- a POSIX `forkpty()` process bridge, including macOS, with shell resize and output polling;
- a Windows ConPTY process bridge selected automatically by the transport factory;
- pinned source submodules for wxWidgets and libtsm.

The transport interface keeps the wx/libtsm rendering layer independent of the
platform process plumbing. Native Windows validation is covered by CI; the
local Linux path exercises the POSIX backend directly.

## Get the source

```sh
git clone --recurse-submodules <repository-url> sakura-wx-prototype
cd sakura-wx-prototype
```

For an existing checkout:

```sh
git submodule update --init --recursive
```

The current pins are wxWidgets `v3.2.9` and libtsm `v4.6.0`; the exact commits are recorded by the superproject's gitlinks.

## Build

The build uses wxWidgets' CMake project directly and compiles libtsm's core sources into a small static target. No system installation of either library is required.

On Ubuntu/Debian, install the wxWidgets build prerequisites first:

```sh
sudo apt install build-essential cmake libgtk-3-dev
```

Then configure and build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/sakura-wx-prototype
```

The same workflow is available through the convenience scripts:

```sh
./build.sh
./run.sh
```

`BUILD_TYPE`, `BUILD_DIR`, and `JOBS` can be overridden as environment variables.
Set `RUN_TESTS=1` to build and run the test suite as part of the build.

## Testing

Configure and run the deterministic core, transport, PTY stress, and Linux wx tests:

```sh
RUN_TESTS=1 ./build.sh
```

For an instrumented run with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
BUILD_DIR=build-sanitize SAKURA_ENABLE_SANITIZERS=ON RUN_TESTS=1 ./build.sh
```

The core tests use semantic screen snapshots, while the PTY stress test exercises burst output, repeated resize, child completion, and shutdown. Transport lifecycle tests cover clean exit and failed shell startup. The wx smoke and UX tests run the real window under Xvfb when available.

Set `SAKURA_TRACE_METRICS=1` when running the app to print periodic output,
input, rendering latency, clipboard, transport queue, and resize counters:

```sh
SAKURA_TRACE_METRICS=1 ./run.sh
```

wxWidgets' platform dependencies vary by OS. On Windows, use the normal wxWidgets CMake/MSVC toolchain; on macOS, use Xcode command-line tools and CMake.

## Follow-up work

1. Expand semantic VT cases and scripted mouse/keyboard latency thresholds.
2. Exercise ConPTY natively in Windows CI and add macOS-specific PTY cases.
3. Benchmark libtsm against libghostty-vt behind the same terminal-core contract.

The vendored third-party code remains under its upstream licenses. See `third_party/libtsm/COPYING` and `third_party/wxWidgets/docs/licence.txt`.
