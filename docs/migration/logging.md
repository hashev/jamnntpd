# Migrated diagnostics

The logger uses spdlog with thread-safe ostream/file and console sinks. An owned
ofstream opens the configured filesystem path in append mode and outlives its sink;
this also permits native wide filesystem paths on Windows. noecholog suppresses the
console sink and debug enables protocol command diagnostics. The service records
group selections when loggroups is enabled.

AUTHINFO input is replaced by a redacted record before any sink receives it.
CR/LF/NUL and other C0/DEL bytes in diagnostic fields are escaped to prevent
forged records and terminal-control injection. Message bodies
are not intended for protocol command logging. Unit tests cover concurrent long
records, file flushing, redaction and injected sink failure. A sink failure uses a
constant bounded stderr fallback once per process, without echoing record contents
or recursively logging. Startup failure to open the configured file returns an error.

Logger lifetime encompasses all server workers, followed by an explicit final flush.

## Production integration (2026-10-24)

The server now owns this logger across all workers, records selected groups when
loggroups is enabled, redacts AUTHINFO before dispatch logging, and flushes after
all workers join. Executable tests exercise successful and failed authentication
with unique password sentinels across file, stdout and stderr outputs; passwords
and POST bodies are not logged. On Linux the `/dev/full` e2e test additionally
verifies a single bounded fallback and continued service. Injected sink failure
unit tests run on every platform.
