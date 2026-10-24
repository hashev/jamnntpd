# Disposable JAM interoperability and restore rehearsal

The executable e2e scenario `test_independent_tosser_and_consistent_backup_restore`
uses a private temporary four-file copy of the historical JAM fixture. It stops
the server and captures all `.jhr`, `.jdt`, `.jdx`, `.jlr` bytes together.
A separate Python tosser process acquires the native first-byte `.jhr` lock,
serializes a new message with independent `struct.pack` code, writes CP866
pseudographics and CHRS metadata, updates counters, and flushes/fsyncs its writes.
The restarted C++ server reads that imported message over NNTP, posts another
message, and an independent decoder verifies the resulting storage bytes.

The test then stops the server, restores all four backup files together, restarts,
checks original GROUP bounds and absence of newer articles, and compares every
restored file with the backup. The scenario passes in local Debug, AppleClang and
Linux ASan/UBSan executable runs. Native Windows acceptance is tracked separately.
The independent process-lock test and 100-round contested shutdown suite additionally
verify native lock conflicts and rejection of blocked mutations during shutdown.

Operational procedure: stop **all** writers, including external tossers; copy the
complete four-file set and matching configuration; migrate/restart and verify reads
before permitting posting. To restore, stop every writer again, restore the complete
set and restart all users of that base. Restoring a backup discards posts and
last-read updates made after that snapshot. Never mix files from different snapshots.
Do not copy a live base without coordinating every writer.

Recoverable injected I/O failures restore prior files under lock. Persistent failure
that prevents rollback quarantines the base and requires restoration plus process
restart. The software does not provide a journal, crash/power-loss atomicity, physical
disk availability, or compatibility certification for every third-party tosser.
The fixture proves the shared binary format and locking protocol independently of
the production serializer.
