# Migration acceptance

Change: `modernize-cpp23-raii`. Date: 2026-10-24.
Accepted full native CI: [40069175330](https://github.com/hashev/jamnntpd/actions/runs/40069175330),
Validated implementation revision: `16a3105b736cf5d4f517cdc134e2787f60a274f5`.
Documentation-only completion commits do not change the validated production/test sources.

The production `jamnntpd` executable integrates the network loop, scoped workers,
NNTP session engine, authentication/privacy, MIME/text conversion, spdlog and the
C++ JAM backend. `makechs` is also C++23. All forty historical production C/header/
Makefile hashes were checked before removal; original notices and licenses remain.
There are 21 own production C++ translation units and no production C sources.

| Check | Accepted result |
| --- | --- |
| Native Linux GCC 14 | All ten CTest groups pass with warnings as errors |
| Native Linux Clang 19 | All ten CTest groups pass with warnings as errors |
| Native macOS AppleClang 21 | All ten CTest groups pass with warnings as errors |
| Native Windows MSVC | All ten CTest groups pass with warnings as errors |
| Native Windows clang-cl | All ten CTest groups pass, with native coverage export |
| Linux ASan + UBSan | Full unit/executable/100-round stress suite passes |
| Linux TSan, separate profile | Full suite passes with instrumented dependencies, no race reports |
| Linux GCC Release | Local optimized build and all ten CTest groups pass |
| clang-tidy 23.1.3 | All 21 production translation units and own header paths pass; no diagnostic families disabled |
| Header/format checks | Core headers compile independently; clang-format dry run passes |
| Core Guidelines | All 495 pinned rule entries have reviewed applicability or explicit alternatives/exceptions |
| OpenSpec | Strict validation passes; task acceptance is tracked in tasks.md |

The current suite contains 102 Catch2 cases, 14 real-server e2e methods, independent
fixture checks, native lifecycle/locker/utility probes and 100 contested server
read/post/cancel/shutdown rounds. Actual NNTP tests exercise every characterized
command, guest/auth/read/post/netmail permissions, replies/cancellation, all ten
standard encodings, special characters/pseudographics, custom CHS, byte-pass-through,
strict refusal without mutation, replacement, oversized/incomplete POST, debug
redaction, terminal-control escaping, admission/config failures and Unicode paths.

[Whole-production coverage](coverage.md) includes every own translation unit, own
headers and uncovered error branches, merging all five executable objects:

| Platform | Lines | Branches | Required |
| --- | --- | --- | --- |
| Linux x64 | 4,930 / 5,323 (92.62%) | 2,394 / 2,796 (85.62%) | 90% / 85% |
| Native Windows x64 | 4,950 / 5,364 (92.28%) | 2,399 / 2,816 (85.19%) | 90% / 85% |

Both reports pass. Native Windows additionally verifies execution of Winsock,
file-identity/record-lock and console-control functions. Some OS failure and
close/logoff/shutdown branches remain uncovered and stay in the denominator;
aggregate gates are not per-module or exhaustive OS-fault guarantees.

Recoverable read/write/flush/allocation failures preserve prior JAM state.
Permanent rollback failure quarantines the filesystem identity until backup restore
and restart. Quarantine tests use separate processes because the safety contract
conservatively includes recycled inodes. A real backend/session fault test refuses
240 after injected write/flush failure, including parent-thread updates.

The independent [tosser/backup rehearsal](rehearsal.md) verifies old-format import,
server read/write, native first-byte locking and complete four-file restoration.
All test bases are disposable copies; production data was not used. Restoring a
backup discards later posts/last-read updates. No journal, physical-disk availability
or power-loss transaction guarantee is claimed.

Explicit table-less read/post rules retain bytes; `-builtin` opts into standard
conversion. Original custom mappings and directionality remain authoritative.
See build.md, text.md, storage.md and the R01–R37 register in review.md for details.
