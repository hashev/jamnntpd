# Implementation verification snapshot

Date: 2026-10-24, macOS arm64. Change: `modernize-cpp23-raii`,
20 of 41 tasks complete. This is an intermediate report, not migration acceptance.

## Implemented scope

The C++23 build contains `jam_core`, the migrated `makechs`, unit tests and a
storage process-lock and lifecycle probes. Core components cover value/error types, configuration
parsing, JAM serialization and transactional storage, ICU/custom CHS conversion,
MIME, formatting, socket primitives, worker/signal ownership, NNTP session dispatch
and spdlog logging. Components awaiting service integration
do not close their corresponding end-to-end tasks.

The original NNTP server remains C and is not built by the new CMake project.
All 40 original source/build-file hashes match `tests/fixtures/source-baseline.json`.
Its findings remain open until replacement and regression verification.

## Checks executed

Local versions and build commands are in [build.md](build.md).

| Check | Result and scope |
| --- | --- |
| Debug build, warnings as errors | Passed for current CMake targets |
| Debug CTest | 6/6 groups passed |
| Catch2 | 83 cases, 77,602 assertions passed |
| Baseline Python | 16 fixture tests passed; not server e2e |
| Integration checks | `makechs` CLI, independent process JAM lock and real SIGINT/SIGTERM passed |
| ASan + UBSan | Build and all 6 CTest groups passed |
| TSan, separate build | Build and all 6 CTest groups passed |
| clang-tidy 23.1.3 | 16 production translation units passed; five active native-boundary NOLINT uses on this host; third-party diagnostics excluded |
| clang-format | Dry run of current C++ production/test files passed |
| OpenSpec strict validation | Passed |

Sanitizer results apply to these macOS component runs. They do not replace the
planned Linux sanitizer suite or native Linux/Windows/AppleClang service runs.

## Coverage and outstanding gates

Fresh LLVM instrumentation profiles from the coverage CTest run were merged with
`llvm-profdata`. `llvm-cov export` combines `jam_unit`, `makechs` and
`jam_storage_probe` and `jam_lifecycle_probe`; totals below select production files under `src/`, excluding
test and external dependency files. Local raw output is in ignored
`build/coverage/coverage.json` and `summary.json`.

| Metric | Covered / instrumented | Percentage | Required |
| --- | --- | --- | --- |
| Lines | 3,608 / 3,987 | 90.49% | 90% |
| Branches | 1,755 / 2,099 | 83.61% | 85% |

The branch gate remains unmet; whole-project acceptance is not established. The denominator covers the migrated subset, not the
whole project; the C server and inactive Windows branches are not covered by
this run. Whole-project coverage gates, pinned dependency CI, native platform
checks, full NNTP e2e/concurrency/shutdown, remaining Guidelines applicability
review and migration/rollback rehearsal are outstanding.

## Confirmed compatibility decision

On 2026-10-24 the user confirmed preservation of byte-pass-through rules without
a table, with new explicit `-builtin` opt-in. The specification, design and task 5.2
reflect this decision. See [text.md](text.md) for implemented library selection
semantics; service integration is still outstanding.

The confirmed persistent-I/O policy is reflected in [storage.md](storage.md):
rollback after recoverable failures; quarantine and explicit diagnosis if
persistent failure prevents rollback, followed by restoration from backup.
No recovery journal or physical-disk availability guarantee is claimed.

## Current source-module coverage

All 16 compiled production C++ translation units are present in the merged export.
Native Windows-only branches are not instrumented by this macOS build.

| Module | Lines | Branches |
| --- | --- | --- |
| `src/core/charset_rules.cpp` | 95.68% | 85.06% |
| `src/core/codec.cpp` | 92.80% | 87.13% |
| `src/core/config.cpp` | 94.35% | 89.42% |
| `src/core/formatting.cpp` | 93.61% | 87.05% |
| `src/core/jam_format.cpp` | 98.84% | 96.30% |
| `src/core/logging.cpp` | 91.53% | 80.00% |
| `src/core/map_generator.cpp` | 91.49% | 79.38% |
| `src/core/message_values.cpp` | 91.20% | 89.86% |
| `src/core/mime.cpp` | 96.54% | 92.57% |
| `src/core/network.cpp` | 76.08% | 54.90% |
| `src/core/session.cpp` | 97.48% | 90.00% |
| `src/core/signals.cpp` | 91.67% | 80.00% |
| `src/core/storage.cpp` | 78.78% | 69.64% |
| `src/core/text.cpp` | 100.00% | 96.43% |
| `src/core/workers.cpp` | 100.00% | 88.89% |
| `src/makechs.cpp` | 63.41% | 61.11% |
