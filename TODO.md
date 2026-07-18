# Sakura wx terminal TODO

This is the working backlog for turning the prototype into a reusable,
cross-platform terminal library and a Sakura frontend foundation.

## Terminal correctness and test infrastructure

- [x] Build a deterministic VT replay harness for recorded terminal sessions.
  - Feed captured byte streams through `SakuraTerminal`'s C ABI.
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

- [x] Add dirty-cell tracking.
  - Expose libtsm ageing as generation-based dirty-frame metadata.
  - Reuse unchanged terminal cells through a copy-on-write snapshot cache.
  - Use a wx backing bitmap so unchanged cells are not redrawn.
  - Preserve correct invalidation for scrolling, selection, cursor, and resize.
  - Reuse packed row objects and expose scroll-delta hints for full-screen
    output scrolling.

- [x] Add glyph caching.
  - [x] Cache UTF-8 conversion and font variants in the wx renderer.
  - Cache measured and rendered glyph runs by font, attributes, colors, and text.
  - Cover repeated ASCII, wide, and combining runs in wx smoke and benchmark tests.

- [x] Add a terminal-core frame timing benchmark.
  - Report feed, packed-frame, and row-run access timings.
  - Cover full repaint, partial repaint, Unicode/wide/combining output,
    scrollback, and clean row-run traversal.
- [x] Expand the benchmark matrix with burst output, resize, selection,
  large-screen, scrolling, and cached-versus-uncached renderer comparisons.
  - Report background rectangle batching, bitmap/text draw paths, DC state
    changes, and maximum per-frame paint time.
- [x] Bound the wx glyph bitmap cache by entry count and estimated bitmap
  memory, with LRU eviction and cache-churn benchmark coverage.
- [x] Add machine-readable wx benchmark output and invariant-based regression
  checks without using noisy wall-clock thresholds.
- [x] Add rolling p50/p95/p99 paint latency metrics and a RelWithDebInfo wx
  profiling matrix for 2/4/8/16 MiB cache limits.

## Transport reliability and portability

- [ ] Harden transports.
  - POSIX/macOS PTY handling covers bounded shutdown escalation,
    process-group cleanup, PTY `EIO`/EOF, non-blocking writes, bounded resize
    dimensions, and restart-safe reader cleanup.
  - Shell lifecycle preserves final output until drained, discards undrained
    output at restart, makes `Start()`/`Stop()` idempotent, and has restart
    regression coverage.
  - Windows ConPTY implementation exists but its interactive runtime coverage
    is temporarily disabled in CI; see [`TODO-WINDOWS.md`](TODO-WINDOWS.md).

- [x] Define thread-safety and ownership rules.
  - Document which `SakuraTerminal` C ABI calls are UI-thread-only.
  - Specify transport worker-thread and output-queue ownership.
  - Define callback thread, lifetime, reentrancy, and shutdown guarantees.
  - Add debug thread-affinity assertions and validate the contract with
    lifecycle, PTY stress, and replay tests.

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
