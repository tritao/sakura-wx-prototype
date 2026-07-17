# Sakura wx terminal TODO

This is the working backlog for turning the prototype into a reusable,
cross-platform terminal library and a Sakura frontend foundation.

## Terminal correctness and test infrastructure

- [ ] Build a deterministic VT replay harness for recorded terminal sessions.
  - Feed captured byte streams into `TerminalCore`.
  - Record snapshots, cursor state, selection output, titles, and metrics.
  - Support deterministic regression fixtures and replay failures.

- [ ] Improve selection UX.
  - Make selection boundaries grapheme-aware.
  - Add hyperlink-aware selection and activation behavior.
  - Add terminal search with match navigation and highlighting.
  - Harden clipboard copy/paste behavior, including multiline and Unicode text.

- [ ] Expand terminal semantics.
  - Add OSC 8 hyperlinks.
  - Cover emoji ZWJ clusters and other grapheme sequences.
  - Define and test a sixel/image handling policy.
  - Support more cursor modes and cursor rendering semantics.

## Rendering and performance

- [ ] Add dirty-cell tracking.
  - Avoid repainting unchanged cells.
  - Preserve correct invalidation for scrolling, selection, cursor, and resize.

- [ ] Add glyph caching.
  - Cache measured and rendered glyph runs by font, attributes, and text.
  - Include wide glyphs, combining marks, and fallback fonts in cache tests.

- [ ] Expand frame timing and rendering benchmarks.
  - Track input-to-frame latency and frame production rate.
  - Measure burst output, scrollback, resize, selection, and large-screen cases.
  - Compare cached and uncached rendering paths.

## Transport reliability and portability

- [ ] Harden transports.
  - ConPTY: handle blocked I/O cancellation, graceful shutdown, Job Object
    process-tree cleanup, startup failures, resizing, EOF, and rapid restart.
  - macOS PTY: test `forkpty()`, default shells, `SIGWINCH`, PTY `EIO`/EOF,
    Unicode environment, process groups, and native resize behavior.
  - Shell lifecycle: preserve or deliberately drain final output, prevent
    stale output after restart, make `Start()`/`Stop()` idempotent, and report
    exit codes, signals, and failure reasons exactly once.

- [ ] Define thread-safety and ownership rules.
  - Document which `TerminalCore` methods are UI-thread-only.
  - Specify transport worker-thread and output-queue ownership.
  - Define callback thread, lifetime, reentrancy, and shutdown guarantees.
  - Add misuse and concurrent lifecycle tests where practical.

## Backend abstraction and comparison

- [ ] Add a backend contract so libghostty-vt can be benchmarked against
  libtsm.
  - Define common input, output, snapshot, selection, title, resize, and
    metrics behavior.
  - Add an adapter boundary without leaking either backend's native types.
  - Replay the same fixtures through both implementations.
  - Compare correctness, latency, memory use, Unicode behavior, and feature
    coverage.

## Suggested order

1. VT replay harness and transport lifecycle fixtures.
2. Thread-safety and ownership contract.
3. Transport hardening on POSIX/macOS and native Windows.
4. Dirty rendering, glyph caching, and frame benchmarks.
5. Selection and terminal semantic expansion.
6. libtsm/libghostty-vt backend contract and comparison.
