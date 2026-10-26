# Comprehensive functional testing

Current status: 32/41 tasks complete. The expanded local macOS suite passes all
13 CTest groups; formatting, standalone headers and clang-tidy pass locally.
Native CI, sanitizer/coverage gates, full extended runs and real client acceptance
remain pending. Historical failures below are retained as defect evidence, not
current acceptance results. The registry now contains 79 features, including makechs.

## Baseline — 2026-10-26

Source revision: `359cfc745a14f7ea33dfc9da4b4fddf73e2fc541`.
Branch for this change: `codex/comprehensive-functional-testing`.

Actual local environment: macOS 26.6.2 arm64; CMake 4.4.4; Homebrew Clang
23.1.3; Python 3.15.0; ICU 78.3; spdlog 1.17.0; Catch2 3.16.1.
The existing Debug build uses Homebrew dependencies (CMake cache paths), not
vcpkg-installed binaries. Repository vcpkg manifest baseline remains
`b42e8ed051d17a7f95b9d2a3140c1f9296423712`. Native CI acceptance will separately
record its actual pinned dependency installation and compiler versions.

Command executed from repository root, without changing tests or assertions:

```sh
ctest --preset debug --output-on-failure
```

All ten groups passed in 19.33 seconds: server-stress (100 rounds), server-e2e,
lifecycle-native, storage-process-lock, makechs-cli, unit, storage-quarantine,
storage-quarantine-reopen, timezone-native and baseline.
Full output: [baseline evidence](evidence/baseline-macos-arm64.txt).
This is an existing-build baseline, not a new clean build or cross-platform
acceptance of the forthcoming tests. No new CI/extended/client result is claimed.

## Contract discrepancy: POST storage failures

Status: resolved by the user on 2026-10-26: restore historical storage-error status in a separately committed production fix (`01d7bc2`).

Evidence:

- Historical source `a05db5daf83d8277dd08bc1c64c20c5781d3a968:src/nntpserv.c`,
  lines 2833–2858, returns `503` on messagebase open, lock and write failures;
  successful write returns `240`.
- `tests/fixtures/protocol/contracts.json`, POST entry, retains `503 storage failure`.
- Current `src/core/session.cpp`, Session::post, line 319, maps every submit
  error to `441 Posting failed`, without examining ErrorKind.
- `tests/unit/backend.cpp`, test `Real backend never acknowledges a post after
  injected write or flush failure`, currently asserts `441` on injected faults.
- Main nntp-service spec preserves existing response contracts except documented
  defect corrections. jam-storage requires failure reporting/no false success,
  but does not independently choose a POST status code. Existing tests agree
  with current code, so a green baseline does not resolve historical compatibility.

Reproduction of source comparison (read-only):

```sh
git show a05db5d:src/nntpserv.c
rg -n 'auto Session::post|441 Posting failed' src/core/session.cpp
```

Proposed resolution: preserve historical `503` for local storage failures and
`441` for invalid/rejected articles, with focused regression tests. This needs
a separately scoped production fix, as the approved testing proposal excludes
production edits. Alternative: explicitly approve documenting the changed `441`
contract, then adjust the fixture and tests consistently. Do not silently change
assertions, bless the observed implementation, or mark acceptance passed.


## POST correction verification

Commit `01d7bc2` restores 503 for io/corrupt/timeout; input/encoding/permission
and stopped submission retain 441. Internal error messages are not exposed.
The existing real-backend fault assertions now enforce 503, while retaining
rollback/parent-link assertions. A new seven-category unit regression and real
TCP missing-companion/rejected-article/recovery regression pass.

Commands: `cmake --build --preset debug --parallel 4`,
`ctest --preset debug --output-on-failure`,
`clang-tidy -p build/debug src/core/session.cpp`, and clang-format dry-run on
changed C++ files. All passed; full CTest took 21.37 s. Logs are in evidence/.
This local result does not claim fresh Linux/Windows or extended acceptance.

## Traceability registry

`tests/fixtures/acceptance-matrix.json` maps 78 inventory features: all 18 NNTP
commands, 15 main-spec requirements, config/user/default options and seven short
aliases. Each row records applicability, input/expectation/oracle, test selector,
mandatory status and evidence. Initial selectors identify related existing tests,
not proof that all scenario axes are covered: rows remain `planned` until the
expanded tests and per-platform evidence exist.

