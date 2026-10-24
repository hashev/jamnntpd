# Whole-production coverage evidence

Accepted CI run: [40068403981](https://github.com/hashev/jamnntpd/actions/runs/40068403981),
source tree `598a17c4943746b9bd09b409263da3a2c6ebf996` (identical to `aadeb75`).
Source coverage merges the unit binary, both production executables and both native
probes after all ten CTest groups, including 100 contested server shutdown rounds.
Every one of the 21 production C++ translation units must be present. Own headers
and error branches are included; dependencies and tests are outside the denominator.
A dash means that the compiled file has no instrumented branches.

| Platform | Lines covered / total | Lines | Branches covered / total | Branches |
| --- | --- | --- | --- | --- |
| Linux x64, Clang 19 | 4,925 / 5,323 | 92.52% | 2,392 / 2,796 | 85.55% |
| Windows x64, native clang-cl | 4,946 / 5,364 | 92.21% | 2,397 / 2,816 | 85.12% |

Both platform reports pass the 90% line and 85% branch aggregate gates. Native
Windows evidence also requires executed network/storage/signal adapters and their
platform functions; it cannot be substituted with a POSIX or cross-compiled run.
The gate script does not exclude uncovered error branches to raise these numbers.

| Production file | Linux lines | Linux branches | Windows lines | Windows branches |
| --- | --- | --- | --- | --- |
| `src/core/backend.cpp` | 87.50% | 75.95% | 87.50% | 75.95% |
| `src/core/charset_rules.cpp` | 96.04% | 87.50% | 95.68% | 87.50% |
| `src/core/codec.cpp` | 92.80% | 87.13% | 92.80% | 87.13% |
| `src/core/config.cpp` | 95.04% | 88.99% | 95.04% | 88.99% |
| `src/core/error.hpp` | 100.00% | — | 100.00% | — |
| `src/core/formatting.cpp` | 93.61% | 89.29% | 93.61% | 89.29% |
| `src/core/ftn.cpp` | 95.72% | 89.50% | 95.72% | 89.50% |
| `src/core/jam_format.cpp` | 98.84% | 96.30% | 98.84% | 96.30% |
| `src/core/logging.cpp` | 95.24% | 95.83% | 95.24% | 91.67% |
| `src/core/map_generator.cpp` | 91.61% | 79.17% | 91.61% | 79.38% |
| `src/core/message_values.cpp` | 91.20% | 89.86% | 91.67% | 89.86% |
| `src/core/mime.cpp` | 96.34% | 91.36% | 96.34% | 91.36% |
| `src/core/network.cpp` | 83.46% | 66.67% | 83.40% | 69.07% |
| `src/core/network.hpp` | 100.00% | — | 100.00% | — |
| `src/core/post_builder.cpp` | 95.68% | 80.53% | 95.68% | 80.53% |
| `src/core/server.cpp` | 86.07% | 78.05% | 86.07% | 78.05% |
| `src/core/session.cpp` | 98.02% | 90.95% | 98.02% | 90.95% |
| `src/core/session.hpp` | 100.00% | — | 100.00% | — |
| `src/core/signals.cpp` | 91.67% | 90.00% | 78.95% | 52.78% |
| `src/core/storage.cpp` | 85.49% | 78.33% | 85.10% | 78.06% |
| `src/core/storage.hpp` | 100.00% | — | 100.00% | — |
| `src/core/text.cpp` | 100.00% | 96.55% | 100.00% | 96.67% |
| `src/core/workers.cpp` | 100.00% | 88.89% | 100.00% | 88.89% |
| `src/core/workers.hpp` | 84.62% | — | 84.62% | — |
| `src/jamnntpd.cpp` | 92.11% | 100.00% | 92.11% | 100.00% |
| `src/makechs.cpp` | 92.68% | 94.44% | 92.68% | 94.44% |

Native Windows-only execution counts in the merged report: `console_control` 128,
`native_lock_code` 4,798, `identify_open_file` 2,822, and Winsock runtime creation 134.
These are coverage counters, not performance measurements. Windows control close/
logoff/shutdown paths and some OS failure branches remain uncovered; their lines
remain in the denominator. No claim of per-module 90%/85% coverage or exhaustive OS
fault simulation is made. Tests exercise real CTRL_BREAK, native lock conflicts,
shared-file interoperability, EOF/timeouts, invalid handles and process cleanup.

Reproduce with `tools/check_coverage.py`; native Windows additionally passes
`--native-windows-evidence`. CI uploads full LLVM JSON and totals beside CTest logs.
The pinned vcpkg manifest and native compiler configuration are recorded in build.md.
