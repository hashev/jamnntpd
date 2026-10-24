# Migration review register

Baseline revision: `a05db5daf83d8277dd08bc1c64c20c5781d3a968`. This register records source inspection, not successful runtime reproduction.

| Finding | Source evidence | Required regression / closure evidence |
|---|---|---|
| R01 / P1 | misc.c: strip, условие читает str[c] перед c>=0 | Дефект: чтение перед буфером на пустой строке и после удаления всех пробелов. Unit: пусто, CRLF, пробелы, ASan. |
| R02 / P1 | login.c: login копирует realnames[100] в var.realnames[36] | Дефект: переполнение при длинном пятом поле. Unit/e2e: длины 35/36/99 и отказ без порчи сессии. |
| R03 / P1 | os_linux.c и os_win32.c: sighandler вызывает блокировки | Дефект POSIX: async-signal-unsafe mutex, возможность deadlock. Windows-обработчик требует отдельного безопасного пути. Проверка завершения при удерживаемых рабочих блокировках. |
| R04 / P1 | os_linux.c: linux_srv глобален, pthread_create не проверяется, attr не уничтожается; worker detached | Ошибки управления ресурсами, риск гонки записи callback и работы потока. Тест отказа запуска, TSan, завершение сразу после accept. |
| R05 / P1 | main.c и nntpserv.c: учет соединений начинается внутри worker, detached startup не синхронизирован с shutdown | Риск освобождения общего состояния до старта worker; лимит проверяется после запуска потока. Тест барьера запуска и конкурентного admission. |
| R06 / P1 | sockio.c: socksendtext вызывает send один раз и проверяет только -1 | Дефект: короткая запись теряет хвост ответа. Детерминированный fake transport, backpressure e2e. |
| R07 / P1 | sockio.c: sockreadline возвращает неполную строку при len-1; sockprintf использует vsprintf | Дефект: длинная команда разбивается на несколько; переполнение форматирования. Тесты границ, oversized line и полного дренирования/закрытия. |
| R08 / P1 | os_*.c: os_logwrite использует logline[500], vsprintf, strcat | Дефект: неограниченное форматирование. Параллельные длинные записи проверять без обрезания и повреждения памяти. |
| R09 / P1 | nntpserv.c: server печатает полный вход при cfg_debug | Дефект конфиденциальности: AUTHINFO PASS попадает в stdout. Тест всех sink с уникальным секретом. |
| R10 / P1 | jamlib/jam.h: ulong предполагается 32-битным; message.c и lastread.c используют sizeof структур для дисковых смещений | Дефект переносимости на LP64: padding/размер не равен JAM-размеру. Сравнить offset/index/subfield/lastread с независимыми бинарными fixtures. |
| R11 / P1 | jamlib/structrw.c: jamgetulong умножает продвинутые до int байты до приведения к ulong | Риск signed overflow для старшего байта >=0x80. Тест 0x80000000/0xffffffff под UBSan; приводить до сдвигов/арифметики. |
| R12 / P1 | jamlib/mbase.c: POSIX fcntl блокирует первый байт; каждая сессия открывает собственную базу | Межпроцессная блокировка сама по себе не сериализует потоки одного процесса; закрытие fd может влиять на process-owned locks. Требуется реестр и координация lifetime всех handles одной базы. Тесты два потока + отдельный процесс. |
| R13 / P1 | jamlib/mbase.c: JAM_UnlockMB игнорирует fflush; JAM_LockMB(-1) возвращает 0 после любой ошибки кроме LOCK_FAILED | Дефекты: ложный успех сброса/захвата. Ввести fault injection write/flush/lock и запрет успешного NNTP-ответа при отказе. |
| R14 / P2 | misc.c: makedate; os_*.c: localtime; nntpserv.c: gethostbyaddr/inet_ntoa | Риски общего статического состояния и неверной TZ/DST-арифметики; переносимость API зависит от платформы. Тесты TZ/DST и параллельного форматирования/адресов. |
| R15 / P1 | unicode/ReadMe.txt, xlat.c: побайтовая таблица 256x4; mime.c: ручное folding | Односторонняя Unicode-схема не покрывает UTF-8 POST; возможен разрыв символов. Unit для всех границ encoded-word, unit/e2e двустороннего текста. |
| R16 / P2 | nntpserv.c: nonbsp ищет литерал с начальным NUL, цикл пропускает индекс 0 | Дефект поиска UTF-8 NBSP и обработки первого символа. Тесты NBSP в начале/середине/конце и соседних многобайтных букв. |
| R17 / P2 | xlat.c: chsgetbyte использует atoi/sscanf без полной валидации; таблицы допускают удаления/расширения | Риск неверных escape и потерь при попытке инверсии. Строгий parser с диагностикой, golden fixtures пользовательских таблиц. |
| R18 / P2 | allow/groups/main/misc: фиксированные поля, getcfgword молча усекает; login.c line не инициализирован | Дефект диагностики номера строки; риск изменения смысла путей/прав при усечении. Проверять числовые диапазоны, кавычки, длины и ошибочные конфиги. |
| R19 / P1 | nntpserv.c: централизованный ручной cleanup и множество ветвей ошибок; jamlib/subpack.c: malloc/realloc | Риски утечек и неполного освобождения при отказе. RAII и fault injection открытия/выделения/запуска. |
| R20 / P2 | Makefile.* используют -Wall без стандартов, test/static-analysis целей; os_win32.h использует старый winsock.h | Нет воспроизводимых ворот качества; риск усечений SOCKET/типов. CMake с C++23 без extensions, Winsock2 и native handle types. |

