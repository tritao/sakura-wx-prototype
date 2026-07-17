# Runtime ownership and threading contract

This document defines the concurrency boundary for the reusable terminal
library. It is intentionally conservative: the UI owns orchestration, while
transport reader threads only move process output into a queue.

## `TerminalCore`

`TerminalCore` is single-thread-affine.

- Construct it and call all methods from one thread.
- Do not call `FeedOutput`, input, selection, scrolling, snapshot, or metrics
  methods concurrently.
- The write callback is synchronous and runs on the calling thread.
- The write callback must not call back into the same `TerminalCore`.
- The callback may hand bytes to a thread-safe transport, but it must not
  retain the callback data pointer after it returns.
- Move construction/assignment transfers the core to the destination object's
  current thread; the moved-from object must not be used.

The core intentionally has no internal mutex. This keeps terminal parsing,
selection, and snapshot generation deterministic and avoids presenting a false
guarantee that libtsm itself is reentrant.

## `TerminalTransport`

The owner thread is responsible for `Start`, `Stop`, `Write`, `Resize`, and
`TakeOutput`. These operations must not be called concurrently or from the
reader thread.

Implementations may have an internal reader thread:

```text
child process -> reader thread -> synchronized output queue -> UI TakeOutput()
```

`GetStatus`, `GetMetrics`, and `IsRunning` are snapshot reads and may be used
while the reader thread is active. A returned output vector owns its strings;
the caller may retain them after `TakeOutput()` returns.

`WxTerminalCtrl` owns the `std::unique_ptr<TerminalTransport>` passed to its
constructor. It stops the transport before releasing it. Applications must not
use that transport pointer after handing ownership to the control.

## `WxTerminalCtrl` and callbacks

The wx control and all of its callbacks are UI-thread-affine:

- timer-driven output draining occurs on the wx event thread;
- title, status, error, and metrics callbacks execute synchronously on that
  same thread;
- callbacks must be short and must not destroy or re-enter the control;
- hosts that need to update other threads or perform destructive actions should
  post work back to their own event loop;
- callback state is owned by the control and is not invoked after destruction.

Destruction stops the timer first, requests transport shutdown, joins the
transport reader thread through `Stop()`, and then destroys the core and
transport state.

## Validation

The ownership boundary is exercised by the transport lifecycle and PTY stress
tests. The deterministic VT replay test validates that core parsing and
snapshot generation remain synchronous and repeatable. Any future asynchronous
core or callback API must add an explicit synchronization contract rather than
silently relaxing these rules.