```sh
python3 tools/check_acceptance.py
python3 -m unittest discover -s tests/baseline -p test_acceptance_matrix.py -v
python3 tools/check_acceptance.py --final
```

Structural validation and all nine validator self-tests pass. `--final` must
currently fail because mandatory scenarios remain planned. The validator checks
inventory coverage, duplicate IDs, unknown selectors, mandatory skips, evidence
paths/results/platforms and matching revision. Structural success is never an
acceptance result. Unit-test counterexamples verify each rejection path.

## Independent corpus and raw TCP harness

`tests/fixtures/jam/corpus-recipes.json` defines empty/single/all-deleted/gapped,
mixed CP866/Windows-1251/KOI8-R with binary unknown subfields, and lastread bases.
`tests/baseline/jam_corpus.py` writes fixed-width little-endian records independently
of production code, uses fixed timestamps and refuses to overwrite existing bases.
Python codec names are translated to the advertised canonical CHRS labels.
`test_jam_corpus.py` independently decodes records, checks live counts, exact
Unicode content, unknown binary subfields and lastread layout, reproducibility,
10,000-record generation and refusal to overwrite.

```sh
python3 tests/baseline/jam_corpus.py --base /path/to/new/temp/base --recipe mixed --count 10000
python3 -m unittest discover -s tests/baseline -p test_jam_corpus.py -v
```

The command requires an existing private temporary parent directory; never point it
at a live base. Existing files are refused rather than backed up or replaced.

`tests/integration/raw_client.py` preserves received bytes and explicitly checks
CRLF, dot-stuffing, terminators, response codes and line/aggregate/deadline bounds.
Its seven self-tests feed intentionally invalid responses, confirming failures
instead of normalization. Requests are recorded only as byte counts: fragmented
AUTHINFO data must never be retained in transcripts.

The real executable reads all six corpus variants, including independently
encoded messages, using an unforced group default so per-message CHRS applies.
An initial test-setup failure (forced CP866 and noncanonical CP1251 metadata)
was corrected in the harness/generator; it was not classified as a production bug.

Server startup failure, timeout or a test exception now kills/reaps its subprocess,
closes streams and cleans the temporary base. Failure artifacts are saved to
`JAM_TEST_ARTIFACTS` (default ignored `build/acceptance-failures`) with seed, phase,
redacted transcripts, disposable JAM snapshots and safe server diagnostics;
credential/config files are excluded. Controlled timeout/exception tests verify
reaping, removed root, closed port, seed/snapshot/transcript and absent credential
sentinel in every artifact. Set `JAM_TEST_SEED` to reproduce a generated run.

## Raw command/state and network cases

The real-server raw TCP suite covers all 18 commands, LIST variants, mixed case,
unknown/empty commands, article current/number/message-id selectors, invisible and
deleted articles, numeric overflow/invalid suffixes, NEXT/LAST ends and navigation
after a new POST, XOVER bounded/open/reversed/overflow ranges, empty group state,
exact HELP bytes and QUIT disconnect. Matrix command rows are `implemented`, not
`passed`: native per-platform final evidence is still pending.

Session tests check separate clients, failed authentication preserving identity,
options-only login, rejected options, reauthentication dropping read/post rights
and invalidating selected/current state, and negative posts preserving private
base bytes. Guest read-only/post-only/denied/full roles additionally test first
matching wildcard rule precedence and independent read/post permissions. Further
Alice/Bob/third-party strictnetmail combinations remain task 3.3 work.

Raw boundaries cover command 997/998/999 payload bytes (999/1000/1001 with CRLF),
fragmented and coalesced requests, LF compatibility, NUL and oversized-tail
injection. POST 19999/20000/20001 unstuffed bytes checks success/rejection and
unchanged files on refusal. Dot-stuffing is checked against both stored bytes and
raw BODY output. EOF at five incomplete-POST phases never publishes data.

Commands used for focused checks:

```sh
python3 tests/integration/test_server.py --executable "$PWD/build/debug/jamnntpd" --fixtures "$PWD/tests/fixtures" -v
python3 tools/check_acceptance.py
```

