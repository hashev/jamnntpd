# Design

## Context

Исходная точка: CMake/CTest, Catch2, Python unittest, независимые baseline-декодеры JAM/CHS, реальные subprocess/TCP e2e, отдельные storage/lifecycle probes. В CMake зарегистрированы 10 групп, включая 100 конфликтующих раундов; test_server.py содержит 14 методов. Это инвентаризация исходников, а не результат нового запуска. Основные спецификации содержат 15 требований. Мотивация — proposal.md.

Текущий Server копирует небольшие fixtures в TemporaryDirectory, использует порт 0 и локальный TCP; Client автоматически добавляет CRLF и dot-stuffing. Поэтому этот клиент подходит для обычных сценариев, но может скрыть ошибки wire framing: негативные тесты должны использовать raw socket и независимо проверять точные байты.

## Goals / Non-Goals

**Goals:** проверяемая полнота по функциям и ветвям контракта, независимые ожидаемые результаты, воспроизводимость отказов и конкурентных историй, явные ограничения приёмки.

**Non-Goals:** доказательство отсутствия всех ошибок; 100% сочетаний всех настроек; новые функции или изменение совместимости; обещание сохранности незавершённого POST после аварийного завершения процесса.

## Decisions

### 1. Реестр функций и трассировка

Переиспользовать существующие тесты, а не создавать второй framework. В реализации создать docs/testing/plan.md, machine-readable tests/fixtures/acceptance-matrix.json и итоговый docs/testing/acceptance.md. В каждой строке: стабильный ID, requirement/source reference, функция/вариант, preconditions, вход, ожидаемый ответ/эффект, oracle, unit/integration/e2e, test selector, платформы, риск, статус и ссылка на evidence. Допустимые статусы: planned, implemented, passed, failed, blocked, not-applicable с причиной. Проверять уникальность ID и существование test selectors; наличие записи не означает passed.

Источники ожиданий: основные specs → согласованная документация/зафиксированная совместимость → независимые fixtures. Противоречия отмечать явно и разбирать до изменения assertions. В частности POST storage failure 503 в contracts.json расходится с 441 в Session::post и нынешними backend tests; нельзя выбирать ответ только потому, что его сейчас возвращает код.

Альтернатива «только coverage» отвергнута: исполнение строки не доказывает правильность ответа, приватность и состояние базы.

### 2. Матрица обязательных сценариев

Для каждой применимой строки обязательны happy path, отказ, граница и переход состояния. Результат — точный status/framing, содержимое и допустимые изменения файлов; отрицательная мутация сравнивается по всем четырём файлам.

