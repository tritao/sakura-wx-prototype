# Sakura wx prototype

This is a deliberately small wxWidgets/libtsm terminal vertical slice for exploring a cross-platform Sakura frontend. It is separate from the GTK/VTE application in the parent repository.

The prototype currently provides:

- wxWidgets window and custom terminal-cell drawing;
- libtsm VT parsing, colors, cursor, scrollback, keyboard handling, and palette;
- a POSIX `forkpty()` process bridge with shell resize and output polling;
- pinned source submodules for wxWidgets and libtsm.

The process bridge is POSIX-only in this first slice. Windows ConPTY and a native macOS process backend are explicit follow-up work, so the wx/libtsm rendering layer can be evaluated independently of that platform plumbing.

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

Configure and run the deterministic core, PTY stress, and Linux wx smoke tests:

```sh
RUN_TESTS=1 ./build.sh
```

For an instrumented run with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
BUILD_DIR=build-sanitize SAKURA_ENABLE_SANITIZERS=ON RUN_TESTS=1 ./build.sh
```

The core tests use semantic screen snapshots, while the PTY test exercises burst output, repeated resize, child completion, and shutdown. The wx smoke test runs the real window under Xvfb when available.

wxWidgets' platform dependencies vary by OS. On Windows, use the normal wxWidgets CMake/MSVC toolchain; on macOS, use Xcode command-line tools and CMake.

## Next plan

1. Finish the POSIX slice with clipboard selection, copy/paste, mouse reporting, and child-exit handling.
2. Extract the renderer and terminal session behind interfaces so the wx frontend does not depend on `forkpty()`.
3. Add a Windows ConPTY backend and verify the same libtsm screen/input contract.
4. Add a macOS PTY backend, automated VT/parser tests, and a small benchmark against the existing GTK/VTE path.
5. Decide whether libtsm remains the prototype engine or is replaced behind the interface by libghostty-vt for the production branch.

The vendored third-party code remains under its upstream licenses. See `third_party/libtsm/COPYING` and `third_party/wxWidgets/docs/licence.txt`.
