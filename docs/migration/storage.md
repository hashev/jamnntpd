# Migrated JAM storage

The C++23 storage library uses fixed-width little-endian records, scope-owned FILE
handles, owned subfield vectors and explicit checked flush before returning mutation
success. It reads an existing four-file base, appends messages and reply links,
cancels messages, and reads/updates sixteen-byte last-read records. Replies append
to the existing forward chain in chronological order, matching the original
setreply behavior. Cancelling a reply updates its live predecessor/parent in the
same transaction, and cycles are rejected before publishing a new message. No native struct
size is used as a disk offset. Independent Python-produced binary fixtures cover
high-bit values and deleted index slots; the maximum uint32 article number is tested.

All operations, file opens and closes share a registry mutex because POSIX record
locks are process-owned and an unrelated close can release a lock. This deliberately
serializes bases as well as sessions. File identities validate every companion file
through aliases. Native locks cover the first byte of .jhr, retaining the existing
external-tosser protocol. A separate Python locker process confirms conflict and
timeout behavior; barrier-controlled workers check 100 concurrent append iterations.

Writes capture affected existing records and file lengths before changing data.
Injected partial-write, flush, cancellation, lastread and allocation failures test
rollback. If rollback cannot complete, the registry quarantines that filesystem
identity for the process lifetime; reopening it does not bypass quarantine, even
after its last handle closes. Restore a consistent backup and restart the service.
No recovery journal or power-loss transaction guarantee is provided. File identity
quarantine is deliberately conservative: replacement files reusing an old inode can
require restart as well. Linux native validation passes; Windows status is tracked
in verification.md.

Reads validate file extents and cap raw read buffers at 64 MiB; corrupt lengths
cannot request arbitrary allocations. The production server uses this library;
authorization is checked in the service layer on every client operation.

## Production and interoperability status (2026-10-24)

The server now uses this storage layer for indices, reads, posting, replies and
cancellation. All legacy production C sources and Makefiles have been removed;
JAM binary layout, CRC and required storage operations are implemented in C++23.
Original notices are preserved in `src/jamlib/NOTICE` and the LGPL text in
`src/jamlib/LICENSE`. Linux native process-lock and ASan/UBSan suites pass.
See [rehearsal.md](rehearsal.md) for independent tosser and complete backup restore
verification. Native Windows validation and final quality acceptance are recorded
in verification.md, superseding the earlier component-stage status above.

On Windows, `_wfsopen(..., _SH_DENYNO)` opens native wide paths with read/write
sharing, while the first-byte record lock coordinates mutations. `_wfopen_s` is
unsuitable because its non-sharing behavior would exclude compatible tossers;
see [Microsoft CRT sharing documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fsopen-wfsopen?view=msvc-170).
The native process-lock and independent tosser tests verify that sharing and locking
remain separate requirements.
