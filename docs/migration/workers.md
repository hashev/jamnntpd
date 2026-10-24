# Worker ownership

`Admission::reserve` atomically counts a session before launching its thread.
A move-only lease returns the slot on scope exit, startup failure or worker
completion; the state outlives the admission owner if a lease still exists.
A zero limit denies all sessions, retaining legacy configuration behavior.

`WorkerPool` has one control-thread owner and owns every `jthread`. Completed
workers are joined and removed; shutdown requests stop on every worker before
joining any of them. The destructor joins all workers. Runtime and allocation
failures return errors without consuming capacity. Move-only session tasks are
owned through a shared callable wrapper compatible with the available C++23
standard library; the thread launch seam permits deterministic startup failures.
Worker exceptions produce a fixed diagnostic, never their potentially private
exception message. A failing reporter uses a bounded constant stderr fallback.

Tests cover 100 barrier-controlled admission rounds, zero/capped capacity,
move-only task ownership, thread creation and allocation failure, idle stop,
exception isolation and release after joins. The single control-thread ownership
of pool mutation is part of its contract; workers do not mutate the pool.
Service and signal integration remain separate tasks.

`SignalStop` is created on the control thread before workers and destroyed there
after their joins. POSIX blocks SIGINT/SIGTERM and the internal wake signal
SIGUSR1 in that thread; workers inherit the mask. A `sigwait` watcher requests
normal cooperative stop, without an asynchronous signal handler calling locks,
allocation or logging. Destruction wakes and joins the watcher before restoring
the original mask. Only one signal owner is permitted.

Windows uses a console callback with native interlocked flags and a watcher;
close/logoff/shutdown callbacks allow a bounded interval for orderly completion.
The Windows implementation has not yet been run on a native host. An independent
macOS process test sends real SIGINT and SIGTERM while a worker waits, checks
exit within five seconds, and verifies that no active reservation remains.
Full service termination under storage contention/load is still outstanding.

The production server now uses these workers and signals. Actual executable
stress tests repeat shutdown 100 times with concurrent POST/read/cancel and an
external JAM lock. Stop-aware registry acquisition prevents queued workers from
multiplying the native lock timeout. Native Windows validation remains required.