## Privacy, POST and CLI additions

The new netmail matrix covers Alice sender, Bob recipient and unrelated Eve under
strictnetmail on/off; ARTICLE/HEAD/BODY/STAT numeric and message-id access,
GROUP counters, LIST ACTIVE bounds, XOVER visibility and unauthorized cancellation
are checked. Legacy non-strict counters intentionally retain physical slot bounds;
strict counters are visibility-filtered. All denied operations preserve the base.

The new publication test rejects missing mandatory headers, crossposting,
HTML/multipart, unsupported controls, invalid references and malformed base64/QP;
every refusal retains all four original files. Valid 7bit/8bit/base64/QP posts
are independently decoded, counters checked, read back, then verified after a
restart with byte-identical persisted files. Historical 503 storage failures
remain covered separately from 441 article refusals.

CLI additions verify all seven aliases, path values with spaces, port boundaries
0/1/65534/65535 and max 0/1/4294967294/4294967295 without starting huge worker pools;
invalid overflow/sign/junk inputs fail. Executable tests use an isolated cwd and
Unicode output paths, check help spellings `?`, `-h`, `--help`, missing config and
immediate `-create` semantics. Duplicate config settings retain the last value;
whole-line `#` comments and quoted paths work, inline `;` comments and `-help` are
not part of the existing syntax and are explicitly rejected. Earlier exploratory
assertions assuming those forms were corrected after inspecting the parser; no
production grammar was broadened to accommodate tests.

The squote user option has a presence-only `smartquote` server default;
`def_squote` is explicitly rejected by the established parser. The registry was
corrected to exclude that nonexistent server option; user squote on/off remains
covered, and omission/smartquote cover default off/on.

## Blocker JAM-01: truncated text file can be republished as valid old content

The standalone production correction remains `01d7bc2` and has no uncommitted
production edits. The new corruption scenario is a separate pre-existing backend
defect, not a POST status-mapping regression.

Reproducer: copy the sample base to a private Server directory; authenticate;
truncate only the temporary test.jdt to zero; STAT by message-id reports 503;
POST `Replacement payload` without References; STAT/BODY the original article.
Observed: POST returns 240, original STAT now returns 223, and the old message
body becomes `Replacement p` instead of `Hello\r.World\r`. A new append has
filled the text range still referenced by the old index/header. No production
base was accessed. See [independent observation](evidence/truncated-jdt-reproduction.json)
and [failing regression](evidence/truncated-jdt-regression.txt).

Cause visible in src/core/storage.cpp: read_info validates base/index sizing;
append uses current .jdt size as its new offset without first validating the
extents of existing published records under that lock. A previously unreadable
record can consequently become readable with another article's bytes.

`test_truncated_text_base_rejects_post_without_reviving_old_article` deliberately
requires 503, unchanged four-file state and continued rejection of the corrupt old
article. It is failing, not skipped or weakened; the JAM validation requirement
is marked failed in the matrix. Task 6.1 and final acceptance remain incomplete.

Proposed separately authorized production fix: validate existing published header
and text extents while holding the native JAM mutation lock before appending;
refuse the operation before any file mutation if they exceed current companion
files. Include header/text truncation and valid empty/deleted bases in regression
checks and preserve stop/deadline behavior and external tosser interoperability.
A broad rewrite, silent repair, placeholder text or automatic backup restore is
not part of this proposal. The testing change explicitly excludes production
backend changes, so this additional fix needs the user's scope decision.

## Additional focused verification

The new cancellation suite checks local/sent/locked/no-cancel policy, repeated
cancellation, first/middle/last reply unlinking and independent header/index
counters. Existing atomic snapshot/TOCTOU storage tests remain mandatory.

A 16-row binary orthogonal array tests all pairs of 13 server flags and asserts
wire/storage/log effects. Separate e2e cases exercise six user options on/off
through both server defaults and login overrides, readorigin metadata versus body
origin, guest suffix and local/nochrs behavior. Output paths and echomail notification
are exercised with private fixture paths. The local-attribute expected mask was
confirmed from historical jam.h (`MSG_TYPELOCAL = 0x00800000`).

