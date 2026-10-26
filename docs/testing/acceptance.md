# Functional acceptance — comprehensive-functional-testing

Status: passed. All mandatory scenarios and final native extended gates passed.
Accepted source: `40628b2fa09256ffabe28215c755b011313a895e`.
Subsequent attestation changes contain documentation, evidence and matrix/task
status only; they do not change the accepted implementation.

## Reproducible evidence

[Full acceptance workflow](https://github.com/hashev/jamnntpd/actions/runs/40318565766)
runs on the exact source SHA above. All nine configurations passed, including
full native load/idle on three OSes. PR merge checkouts have a synthetic merge SHA
and are not substituted for source evidence.
JUnit, CTest logs, generated seeds, failure artifacts and coverage exports are
retained in each workflow's artifacts. Compact source evidence is committed in
[evidence/40628b2](evidence/40628b2/), including executed testcase results,
compiler, Python, CMake and actual installed vcpkg package metadata.

| Configuration | Compiler | Result |
| --- | --- | --- |
| Ubuntu 24.04 | GCC 14.2.0 | 15 CTest groups passed |
| Ubuntu 24.04 | Clang 19.1.1 | 13 CTest groups passed |
| macOS 26 arm64 | AppleClang 21.0.0.21000101 | 15 CTest groups passed |
| Windows Server 2025 | MSVC 19.51.36260.0 | 15 CTest groups passed |
| Windows Server 2025 | clang-cl 22.1.3 | 13 CTest groups passed |
| Linux ASan+UBSan | Clang 19, instrumented dependencies | Passed |
| Linux TSan | Clang 19, instrumented dependencies | Passed |
| Linux coverage | Clang 19 | Passed |
| Static analysis | LLVM 23.1.3 tools | Passed |

Each native source-evidence report records 151 distinct Catch testcases,
40 server e2e methods and two makechs CLI methods, with individual outcomes;
quarantine/timezone subprocess runs remain separate CTest groups. Platform
inapplicability is preserved as skipped at testcase level, never promoted to a
passing scenario. Linux `/dev/full` has a separate Linux-only matrix row; portable
throwing-sink unit coverage supplies the other platforms' logging-error evidence.

Pinned vcpkg baseline: `b42e8ed051d17a7f95b9d2a3140c1f9296423712`.
Actual dependencies: ICU 78.3, spdlog 1.17.0, Catch2 3.16.1. Full package versions,
triplets and ABI identifiers are in each source-evidence file.

## Functional traceability

The [machine-readable matrix](../../tests/fixtures/acceptance-matrix.json) has
80 mandatory scenarios covering 79 distinct features: all 18 NNTP commands,
every CLI/config option and alias, makechs, and all 15 current requirements.
All 80 mandatory rows are passed, with executed selector evidence for every
applicable platform; no unmapped, planned, failed or blocked rows remain.
`python3 tools/check_acceptance.py --final` verifies IDs,
inventory completeness, selectors, source SHA, platform and executed outcomes.

Normal generated profiles execute 1000 cases per parser/JAM/codec family and
100 real-TCP model sessions. Extended profiles require 10000 cases per family
and 1000 model sessions; seed `20261026`. Bounds, replay indices and minimizer
preserving failure identity are tested. Test bodies use disposable JAM copies
and private client profiles; no production database is opened.

## Coverage and quality

| Whole production coverage | Lines | Branches | Required |
| --- | --- | --- | --- |
| Linux | 5079/5445 = 93.28% | 2509/2864 = 87.60% | ≥90% / ≥85% |
| Native Windows | 5099/5486 = 92.95% | 2514/2884 = 87.17% | ≥90% / ≥85% |

The gate includes every own production translation unit and headers represented
by LLVM, across jam_unit, jamnntpd, makechs, jam_storage_probe and
jam_lifecycle_probe. No production file/error-path exclusions were added.
Windows additionally proves native console_control, locking, file identity and
NetworkRuntime execution. [Negative gate evidence](evidence/coverage-gate-negative.json)
executes the real gate with isolated mocked LLVM exports: subthreshold line and
branch coverage each fail with exit code 1.

Warnings-as-errors builds, whole-source clang-tidy, formatting, standalone header
compilation and the acceptance validator passed in static/native jobs.
Actionlint passed locally.
ASan+UBSan and TSan reported no findings. Local baseline self-tests and strict
OpenSpec validation also run before final attestation.

## Real clients

[Final client evidence](evidence/manual-clients-macos.json) is bound to the
accepted source, production tree and executable SHA-256. On macOS 26.6.2 arm64,
Thunderbird 158.0.1 from Mozilla and slrn 1.0.3 / S-Lang 2.3.3 from Homebrew
performed read/post/reply/cancel/reconnect against the same disposable server.
Thunderbird displayed `Н─│` and `.Dot`, then displayed its Unicode reply after a
full application restart. Both replies reference fixture article 1. Independent
HEAD/BODY checks confirm canceled articles 3 and 5 are unavailable, replies 4 and
6 remain readable, and a raw JAM walk confirms the canceled Unicode publication
stored CP866 bytes and the deletion attribute. Thunderbird used existing
`def_showto off` so its cancellation identity matches the plain author address.
Credentials were not saved or included in evidence.

## Defects found and corrected

1. POST incorrectly mapped storage I/O/corruption/lock timeout to input rejection
   441. Separate production commit `01d7bc2` returns temporary server failure 503;
   rejected content still returns 441. Unit and real lock-contention regressions
   verify byte-preserving failure and continued service.
2. Appending to a JAM base with a truncated published text extent could fill the
   missing bytes and silently revive corrupted content. Separate commit
   `2e068aa` validates all published header/text extents under the existing guard
   before mutation. Tests cover corrupt extents, exact unchanged files, deleted
   slots and legitimate empty messages. Evidence includes the original red
   reproduction and passing regression. Validation adds O(slots) reads per append;
   full mixed load measures its behavior with 10000-message bases.
3. Lock timeout while reading ARTICLE/HEAD/BODY/STAT masqueraded as nonexistent
   article 423/430; XOVER could return a successful incomplete overview. Separate
   commit `2e59e8a` maps timeout to 503 and discards incomplete overview output.
   The defect was found by a full mixed-load run, then reproduced with unit
   assertions and a real five-second external-lock TCP regression before fixing.
4. Full macOS load exposed POST response starvation: last-owner BaseState
   destruction waited without a bound behind repeated timed registry acquisitions.
   Native process samples identified the destructor wait. Separate commit
   `a594c06` queues registry open/close and operations in FIFO order, retaining
   recursive ownership and cancellation/deadline cleanup. Two unit cases cover
   recursion/expiry/stop/queue cleanup and progress of all 16 concurrent owners.
   The failed native full report is retained as
   [starvation evidence](evidence/extended-post-starvation-failure-macos.json).

Earlier failed/canceled workflow runs and failed local extended reports are
retained and described in [plan.md](plan.md). They are diagnostic history and do
not count as acceptance. Windows pressure tests were corrected to establish
actual native backpressure before asserting timeout/cancellation; the successful
candidate was run without suppressions or retries concealing failures.

## Extended acceptance

The [extended evidence gate](../../tools/check_extended.py) accepted all six
clean matching-source native load/idle reports. Each load ran three complete
600-second phases with 1/5/32 clients, two bases initially containing 10000
messages each, actual slow readers and an independent native JAM locker.

| Native OS | Warm → after cycles → after load fd/handles | RSS before → after load | Real idle (s) | Stop (s) | 32-client p99 (s) |
| --- | --- | --- | --- | --- | --- |
| [Linux](evidence/40628b2/extended-linux-load/report.json) | 5 → 5 → 5 | 6.34 → 14.64 MB | [300.001](evidence/40628b2/extended-linux-idle/report.json) | 0.114 | 2.608 |
| [macOS](evidence/40628b2/extended-macos-load/report.json) | 5 → 5 → 5 | 4.08 → 11.63 MB | [300.004](evidence/40628b2/extended-macos-idle/report.json) | 0.122 | 2.620 |
| [Windows](evidence/40628b2/extended-windows-load/report.json) | 68 → 68 → 68 | 7.79 → 8.34 MB | [299.986](evidence/40628b2/extended-windows-idle/report.json) | 0.105 | 4.225 |

Each row compares warmed, quiescent measurements of the same PID before and
after 100 reconnect cycles and after load. There is no native resource growth.
RSS is recorded as a trend, without requiring allocator RSS to return exactly.
The reports retain all resource samples, p50/p95/p99, throughput and operations
by response code. The observed latency is evidence for this workload, not a
performance guarantee.

Linux/macOS/Windows acknowledged 18538/18071/10450 publications respectively.
Independent JAM decoding verified publication counts, text bounds, markers and
deletions after stop; restart confirmed readability. Native locker iteration
counts were 1836/1391/1734. All three native job logs explicitly record
`JAM_TEST_CASES=10000`, `JAM_MODEL_SESSIONS=1000` and 15 passing CTest groups.
The committed [workflow attestation](evidence/40628b2/extended-workflow.json)
records all nine successful jobs. The [PR workflow](evidence/40628b2/pr-workflow.json)
also passed all nine checks. Short/dirty/mismatched-source or mixed-PID reports
are rejected and do not contribute to acceptance.

## Limits

This verifies the selected clients and synthetic external-tosser workflow, not
all third-party software. Native Windows CTRL_BREAK is exercised; interactive
console-close/logoff is not synthesized. Physical disk failure, power loss and
crash durability are not promised: recoverable failure rollback and explicit
quarantine/backup recovery retain the documented storage contract. Unit fault
injection does not establish fsync durability. No quality criterion was waived.
