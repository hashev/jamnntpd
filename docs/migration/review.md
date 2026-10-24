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