Extended 256-row CHS tests independently specify expansion/deletion, four-byte
Unicode and high-byte preservation; malformed expansion/trailer/truncation/range
inputs fail with diagnostics. New boundary tests check final CP866/UTF-8 byte
limits at L-1/L/L+1 for 35/71/75/100/20000 bytes; a real-server 70/71/72-character
subject test verifies acceptance/storage and folds using Python's independent
email decoder. makechs output now passes through the independent CHS interpreter,
including supplementary code points, swapped ASCII, identity and malformed-map
refusal. All these focused checks passed locally; they do not replace final native,
sanitizer, coverage, extended or external-client evidence.

Latest local verification: `ctest --preset debug --output-on-failure` ran all ten
groups in 32.18 s. Nine groups passed; server-e2e ran 35 methods with exactly the
new JAM-01 regression failing and the pre-existing Linux-only /dev/full scenario
skipped on macOS. Full output: evidence/expanded-ctest-blocked.txt. Baseline
validator/oracle tests and formatting checks pass. OpenSpec strict validation
passes structurally; implementation is paused at 18/41 tasks for the additional
storage fix decision. No final acceptance, native CI or completion is claimed.


## JAM-01 resolution — resumed apply

The resumed apply implemented the separately reviewed backend fix in commit
`2e068aa`. Before extending files, append validates each published fixed header,
article number and live subfield/text extent under the existing JAM lock; only
fixed-sized index/header records are read and stop is checked between slots.
Deleted sentinels are skipped; valid empty and deleted-only bases still accept
new articles. Validation is O(slots) per append, to be measured by extended load.

Three new storage regressions cover full/partial header/text truncation without
any mutation, empty/deleted-only acceptance, and cancellation during the scan.
The previously failing raw TCP test now passes. Local complete CTest passed all
ten groups in 33.92 s, including all expanded e2e and 100 contention rounds.
clang-tidy on storage.cpp and formatting checks also passed; logs are in
`evidence/jam01-fix-*.txt`. Earlier failure artifacts are retained as historical
reproduction. The requirement row is now implemented, not final passed, because
new cross-platform/sanitizer/extended acceptance remains outstanding.

## Companion faults, locks, restore and bounded transport

Every companion file is now truncated independently and its applicable reader
must fail without changing any of the four files. Existing boundary/cycle/unknown
subfield/lastread and read/write/flush/lock fault cases remain enabled. A dedicated
quarantine-resize subprocess injects each of the three rollback-resize ordinals;
all must quarantine and reject reopen/further append. Directories stay alive so
conservative quarantined inode identities cannot be recycled during that test.

An external native locker and complete four-file hardlink alias now verify the
100 ms probe timeout and successful access after release, preserving all files.
Existing 100 server contention/shutdown rounds also cover stop during native
lock waits. The independent tosser/restore rehearsal adds a second user's explicit
16-byte lastread record while every writer is stopped, checks that server read/
post preserves it, then restores the original complete four-file snapshot.
Hardlinks exercise native local filesystems; no network-filesystem lock claim is
made. POSIX uses fcntl and Windows uses CRT byte locks on its native CI runner.

Independent mapping vectors now verify decode, encode and embedding each known
legacy byte in surrounding ASCII. A real TCP alias/automatic/forced-default
scenario checks override precedence and refuses an unknown unaliased label
without mutation (message-id failure uses 430, numeric failure uses 423).

Native backpressure tests use a small receive buffer and a 32 MiB payload: a
non-reading peer causes timeout, stop interrupts a write after proven progress,
and a deliberately slow reader receives a 64 KiB payload exactly. All local
focused checks passed. Existing short-write/zero-progress and accept/line/read
bounds remain in the suite.

Admission tests cover max=0/1/2/8 and max-1/max/max+1, including eventual lease
release after QUIT. Parallel writers to one/two bases check counters, unique
message IDs and exact independent content. The separate 100-round stress group
retains read/post/cancel/lock/stop checks and its <8 s shutdown bound.

New real-server lifecycle cases send SIGINT/SIGTERM (POSIX) or CTRL_BREAK
(Windows) during connection startup, queued reads, incomplete POST and external
JAM lock waits. macOS cases passed with unchanged storage and final log flush.
Windows execution remains a native-matrix requirement; actual console close/
logoff/shutdown UI events are not claimed by this test. No signal is sent before
the server's termination handler is installed.

