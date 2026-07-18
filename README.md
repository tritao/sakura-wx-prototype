# Sakura wx prototype

This is a deliberately small wxWidgets/libtsm terminal vertical slice for exploring a cross-platform Sakura frontend. It is separate from the GTK/VTE application in the parent repository.

The prototype currently provides:

- wxWidgets window and custom terminal-cell drawing;
- libtsm VT parsing, colors, cursor styles, alternate screen, scrollback, keyboard handling, and palette;
- semantic VT coverage for OSC titles, bracketed paste, truecolor, text attributes, resizing, and wide glyph cells;
- drag, word, line, multiline, Unicode, and scrollback selection with automatic copy;
- scrollback auto-scroll, Shift-click extension, application mouse reporting, and Shift-forced local selection;
- clipboard copy/paste and core/transport telemetry;
- process lifecycle state, exit status, visible exit notices, and `Ctrl+Shift+R` restart;
- a POSIX `forkpty()` process bridge, including macOS, with shell resize and output polling;
- a Windows ConPTY process bridge selected automatically by the transport factory;
- pinned source submodules for wxWidgets and libtsm.

The transport interface keeps the wx/libtsm rendering layer independent of the
platform process plumbing. The CI matrix builds all three platforms and runs
the reliable POSIX/macOS transport coverage; Windows ConPTY smoke and
lifecycle tests remain available locally while their runtime behavior is being
fixed. See [`TODO-WINDOWS.md`](TODO-WINDOWS.md).

The reusable library targets are:

- `Sakura::TerminalCoreC`: libtsm-backed terminal state, parsing, input, and
  selection through the stable C ABI in `<sakura/terminal/core_c.h>`;
- `Sakura::TerminalTransport`: the platform process bridge;
- `Sakura::WxTerminal`: the wxWidgets terminal control, rendering, input, and clipboard integration.

The C ABI is the canonical integration boundary for all frontends. It
uses opaque terminal and frame handles, explicit release functions, immutable
frame views, cursor/mode metadata, scroll-delta hints, and row-local dirty
spans. The C++
core implementation is private to the library. Native renderers can request
cached UTF-8 row runs with packed style data via
`sakura_terminal_frame_row_run_count()` and
`sakura_terminal_frame_row_run()`; run text is borrowed from the frame.

The wx control accepts an optional `TerminalTransport`, so applications can use
the renderer with the bundled process bridge, a different backend, or no child
process at all while driving the returned `SakuraTerminal` handle directly.
The bundled bridge is available through `CreateTerminalTransport()` in
`<sakura/terminal/factory.h>`.

Presentation and lifecycle behavior can be customized without subclassing the
control:

```cpp
TerminalConfig config;
config.font_size = 14;
config.start_transport = true;

TerminalCallbacks callbacks;
callbacks.on_title_changed = [](const std::string& title) {
    // Update the host window title.
};
callbacks.on_error = [](const std::string& message) {
    // Surface the error in the host application.
};

auto* terminal = new WxTerminalCtrl(parent, CreateTerminalTransport(),
                                    config, callbacks);
```

Callbacks also report transport state changes and periodic core/transport
metrics.

The public terminal headers do not expose libtsm implementation types. Input
and mouse constants are defined by Sakura's terminal API, while libtsm remains
an implementation detail of the compiled core library.

Threading, ownership, callback lifetime, and shutdown guarantees are documented
in [`docs/architecture.md`](docs/architecture.md).

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

To install the reusable targets and headers:

```sh
cmake --install build --prefix "$HOME/.local"
```

An installed consumer uses the package config and must provide a wxWidgets
development installation:

```cmake
find_package(SakuraTerminal CONFIG REQUIRED)
target_link_libraries(my_terminal_app PRIVATE Sakura::WxTerminal)
```

The source-tree build keeps wxWidgets as the pinned submodule; the installed
package treats wxWidgets as a normal external dependency.

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

With `SAKURA_BUILD_BENCHMARKS=ON`, the terminal-core benchmark reports average
feed, packed-frame, and row-run access time for full repaint, partial repaint,
Unicode/wide/combining output, scrollback, and clean row-run traversal:

```sh
cmake -S . -B build -DSAKURA_BUILD_BENCHMARKS=ON
cmake --build build --target sakura-terminal-core-benchmark -j
./build/sakura-terminal-core-benchmark
```

The wx paint benchmark also reports full/partial paints, painted cells, p50/p95/
p99 and maximum frame paint time, glyph-cache hits/misses, evictions,
entry/byte occupancy, background rectangles, bitmap versus direct text draws,
and DC state changes.
Its matrix covers full and partial ASCII, cached versus uncached
Unicode/wide/combining output, glyph-cache churn, burst output, large screens,
resize, scrolling, cursor, and selection workloads:

```sh
xvfb-run -a ./build/sakura-wx-paint-benchmark
# Emit JSON for tooling; the process exits nonzero if renderer invariants fail.
xvfb-run -a ./build/sakura-wx-paint-benchmark --json
```

When benchmarks are enabled, `wx_paint_regression` runs the JSON benchmark
under CTest and checks cache byte/entry limits, LRU churn, and cached versus
uncached draw-path behavior. It intentionally does not gate on wall-clock
timings, which are display-server and build-mode dependent.

For release-mode profiling across 2, 4, 8, and 16 MiB cache limits, use the
profiling helper. It stores one JSON result per cache size in
`benchmark-results/`:

```sh
./scripts/run_wx_paint_profile.sh
```

The deterministic VT replay harness runs hex-encoded session fixtures through
the C terminal ABI and checks semantic cells, cursor state, titles, selection,
paste accounting, and metrics:

```sh
./build/sakura-vt-replay tests/fixtures/basic.vtlog
```

Replay fixtures use line-oriented commands such as `resize`, `output`,
`paste`, `select`, and `expect`. Keeping terminal payloads as hexadecimal
bytes makes control sequences and Unicode input stable under version control.

The renderer preserves libtsm's UTF-8 cell text and wide-cell widths, and maps
bold, italic, underline, dim, inverse, truecolor, and DECSCUSR cursor styles
into wxWidgets drawing state. The libtsm submodule tracks the
`tritao/libtsm` fork's `sakura/combining-marks` branch, which preserves
zero-width combining marks in screen cells and selection copy.

Set `SAKURA_TRACE_METRICS=1` when running the app to print periodic output,
input, rendering latency, clipboard, transport queue, and resize counters:

```sh
SAKURA_TRACE_METRICS=1 ./run.sh
```

wxWidgets' platform dependencies vary by OS. On Windows, use the normal wxWidgets CMake/MSVC toolchain; on macOS, use Xcode command-line tools and CMake.

## Follow-up work

1. Define an explicit ABI/versioning policy for shared-library packaging.
2. Expand combining-mark coverage into full grapheme-cluster and emoji ZWJ selection tests.
3. Use native CI failures to guide further ConPTY and macOS PTY coverage.
4. Benchmark libtsm against libghostty-vt behind the same terminal-core contract.

The vendored third-party code remains under its upstream licenses. See `third_party/libtsm/COPYING` and `third_party/wxWidgets/docs/licence.txt`.