## Additional source findings

- R21 (P1, static): `JAM_GetSubfield` in `src/jamlib/subpack.c` uses a function-static packet and cursor; `JAM_AddMessage` traverses it in `src/jamlib/message.c`. Different messagebases can race even when each base has its own lock. Closure: value-owned iteration, two-base concurrent append regression under TSan.
- R22 (P1, static): `JAM_AddMessage` copies receiver subfield bytes into `User_AC[101]` without a length check. Closure: long/corrupt receiver subfield tests under ASan, checked parser and no fixed copy.
- R23 (P1, static): `command_post` sends `240 Article posted` before parent-thread updates and `JAM_UnlockMB` flushes. Closure: injected parent-update/flush failure cannot yield a success response.

Every finding remains **open** until its regression passes against the migrated production implementation. Baseline fixture checks do not close these findings.

- R24 (P2, static, migrated makechs regression covered): original `makechs.c` leaves firstdiff uninitialized for identical maps and bypasses explicit destination-map ASCII remaps. New tests verify identity-table generation and swapped ASCII destination bytes. Closure of the project-wide finding still requires final executable/platform acceptance.

- R25 (migration conformance, corrected in codec component): ICU IBM DOS aliases differ from the project's published Microsoft mappings for SUB/FS/DEL and CP437/865 MICRO SIGN; some Windows aliases also define control mappings for undefined bytes. Explicit adapter profiles and independent per-byte Unicode fixtures enforce the intended code-page definitions. Full service integration remains open.

## Integrated regression evidence (2026-10-24)

The historical source paths above refer to the recorded baseline revision. The old
production C has now been removed. The earlier component-stage open status is
superseded by the evidence below. Native Windows acceptance remains outstanding;
no platform-independent closure is claimed for R03/R19/R20 until it passes.

| Findings | Current replacement and regression evidence |
| --- | --- |
| R01, R18 | text/config checked parsing, empty/whitespace/long/quoted and numeric-boundary unit tests; executable CLI create/help/error and connection config rejection tests |
| R02 | value-owned Identity/session state, long realname config tests and actual authenticated/netmail read/post/cancel permissions |
| R03–R05 | SignalStop/WorkerPool/admission RAII, startup-failure barriers, actual idle/load/external-lock shutdown repeated 100 times; Linux TSan passes; Windows native gate pending |
| R06–R07 | deterministic short/zero/error writes, bounded fragmented line reader, timeout/closed socket tests; executable overlong-command injection and incomplete/aggregate oversized POST refusal |
| R08–R09 | spdlog concurrent long records and sink errors; actual AUTHINFO success/failure sentinel checks across log/stdout/stderr; bounded `/dev/full` fallback on Linux |
| R10–R11 | fixed little-endian format constants, independent binary decoder and high-bit field fixtures; Linux ASan/UBSan passes |
| R12–R13 | shared file-identity registry, native separate locker, alias/timeout tests, injected reads/writes/flushes/rollback and persistent quarantine; real backend/session failure never returns 240 |
| R14 | native timezone/DST process, negative offsets and concurrent date/address unit tests; numeric peer formatting avoids DNS/static-buffer lifetime |
| R15–R17 | exhaustive independently mapped standard code pages, UTF-8 malformed corpus, encoded-word boundaries; actual all-ten-encoding read/post/reply, CHS forward/reverse and byte-pass-through e2e; NBSP at index zero and CP866 0x8D tests |
| R19–R20 | all own targets C++23 with warnings as errors; native owners, failure cleanup and separate expected/jthread/stop_token probe; native Windows MSVC/clang-cl evidence pending |
| R21 | value-owned subfields, independent bases concurrently append/read different markers under TSan rather than sharing a function-static cursor |
| R22 | 32,000-byte receiver storage regression verifies full bytes without a fixed C buffer, plus malformed subfield corpus |
| R23 | real JamBackend + Session POST with injected write/flush failures verifies 441, unchanged parent header and no published index slot; 240 follows storage commit |
| R24 | makechs identical and explicitly remapped ASCII maps, CLI errors/warnings and independent parser roundtrip |
| R25 | independently generated byte-to-Unicode fixtures enforce DOS control/MICRO SIGN and undefined Windows-byte semantics; actual standard encoding executable roundtrips |