| ID | Область | Обязательная программа | Основной oracle |
| --- | --- | --- | --- |
| CLI | Запуск/конфигурация | help, create, implicit/explicit config, порядок и aliases, кавычки/комментарии, пустые/неизвестные/повторные параметры, числовые границы, Unicode/пробелы в путях, отсутствующие/недоступные файлы, занятый порт, повторный запуск | exit code, stdout/stderr, созданный config и доступность TCP; сохранить немедленную семантику -create |
| CFG | Настройки | Каждая опция из config.cpp и command_help: port/max, все 6 путей, origin/guestsuffix/conversion_errors, 12 flags и smartquote, все def_* и 6 пользовательских on/off; дефолт, переключение и невалидное значение | разбор плюс наблюдаемый NNTP/FTN/log эффект; pairwise для прочих комбинаций |
| SES | Состояния | guest/login/failed login/re-login/options-only; selected/unselected group и current article; независимость двух сессий; отказ не меняет identity/options/current сверх контракта | модель состояния и TCP transcript |
| CMD | Все команды | ARTICLE/HEAD/BODY/STAT по current/number/id, GROUP, LIST default/ACTIVE/NEWSGROUPS/OVERVIEW.FMT, NEXT/LAST, XOVER single/closed/open range, AUTHINFO USER/PASS, POST, HELP, QUIT; IHAVE/NEWGROUPS/NEWNEWS/SLAVE/MODE и unknown/empty/mixed-case | точные коды, CRLF, multiline terminator и отсутствие лишних байтов; QUIT реально закрывает TCP |
| ACL | Доступ/netmail | allow first-match/wildcards/deny, read-only/post-only/guest/auth, группы с разным access, Alice/Bob/посторонний; все способы чтения и XOVER/LIST/GROUP; strictnetmail on/off; повторная авторизация с потерей прав | отсутствие приватного текста/недопустимого раскрытия по утверждённому контракту; файлы неизменны при отказе |
| POST | Публикация/reply/cancel | 340→240 только после commit; обязательные headers, crosspost, неподдерживаемые HTML/multipart/control, From/Subject/To, references, локальные/нетмейл группы; отказ foreign/non-local/sent/locked/no-cancel, повторная отмена; first/middle/last reply и TOCTOU snapshot | независимый JAM decoder, повторное чтение после restart, цепочки reply, CRC/атрибуты и echo notification |
| FTN | Заголовки/форматирование | CHRS/CODEPAGE/TZUTC/MSGID/REPLY/REPLYADDR/PID/NOTE, origin/tearline, noencode/notzutc/noreplyaddr/readorigin/note, local/nochrs; showto/flowed/nonbsp/delssq/addcr/squote, nostripre и подписи/цитаты | точные wire и stored bytes, metadata соответствует реальной кодировке |
| ENC | Unicode/CHS/MIME | UTF-8 и все 9 legacy encodings в обе стороны, кириллица/латиница/спецсимволы/псевдографика; byte maps; aliases/default/forced/CHRS/CODEPAGE precedence; label-only/-builtin/custom, level1/2/extended, deletion/expansion/asymmetric/keepsoftcr; strict/replace; malformed UTF-8, surrogate, emoji; encoded words, folding, QP/base64, 7/8bit и ошибки | независимые карты и Unicode code points; полный round-trip; strict отказ до мутации; replacement с диагностикой |
| NET | Транспорт/лимиты | raw fragmented/coalesced/pipelined requests без обещания новой NNTP-функции, CRLF/LF compatibility, dot-stuff/unstuff ровно один раз, NUL/CRLF injection, EOF в каждой фазе, short write/zero progress, slow reader/backpressure, read/write/idle timeout | raw bytes; отсутствие второй команды из обрезанного хвоста; bounded deadline и отсутствие частичного успешного POST |
| JAM | Формат/целостность | пустая база, один/много сообщений, gaps/deleted, last-read create/update/multiuser, большие subfields, binary unknown fields, пределы uint32/offset/count; повреждение каждого из .jhr/.jdt/.jdx/.jlr, подписи/длины/CRC там где контракт требует валидацию, циклы reply, missing companion | независимый struct decoder, счётчики/индексы/extent, untouched byte ranges, явная безопасная ошибка |
| IO | Отказы/блокировки | каждый существующий IoEvent read/write/flush/resize/lock и достижимый ordinal, частичное открытие, recoverable rollback, persistent failure/quarantine/reopen, stop и deadline при external lock; aliases/hardlinks где доступны | snapshots до/после, отказ 240, повторное открытие, отсутствие доступной частичной записи; quarantine отдельным subprocess |
| CON | Конкурентность/lifecycle | max-1/max/max+1, параллельный admission, startup failure, одна/разные базы, несколько процессов, read/post/reply/cancel/external tosser, SIGINT/SIGTERM и Windows CTRL_BREAK, stop на каждой фазе | уникальные номера/ID, согласованные успешные операции, нет deadlock/race/утечек; штатный stop <8 s |
| LOG | Логи | debug/noecholog/loggroups, path/flush, параллельные длинные записи, CR/LF/NUL/ESC/DEL, AUTHINFO success/failure/options/error, sink open/runtime failure | sentinel отсутствует во всех sinks, целые записи и безопасный bounded fallback; /dev/full только Linux |
| UTIL | makechs | аргументы/ошибки I/O/Unicode paths, identical maps/ASCII overrides/fallbacks/supplementary Unicode, неверные Unicode maps, parse generated CHS | независимый CHS parser/byte maps; platform EOL учитывается явно |
| OPS | Эксплуатация/interop | полный backup→внешняя запись→read/post→stop→restore→restart, сохранение lastread, клиентский read/reply/cancel сценарий, длинная история операций | четыре файла из одного snapshot; независимый tosser; внешние клиенты как отдельное evidence |

Границы проверять L-1/L/L+1: строка протокола 1000 байт с terminator, команда 998 после его удаления, POST 20000 после unstuffing; имена/subject/newsreader/address и финальные encoded-byte limits извлекать из post_builder/formatting; integer limits — из типов. Не путать длину Unicode с числом байтов. Последовательности auth→group→re-auth→read/post и post→reply→cancel→restart обязательны целиком, не заменяются pairwise.

### 3. Независимость и генеративные проверки

Baseline Python oracle не импортирует production serializer. Ожидания не генерируются из ответа проверяемого сервера. Новые корпуса включают пустую/удалённую/разреженную базу, неизвестные subfields, malformed companions, смешанные charsets и Unicode paths. Fixtures малы и обозримы; большой корпус строится детерминированно.

Добавить seeded генерацию последовательностей команд, допустимых JAM records/CHS/MIME и мутации длин/offsets/truncation. На PR: минимум 1000 bounded cases на семейство parser/JAM/codec и 100 модельных сессий; расширенный профиль: 10000 на семейство и 1000 сессий. Записывать seed, индекс, минимизированный reproducer; выявленное падение становится постоянным regression fixture. Catch2/Python достаточно; отдельную fuzz dependency не вводить. Альтернатива случайным нерепродуцируемым нагрузкам отвергнута.

### 4. Разделение быстрых и длительных прогонов