Logging flags, group selection, concurrent/control-character records and credential
redaction remain covered by direct unit tests plus the new native pairwise config
and harness-artifact scenarios. /dev/full applies only to Linux; portable failing
stream/sink unit checks cover failure isolation on the other platforms. No
platform skip counts as executed evidence for another platform.

## Seeded and extended profiles

The `server-generated` group runs 100 real TCP sessions, 20 seeded operations per
session, checking an independent auth/group/article state model after every step.
Operations are stored as stable integer indices, never raw credential arguments.
Failures save a versioned replay JSON and the seed/session index. The hidden
Catch2 replay entry is intentionally selected only with an explicit input file;
it is not a skipped acceptance scenario.

The generated C++ test runs at least 1000 inputs per parser/JAM/codec family,
mixing mutated valid fixtures with bounded arbitrary data. Codec checks preserve
all CP866 bytes through Unicode. Valid parser/JAM results retain their documented
invariants. Failure files include family/seed/index; count is bounded at 10000.
The explicit extended checks passed locally at 10000 inputs per family and 1000
model sessions with seed 20261026. Replay and the reducer CLI were also tested.

```sh
JAM_TEST_CASES=10000 build/debug/jam_unit 'Generated parser JAM and codec families retain invariants with replayable seeds'
python3 tests/integration/test_generated_sessions.py --executable "$PWD/build/debug/jamnntpd" --fixtures "$PWD/tests/fixtures" --sessions 1000
JAM_REPLAY_FAMILY=codec JAM_REPLAY_FILE=/path/to/case.bin build/debug/jam_unit 'Replay a minimized generated case'
JAM_REPLAY_FAMILY=codec python3 tools/minimize_case.py --input /path/to/failure.bin --output /path/to/new/minimal.bin -- build/debug/jam_unit 'Replay a minimized generated case'
# State-model replay/reduction uses --replay and JAM_REPLAY_FILE instead of generating sessions.
python3 tests/integration/test_generated_sessions.py --executable "$PWD/build/debug/jamnntpd" --fixtures "$PWD/tests/fixtures" --replay
```

`tools/minimize_case.py` refuses a successful/non-reproducing input and requires
an exclusive new output file. It preserves the replay's failing exit code, with
a 10-second bound per candidate. Self-tests prove reduction, repeatability and
that non-reproducing input is rejected. A state-model replay uses exit 2 for an
invalid replay format and exit 3 for a functional mismatch, so invalid JSON
cannot be mistaken for the original failure.

```sh
cmake --preset extended -DCMAKE_TOOLCHAIN_FILE=/path/to/pinned/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset extended --parallel 4
ctest --preset extended --output-on-failure
```

For this local Homebrew LLVM setup, use the explicit compiler plus
`-DICU_ROOT=/opt/homebrew/opt/icu4c@78 -DICU_INCLUDE_DIR=/opt/homebrew/opt/icu4c@78/include`.
Without the explicit include root, a fresh local FindICU selected SDK ICU headers
alongside the Homebrew library and produced math/fmt include-order failures;
correct dependency selection fixed configuration rather than disabling warnings.

The normal suite now retains the original ten groups and adds resize quarantine,
model sessions and a resource/load smoke group. Extended configuration adds the
30-minute 10000-message/base load and real five-minute idle groups. Smoke reports
carry `acceptance_profile: false`; they never count as extended acceptance.
Load phases use 1/5/32 total clients (including slow readers), two active bases,
independent native lock contention, exact post/cancel accounting, percentile and
throughput/resource series, 100 quiescent connection cycles and restart validation.
Reports include revision, dirty-source flag and native platform. No latency SLO
or exact allocator-RSS return is invented.

The evidence gate requires clean, matching-revision full load and idle reports
for Linux/macOS/Windows, rejects short/smoke/stale/failed runs and checks resources
and shutdown bounds. A later pass cannot hide an earlier failure at that revision.
Separate JUnit collection rejects missing, failed or skipped required CTest groups
and records compiler and actual vcpkg package metadata. Evidence gates have their
own deliberate-failure self-tests; synthetic self-test reports are never stored
as acceptance artifacts.

## Extended harness preflight corrections