Additional findings resolved during integration:

- R26: stale authorization could race cancellation. `cancel_checked` now revalidates
  the entire authorized header/subfields/body under the mutation lock; changed
  snapshot tests reject mutation.
- R27: queued registry waits could multiply shutdown lock timeouts. Registry/open
  waits now observe stop and a single deadline; 100 contested executable shutdown
  repetitions verify no blocked post is published.
- R28: diagnostic callback under the notification mutex could deadlock on reentry.
  The callback now runs after release; a reentrant real-backend test completes and
  verifies all committed records.
- R29: a REPLYADDR display name could produce nested From angle brackets. Mailbox
  parsing now selects only the address; FTN rendering tests cover that exact form.
- R30: stored headers and accepted POST headers disagreed on horizontal tabs/control
  bytes. Tabs remain valid and other framing controls are rejected before publish;
  tests verify readable tab-bearing subjects and refused control-bearing headers.
- R31: outgoing MIME lost IBMPC CODEPAGE context. Both encoded headers and body
  Content-Type use the resolved codec name; CP866 metadata/pseudographic regression
  checks the label and exact legacy byte.
- R32: Clang/libstdc++ deduction of the 863-entry makechs fallback array exceeded
  expression nesting. Its explicit array type/count preserves contents without
  raising compiler limits; native Linux Clang builds and utility tests pass.

This register is evidence against known findings, not a claim that no undiscovered
bug exists. Final whole-diff review, native CI and quantitative gates are tracked in
verification.md and OpenSpec tasks.

- R33: native MSVC Debug uses checked std::array iterators, so declaring a ranges
  search result as `auto*` was non-portable. FTN labels now use a value lookup;
  configuration/dispatch/makechs lookups obtain bounded positions with standard
  iterator distance and checked array access. Existing command/CLI/map regressions
  cover successful and absent matches; native Windows rerun is required.

- R34: non-ASCII PID/NOTE and other X-JAM textual headers bypassed the selected
  read conversion. The read model now owns converted extra headers, rendering
  encodes them with the outgoing charset, and mismatched models/control injection
  fail. Unit and real IBMPC/CODEPAGE e2e cases verify decoded Unicode and wire bytes.

- R35: MSVC/clang-cl rejected deprecated `_wfopen` under warnings-as-errors.
  Native storage now adopts `_wfsopen` with explicit `_SH_DENYNO`, retaining wide
  paths and external tosser sharing. The secure `_wfopen_s` alternative is not used
  because its non-sharing semantics would break that contract. Native lock/tosser
  e2e must pass before acceptance; no CRT diagnostic is disabled.

- R36: Windows static vcpkg dependencies and CMake's default dynamic CRT disagreed.
  CI now selects the matching CMAKE_MSVC_RUNTIME_LIBRARY for all own targets;
  Debug/Release presets also choose the actual multi-config build/test configuration.
  Native linking and complete CTest must pass; no mismatch or duplicate-symbol check
  is bypassed.

- R37: debug records escaped CR/LF/NUL but could still send raw terminal ESC and
  other C0/DEL controls to the console sink. These bytes now become printable
  hexadecimal escapes before any sink. A regression verifies ESC/DEL absence and
  the exact escaped payload while preserving ordinary UTF-8 text and AUTHINFO
  redaction.

- R38: `-create` could serialize quotes/newlines/NUL into an unreadable configuration
  and truncate an existing destination before detecting any problem. Serialization
  now returns a checked expected result and refuses values the retained legacy
  grammar cannot represent, before opening the output file. Unit cases cover paths,
  origin and guest suffix; executable tests preserve an existing destination byte
  for byte after quoted/multiline input is refused. Runtime CLI values retain their
  previous semantics; no new configuration escape syntax is introduced.
