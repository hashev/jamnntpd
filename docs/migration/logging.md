# Migrated diagnostics

The logger uses spdlog with thread-safe ostream/file and console sinks. An owned
ofstream opens the configured filesystem path in append mode and outlives its sink;
this also permits native wide filesystem paths on Windows. noecholog suppresses the
console sink and debug enables protocol command diagnostics. The service must wire
loggroups when integrating command dispatch.

AUTHINFO input is replaced by a redacted record before any sink receives it.
CR/LF/NUL in diagnostic fields are escaped to prevent forged records. Message bodies
are not intended for protocol command logging. Unit tests cover concurrent long
records, file flushing, redaction and injected sink failure. A sink failure uses a
constant bounded stderr fallback once per process, without echoing record contents
or recursively logging. Startup failure to open the configured file returns an error.

Logger lifetime must encompass all server workers, followed by an explicit final
flush. This lifecycle and real AUTHINFO command integration remain open server tasks.

## Production integration (2026-10-24)

The server now owns this logger across all workers, records selected groups when
loggroups is enabled, redacts AUTHINFO before dispatch logging, and flushes after
all workers join. Executable tests exercise successful and failed authentication
with unique password sentinels across file, stdout and stderr outputs; passwords
and POST bodies are not logged. On Linux the `/dev/full` e2e test additionally
verifies a single bounded fallback and continued service. Injected sink failure
unit tests run on every platform.