- PR: все существующие 10 CTest групп, новые детерминированные сценарии и короткая генеративная программа; сохранить 100 конфликтующих раундов.
- Extended через workflow_dispatch: 30 минут непрерывной смешанной нагрузки на Linux, macOS и Windows с 1/5/32 клиентами и max=32 в последовательных фазах. Корпус 10000 сообщений/база, две базы, raw slow clients и external locker; фиксированный seed. Таймаут job 60 минут. Это выбранный приёмочный профиль, а не обещание производительности.
- Сохранять throughput, p50/p95/p99 и максимальный shutdown, RSS и native descriptors/handles через интервалы; абсолютный latency SLO не придумывать. После warm-up снять quiescent baseline, повторить 100 connect/disconnect cycles, затем повторное quiescent измерение: handles/fds не растут; RSS оценивать трендом с учётом allocator retention, подтверждать утечки sanitizer-инструментами, не требовать точного возврата RSS.
- Fault/sanitizer сценарии изолированы от длительной производительной нагрузки. Прежние timeouts не растягивать ради прохождения; расширенным группам назначить собственные deadlines.

Реальный idle default равен 5 минутам. Как минимум один extended native прогон на каждой ОС должен проверить его без изменения production default; меньшие deadlines проверяются existing socket API в unit/probe. Thread/resource launch failures проверять имеющимся ThreadLauncher seam.

### 5. Native matrix, совместимость и evidence

Сохранить Linux GCC/Clang, macOS AppleClang, Windows MSVC/clang-cl; отдельные Linux ASan+UBSan и TSan с инструментированными зависимостями. Coverage Linux и native Windows ≥90% lines/85% branches по всем own sources/headers и пяти исполняемым объектам; не исключать ошибки и неудобные файлы. clang-tidy, compiler warnings-as-errors, formatting и standalone headers остаются обязательными; добавить CI-вызов статического анализа, сейчас quality.yml запускает native/instrumented builds без отдельного tidy job.

Внешняя совместимость: автоматический независимый raw TCP клиент и Python fake_tosser обязательны; дополнительно провести документированные ручные read/post/reply/cancel/reconnect сценарии Thunderbird и одного терминального клиента (например slrn), записать фактические версии/ОС и обезличенный transcript. Отсутствие установленного клиента фиксируется blocked, не считается pass; не подменять этим автоматические проверки. Это совместимость выбранных клиентов, не сертификация всех клиентов/tossers.

Отчёт привязан к commit SHA, toolchain/dependency versions, ОС, seeds, командам, testcase ID и артефактам CTest/JUnit/logs/coverage. Платформенные not-applicable допустимы только для реального OS-specific механизма с native альтернативой; обязательные scenarios нельзя скрывать skip-ом.

### 6. Критерии завершения

1. Все 15 требований и каждый элемент CLI/commands/options/flags имеют traceability; 100% обязательных строк матрицы implemented и passed на применимых платформах. Unmapped и planned/blocked/failed не принимаются как завершение.
2. Все команды проверены через настоящий jamnntpd с положительными, отрицательными, boundary/state сценариями; storage API lastread, fault seams и недостижимые wire ветви дополнительно проверены unit/probe, без выдумывания lastread NNTP-команды.
3. Нет нарушения приватности, публикации неполного POST, порчи committed JAM, races, sanitizer findings, compiler/tidy warnings проекта; никаких отключений assertions/check families для зелёного результата.
4. PR/native/sanitizer/coverage и extended/interop evidence относятся к одному принятому source SHA; новая source-правка требует повторить затронутые проверки и всю обязательную final matrix.
5. Все найденные дефекты имеют воспроизводящий regression и исправление либо остаются явным blocker. Порог покрытия необходим, но не заменяет функциональную матрицу.

## Risks / Trade-offs

- Существующие golden transcripts могут закреплять исторические дефекты → разбирать противоречия отдельно, сохранять утверждённую совместимость, не обновлять snapshots массово.
- Встроенный Client скрывает wire дефекты → независимые raw-byte проверки и duplex tests с deadlines.
- Конкурентные тесты flaky → barriers/handshakes, bounded waits, детерминированные seeds, сохранение истории; повторный успешный run не отменяет сбой.
- 30-минутная нагрузка дорога для PR → отдельный запускаемый extended workflow, обязательный для финальной приёмки.
- Мало наблюдаемости для production faults → existing seams/probes и явное разделение реального OS failure от injected; новые production seams требуют отдельного решения.
- kill/диск/power loss → никаких новых гарантий; только безопасная диагностика при следующем открытии и процедура восстановления целого backup. Проверки fail_io не доказывают fsync/crash durability.

## Migration Plan

1. Зафиксировать исходный SHA и выполнить существующий baseline без изменения тестов.
2. Заполнить реестр, классифицировать пробелы и спорные контракты, затем добавлять тесты по риску: privacy/commit/storage → protocol/state → encodings/config → extended/interop.
3. Вводить CTest labels/профили и CI без удаления имеющихся gates. Проверить что новый негативный сценарий действительно ловит контролируемое нарушение oracle/test double, не меняя production ради проверки.
4. Выполнить всю матрицу и выпустить acceptance report. Откат тестовой инфраструктуры не затрагивает базы; тестовые процессы и временные файлы очищаются даже при timeout, failure artifacts остаются в CI.
