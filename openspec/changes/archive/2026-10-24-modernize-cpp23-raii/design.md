# Design

## Context

См. proposal.md о мотивации и specs/*/spec.md о внешних контрактах. Исходная архитектура: main.c разбирает CLI и принимает соединения; платформенные os_*.c создают потоки и реализуют журнал; nntpserv.c (3 336 строк) объединяет NNTP, MIME, FTN и операции базы; sockio.c отвечает за транспорт; allow/login/groups — за настройки и права; xlat/mime/misc — за преобразования; jamlib — за двоичное хранение; makechs — отдельная утилита. Тестов, CI, clang-tidy и coverage-конфигурации не обнаружено. Рабочее дерево уже содержит пользовательские untracked .agents/, .opencode/, openspec/; не изменять их за пределами этого change.

### Подробное первоначальное ревью

Это статическое ревью исходников, не результат запуска серверов, санитайзеров или clang-tidy. «Дефект» ниже означает видимую ошибку в коде; «риск» требует воспроизведения на конкретной платформе или входе. Нельзя утверждать, что перечень исчерпывает все ошибки.

| ID / приоритет | Область и свидетельство | Вывод и проверка исправления |
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

Полное приемочное ревью дополнительно проходит все пути command_post, command_abhs, command_xover, getparentinfo, cancelmessage и jamlib message/subpack/crc32/lastread: deleted/missing articles, ownership, проверки размера, arithmetic overflow, порядок публикации, ошибка каждого I/O. Найденное фиксируется с воспроизведением; обещание «все ошибки устранены» заменяется проверяемым отсутствием известных незакрытых дефектов и диагностик.

## Goals / Non-Goals

**Goals:** явное владение каждым ресурсом; независимое тестирование протокола, кодировок и хранения; сохранение формата и совместимости; наблюдаемая и воспроизводимая приемка.

**Non-Goals:** новые NNTP-команды, TLS, изменение модели аутентификации, новая база данных, автоматическая транслитерация, lock-free архитектура или обещание атомарности всей базы при потере питания. Удаление OS/2 согласовано. Публичный ABI встроенной C jamlib не сохраняется; все ее функции, необходимые проекту, мигрируют.

## Decisions

### 1. Архитектура и RAII

Разделить приложение на core library и тонкие executable targets jamnntpd/makechs. Модули: config/access, network/session, protocol, MIME/text, FTN mapping, JAM storage, logging. Не создавать иерархии ради замены C-функций классами: обычные value types и небольшие concrete owners предпочтительны.

Владеющие Socket, File, JamBase и JamWriteGuard — move-only; закрытие, unlock и join гарантируются scope. Деструкторы noexcept, явный commit/flush возвращает ошибку до ответа клиенту; destructor cleanup не скрывает ошибку успешной операции. std::vector/std::array для данных, std::string для текста, std::string_view только на гарантированно живой вход, std::span<std::byte> для двоичных данных. Native char* допустим только как заимствованный аргумент системного API; не использовать его для владения или внутренних текстовых интерфейсов. unique_ptr только при необходимой динамической lifetime, shared_ptr только для действительно общего владельца базы. Запрет свободных new/delete/malloc/free в прикладном коде. Переименование .c в .cpp без смены модели владения не считается миграцией.

Альтернатива оставить jamlib как C под RAII-wrapper отклонена: не выполняет полную миграцию и оставляет дефекты формата внутри. Мигрировать ее с сохранением исходных license notices.

### 2. Ошибки и инварианты

Ожидаемые ошибки parse/conversion/network/storage передавать через std::expected<T, Error> с категорией, контекстом и native error code, снятым немедленно. Неожиданные исключения ловить на границе main/worker; bad_alloc не должен оставлять захваченный lock или частично опубликованную статью. Конструктор либо создает валидный owner, либо сообщает отказ через factory/exception; невалидные полуинициализированные объекты не распространять.

### 3. Потоки, остановка и хранилище

Сохранить простую модель worker на принятое соединение с std::jthread и явным stop; admission slot резервируется под mutex до запуска worker. Callback передается по значению. Сервер владеет worker-ами, завершает и join-ит их до logger/config/storage; счетчики — RAII reservation. Остановка пробуждает блокирующие socket waits; deadline тестового shutdown — 10 секунд, интервалы проверок и lock waits ограничены. Не полагаться на DNS для admission и shutdown; использовать reentrant address API и ограниченный/отменяемый lookup либо числовой адрес.

POSIX: signal handler только async-signal-safe уведомляет управляющий поток через self-pipe; никакого logger/mutex/stop_source в handler. Windows: control handler уведомляет штатный event, координация идет вне handler. Alternative detached threads отклонена из-за отсутствия доказуемого lifetime.

Реестр баз по каноническому пути и файловой идентичности координирует внутрипроцессный mutex, открытые handles и process-owned POSIX lock lifetime. Не заменять совместимый lock на flock или только std::mutex: tossers используют блокировку первого байта .jhr. Сохранять этот протокол на POSIX/Windows; тест отдельным процессом подтверждает конфликт lock. Исключить обход через aliases/symlinks, а где идентичность проверить нельзя — отказ, не конкурентная запись наугад.

Сериализация JAM — только явные little-endian uint16_t/uint32_t и константы размера записи, checked offsets и большие host file offsets. Никакого sizeof(struct) на диске. Валидация размеров до выделения памяти. Публикация новой статьи — индекс/видимый header после подготовки body/subfields; при ошибке не публиковать индекс и пытаться восстановить контролируемые изменения под lock. Для отмены/обновления сохранять исходные затронутые записи до успешного commit; rollback обязателен при восстанавливаемом I/O-отказе. По подтвержденному пользователем уточнению при постоянной ошибке, не позволяющей rollback, помечать базу недоступной для последующей записи, сообщать о необходимости восстановления согласованного backup и не возвращать успех. Power-loss transactions не обещаются; тестировать каждый штатный injected I/O failure.

### 4. Кодировки и пользовательские таблицы

ICU обеспечивает проверенную переносимую конверсию стандартных кодировок вместо std::codecvt (устарел) или разных platform iconv/WinAPI mappings. Версию зависимости закрепить в воспроизводимой сборке и тестировать известные code-point mappings. В текстовом core — валидированный UTF-8; normalization отсутствует, чтобы не менять FTN-текст. Внешние байты и сырой .chs pipeline типологически отделены от UTF-8.

Сохранить legacy selection precedence из findreadxlat/findpostxlat: aliases, CHRS/CODEPAGE, group overrides, force/default rules; закрепить таблицей characterization tests до замены. Явные пользовательские mapping rules имеют приоритет над встроенной конверсией, отсутствие явного правила использует стандартный codec при известной паре. Пользовательская .chs применяется к байтам в объявленном направлении; ее выход валидируется согласно объявленному charset перед входом в Unicode-core. Для явно настроенных byte-pass-through правил (например, `read * unknown-8bit`) сохраняется явно выбранный byte-pass-through без ложного утверждения, что это UTF-8. Встроенную обратную конверсию не выдавать за инверсию пользовательской таблицы.

Сохранить все поддерживаемые .chs заголовки, escape, deletion/expansion, aliases, chsdir, keepsoftcr и утилиту makechs. Пользовательская таблица может быть lossy; exact round trip гарантируется для представимых символов стандартных кодировок и обратимых явно заданных custom mappings, а не для произвольных таблиц. Для DOS low-byte graphical conventions не считать управляющий байт рисунком автоматически: такие glyph mappings задаются .chs; стандартные controls/FTN framing не переопределять. Soft-CR обрабатывать в контексте source encoding до разрушительных преобразований, учитывать CP866 0x8D.

Политика conversion_errors=strict по умолчанию; новая явная настройка replace использует '?' для непредставимого legacy output и U+FFFD для ошибочного Unicode input, с предупреждением без исходного приватного текста. В strict режиме вся конверсия и проверка полей завершены до mutation. Известные лимиты JAM/FTN применять к итоговым encoded bytes, без разрыва символов; слишком длинные поля отклонять с диагностикой вместо усечения. Входные protocol buffers и POST имеют конечные документированные лимиты, согласованные с characterization fixtures.

MIME decode --> source decoding --> UTF-8 model --> destination encoding --> MIME encode. RFC encoded-word folding не разрывает кодовые точки, каждый сегмент самодостаточен. Сохранять quote/flowed/delssq/nonbsp/addcr semantics с регрессиями на границах.

### 5. Логирование

Использовать синхронный spdlog logger с thread-safe file/console sinks, timestamp/level/session. Это проще bounded async queue и не теряет записи из-за переполнения очереди. Сохранить logfile/noecholog/debug/loggroups. Не требовать byte-identical старого формата журнала. Redaction выполняется до sink; AUTHINFO PASS никогда не передается в logger, даже при debug. Не логировать полные private message bodies. Logger живет дольше worker-ов. Ошибка sink дает ограниченный stderr fallback без рекурсии; runtime log failure не обрывает обслуживаемую сессию, startup file failure видим пользователю.

### 6. Сборка и Core Guidelines

CMake/CTest с presets debug, release, asan-ubsan, tsan, coverage; CXX_STANDARD 23 REQUIRED и EXTENSIONS OFF. Targets для core, executable, unit/e2e и static analysis. find_package для spdlog/ICU/Catch2; CI закрепляет точные версии toolchain и dependencies, локальная сборка может использовать совместимые установленные пакеты. Не скачивать mutable master при configure. Минимальные версии определить compile-probe для C++23/std::expected, записать в README и presets; поддерживать GCC и Clang на Linux, AppleClang на macOS, MSVC на Windows.

Core Guidelines: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines ; spdlog: https://github.com/gabime/spdlog . Зафиксировать ревизию рекомендаций при implementation. Матрица применимости всех разделов P/I/F/C/Enum/R/ES/Per/CP/E/Con/T/CPL/SF/SL и profiles: rule id, applicable/not applicable, инструмент/ручная проверка, ссылка на свидетельство и обоснованное исключение. Особое внимание R.1/R.3/R.5/R.11, C.20/C.21, F.16, SL.str.1/SL.str.2, CP.2/CP.20/CP.21, E.6 и SF. Не требовать бессмысленного применения правил для отсутствующих возможностей (например, polymorphic hierarchy).

clang-tidy: clang-analyzer-*, bugprone-*, cert-*, cppcoreguidelines-*, concurrency-*, modernize-*, performance-*, portability-*, readability-* с проверенной конфигурацией под закрепленную версию. Точный набор опций формируется после первого запуска, но не отключается ради зеленого результата. Ограниченные NOLINT только с rule-specific причиной и review; нельзя массово скрывать warnings в собственном коде. Заголовки/исходники сторонних dependencies исключаются из собственных ворот, изменяемая встроенная jamlib включается.

### 7. Проверяемые ворота качества

- Все собственные production translation units, jamlib и makechs собираются как C++23 на Linux/Windows/macOS; в финале нет старого C production target или OS/2 build.
- GCC/Clang: -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wformat=2 и -Werror; MSVC /W4 /WX /permissive-. Различия поддерживаемых flags задаются per compiler, не suppressions дефектов.
- clang-tidy по compile_commands для всех собственных source/header путей: ноль незакрытых диагностик в закрепленном профиле.
- Catch2 unit: parser/access, session transitions, partial transport, MIME, encodings, custom tables, JAM serializers/CRC/lastread/subfields, lifetime/error injection. Fuzz smoke для config/MIME/CHS/JAM с фиксированными seed/corpus, обнаруженные случаи становятся регрессиями.
- Python e2e запускает собранный сервер на loopback ephemeral port с временной базой/конфигами, ждет readiness, всегда убирает child process; никаких реальных пользователей/баз. Проверяет полный цикл read/post/reply/cancel, права, netmail, команды/stubs, dot stuffing, deleted/nonexistent articles, charsets и shutdown/load. Golden fixtures не генерировать только проверяемой реализацией: ручные known bytes и независимый test decoder.
- Покрытие: агрегат unit+e2e на Linux Clang coverage, >=90% строк и >=85% ветвей для всего собственного production code, включая jamlib, makechs и POSIX adapters. Windows-specific adapters дополнительно покрываются native unit/e2e и отчетом их покрытия; Linux-отчет не является их покрытием. Не исключать неудобные ветви ошибок/OS adapters для порога; недостижимое явно обосновывается и согласуется при review. Отдельно показывать denominators, per-module coverage и пропуски; generated/external code и сами тесты исключены.
- ASan+UBSan: полный unit/e2e на поддерживаемом Linux Clang, ноль ошибок/утечек. TSan отдельной сборкой на поддерживаемом Linux runtime: unit+concurrency e2e, ноль диагностик; не совмещать с ASan. Остальные платформы: native build+unit+e2e. Отсутствие runtime не считается успешным sanitizer run.
- Конкурентные тесты используют barriers и bounded deadlines, а не надежду на sleep; stress минимум 100 повторений contested read/post/cancel/shutdown и проверка JAM invariants после каждого набора. Shutdown <=10 секунд на CI fixtures, без зависших worker/lock.
- Приемка требует закрытия всех R01-R20 и новых подтвержденных дефектов с regression evidence; никаких известных оставленных races/warnings. Статические и динамические проверки не доказывают отсутствие всех возможных ошибок.

## Risks / Trade-offs

- [Большой объем изменений] --> мигрировать слоями с characterization fixtures и зелеными воротами каждого этапа; финальная приемка только после всех этапов.
- [Различия ICU и исторических приблизительных aliases] --> сохранить явно заданные aliases/CHS, документировать exact mappings и тестировать CP1252/Latin-1 различия; не исправлять пользовательские таблицы автоматически.
- [JAM несколько файлов, внешние писатели] --> сохранить lock protocol, тестировать независимый процесс, backup перед запуском; RAII не является crash transaction.
- [Строгая валидация отвергает прежние поврежденные входы] --> диагностировать причины и записать изменения в migration notes, не воспроизводить memory corruption ради совместимости.
- [90/85 coverage не покрывает все interleavings] --> дополнять инвариантами, ручным ревью, fault injection и TSan.
- [С++23 на старых системах недоступен] --> явные минимальные toolchains и прекращение OS/2; не добавлять C++17 fallback.

## Migration Plan

1. Зафиксировать исходные протокольные/бинарные fixtures, review findings и baseline warnings до изменения поведения. Не использовать небезопасный старый бинарник как единственный oracle.
2. Ввести CMake и тестовые профили, затем переносить utility/config/access, JAM, codec/MIME, network/session и executable integration; каждый слой получает unit/negative tests в своем этапе.
3. Сохранить старые файлы сборки только на переходных этапах; финально удалить OS/2 и неиспользуемый C путь, обновить docs/examples и makechs.
4. На копии production JAM выполнить чтение, controlled post/reply/cancel и независимую проверку структуры/метаданных; проверить совместимый внешний locker/tosser до переключения.
5. Остановить старый сервер и tossers, сделать согласованную резервную копию .jhr/.jdt/.jdx/.jlr и config, запустить новый сервер на существующей базе. Новые настройки strict/replace документированы; старые configs принимаются.
6. Откат: остановить новый сервер, проверить совместимость успешно записанных данных старым reader; при подозрении на повреждение восстановить весь согласованный backup, явно учитывая потерю новых сообщений. Не откатывать отдельные файлы базы независимо и не держать старый/новый сервер одновременно без общего lock protocol.

## Confirmed compatibility clarification

On 2026-10-24 the user confirmed that existing explicit read/post rules without
a CHS file retain byte-pass-through semantics. The new `-builtin` option explicitly
selects standard transcoding and cannot be combined with a CHS file. An explicit
rule always takes precedence over automatic conversion. If no explicit rule
matches, known standard pairs may use the built-in codec; a configured custom
forward table never implies a reverse table. Rule selection retains the existing
first-match aliases, group/global forced defaults and destination constraints.
Component verification is recorded in `docs/migration/verification.md`.
