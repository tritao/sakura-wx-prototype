# Windows portability TODO

This file tracks Windows-specific work for the reusable wx terminal library.
The current priority is to keep MSVC compilation healthy without pretending
that the native ConPTY runtime path is finished.

## Current status

- [x] Build the wx application and reusable libraries with native MSVC and
  Ninja in GitHub Actions.
- [x] Link the wx application as a Windows GUI executable.
- [x] Use the `tritao/libtsm` fork as the libtsm submodule.
- [x] Remove the current MSVC blockers from libtsm's core sources, including
  GNU-only statement expressions, `typeof`, range switch syntax, VLA use in
  screen operations, POSIX names, and unsupported compiler attributes.
- [x] Keep the Windows transport implementation available in the source tree
  and covered by local transport tests.
- [ ] Make ConPTY input/output reliable in the native Windows smoke and
  lifecycle tests. The shell starts and produces a prompt, but scripted input
  is not currently observed executing through the ConPTY channel.
- [ ] Re-enable Windows transport tests in CI after the runtime issue is fixed.

## ConPTY transport goals

- [ ] Capture a minimal native Windows reproduction with raw captured bytes,
  Win32 error codes, process state, and pipe state.
- [ ] Validate pipe direction, handle lifetime, pseudoconsole attachment, and
  `STARTUPINFOEX` attribute setup against the current Microsoft contract.
- [ ] Test interactive `cmd.exe`, PowerShell, and an explicit non-interactive
  command path.
- [ ] Define the input encoding and Enter/control-key mapping, including
  carriage return, line feed, Ctrl-C, and resize behavior.
- [ ] Make shell exit, restart, EOF, blocked-read cancellation, and process-tree
  cleanup deterministic.
- [ ] Add native tests for output bursts, Unicode, resize, exit status, and
  repeated start/stop cycles.
- [ ] Re-enable the Windows transport tests in the main CI matrix.

## MSVC libtsm hardening

- [x] Replace the current empty MSVC `SHL_EXPORT` fallback with a proper
  static/shared-library import/export contract.
- [x] Audit uses of `_shl_free_`, `_shl_close_`, and other GNU cleanup
  attributes; add explicit cleanup paths where MSVC cannot provide them in
  the audited terminal and PTY paths.
- [x] Replace temporary heap allocations introduced for VLA removal with
  bounded scratch storage or reusable workspace where appropriate.
- [ ] Add MSVC and clang-cl builds for both static and shared library modes.
- [ ] Run the same VT replay and semantic tests under GCC, Clang, MSVC, and
  clang-cl.
- [ ] Reduce and upstream the generic portability fixes to libtsm where
  practical, while keeping Sakura-specific behavior isolated.

## Exit criteria

Windows portability work is ready to leave this backlog when:

1. Native MSVC and clang-cl builds pass in static and shared configurations.
2. ConPTY smoke, lifecycle, resize, Unicode, and shutdown tests pass reliably.
3. Windows transport tests are enabled in CI without special diagnostics.
4. The libtsm fork has a documented synchronization/upstreaming strategy.