The first full local load at `803b119` failed after its 600-second first phase:
a slow-reader socket raised TimeoutError during the next phase. It is retained as
a failed diagnostic run, not acceptance. A 30-second/10000-message diagnostic also
showed transient worker/file handles while an unfinished XOVER was draining.
The corrected harness prepares GROUP/XOVER and observes its first byte before
starting writers, polls slow reads within the bounded phase, requires actual
received bytes, and allows up to eight seconds for EOF cleanup before checking
quiescent resources. It does not alter server deadlines or suppress failed runs.
A fresh three-phase 30-second/10000-message diagnostic passed with 5 native fd
before and after load; this diagnostic is not the required 30-minute acceptance.
Mixed reads now exercise ARTICLE/HEAD/BODY/STAT as well as slow XOVER.

Static CI also exposed two build-preparation errors: the official LLVM archive
requires zstd long-window decoding, and Ninja module maps must be generated by
a normal build before standalone clang-tidy reads compile_commands.json. Both
are fixed in workflow setup without diagnostic exclusions.

Real client checks use Thunderbird 158.0.1 (official Mozilla DMG, SHA256
9336e0e31d96f8803aaf96969031e5def5413c4f0b18dd93c9b19089230ca102) and
slrn 1.0.3/S-Lang 2.3.3 on macOS, private profiles and disposable JAM bases.
With the established default showto=on, Thunderbird intentionally hides its cancel
command because the displayed author differs from its identity. For cancel
compatibility use the existing def_showto off (or user showto=off), then fetch
fresh headers. This is a client identity check, not a new server contract.
Unicode entry uses paste: native simulated typing did not insert non-ASCII
characters; the composed text and returned BODY are verified independently.

Real client acceptance completed: both clients read/post/reply/cancel/reconnect.
Thunderbird was restarted, reauthenticated and displayed the persisted Unicode
reply (article 6); slrn was restarted, read its own article 4 and cancelled it.
Independent HEAD/BODY checks returned 430 for both cancelled articles (3 and 4),
verified reply References for 5/6, and decoded the deleted Unicode record directly
from the private JAM header/text to confirm CP866 bytes and the leading dot.
[Sanitized client evidence](evidence/manual-clients-macos.json) contains only
synthetic fixture content, compiler-independent production tree and binary hash;
no credentials, user profile or server users/config files are included.

CI static-analysis passed for PR source 005f267 with LLVM/clang-format 23.1.3,
all owned-source clang-tidy diagnostics, 63-file formatting, standalone public
headers and the acceptance matrix validator. [Recorded evidence](evidence/static-pr-005f267.txt).
The same-source workflow_dispatch quality matrix is still in progress.

## Final traceability review

The registry covers 79 distinct inventory features in 80 concrete rows. Each row
now points to the relevant observable configuration/protocol/storage tests rather
than a generic configuration smoke check. Runtime /dev/full exhaustion has its
own mandatory Linux row; portable throwing-sink unit checks remain mandatory
on Linux, macOS and Windows. Native /dev/full unavailability is never relabelled
as a passing e2e method. Every passing row still requires all named methods to
have executed and passed in matching-platform, matching-revision evidence.
The slow-reader preflight additionally requires the successful XOVER response
class, so an error response cannot masquerade as backpressure activity.

## Native Windows findings

Windows clang-cl executed and passed all applicable server e2e methods, including
CTRL_BREAK during connection/read/incomplete POST/external lock, and the native
lifecycle probe. [Scoped lifecycle evidence](evidence/windows-lifecycle-005f267.json)
does not claim the whole job passed: unit backpressure and baseline selectors
failed separately. Console close/logoff remain outside automated process-group
CTRL_BREAK coverage, as documented for the service.

The backpressure test limited only the receiver queue, so Winsock could buffer
the full 32 MiB locally and legitimately complete the send. The test now bounds
SO_SNDBUF as well as SO_RCVBUF before checking timeout/stop. No production socket
deadline changes. MSVC also rejected unreachable code after Catch FAIL; unknown
generator families now throw invalid_argument directly. Selector discovery now
uses explicit POSIX paths on every OS. These test-infrastructure failures remain
recorded in the 005f267 workflow; new runs require the corrected source.
