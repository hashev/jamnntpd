# Tasks

Задачи выполняются последовательно по зависимостям; тесты и документация входят в соответствующий этап. Критерии приемки и источники R01–R20 — design.md; внешние контракты — specs/*/spec.md. Ни одна задача реализации в propose не выполнена.

## 1. Baseline и воспроизводимые контракты

- [x] 1.1 Зафиксировать revision, инвентарь production-модулей и R01–R20 с источниками; проверить, что каждый finding имеет regression case и критерий закрытия, а предположения отделены от воспроизведенных дефектов.
- [x] 1.2 Добавить независимые бинарные fixtures JAM (headers/index/lastread/subfields/CRC, high-bit uint32 и corrupt/truncated варианты); проверить известные bytes тестовым decoder, не использующим production serializer.
- [x] 1.3 Зафиксировать protocol transcripts всех поддерживаемых команд/stubs, права/netmail и CLI/config options; проверить таблицу ожидаемого поведения по README/ReadMe/исходникам, не утверждая небезопасное поведение как контракт.
- [x] 1.4 Зафиксировать charset precedence и реальные .chs fixtures: aliases/force/default, asymmetric mapping, deletion/expansion и softcr; проверить golden bytes и документировать compatibility baseline.

## 2. C++23 сборка и ранние ворота

- [x] 2.1 Создать CMake targets core/jamnntpd/makechs и CTest с C++23 REQUIRED без extensions; проверить configure/build на доступном toolchain и compile-probe std::expected.
- [x] 2.2 Подключить закрепленные spdlog, ICU, Catch2 и Python e2e runner; проверить чистую воспроизводимую CI установку и документировать версии/команды.
- [x] 2.3 Добавить presets debug/release/asan-ubsan/tsan/coverage и warning flags из design; проверить smoke unit, compiler flag support и получение compile_commands.
- [x] 2.4 Добавить матрицу Core Guidelines и clang-tidy профиль для собственного кода; проверить первый отчет без скрытия baseline diagnostics, записать способ последовательного устранения.

## 3. Value types, ошибки, конфигурация и права

- [x] 3.1 Ввести Error/std::expected, value types и ограниченные byte/text interfaces; проверить lifetime/error tests и отсутствие owning raw pointers в новом слое.
- [x] 3.2 Перенести misc/parser/CLI/config на string/view и checked numeric parsing; тестами закрыть R01/R18, пустые/длинные поля, диапазоны, кавычки, документировать ошибки вместо усечения.
- [x] 3.3 Перенести allow/login/groups и параметры пользователя на RAII containers; тестами закрыть R02 и проверить guest/auth/read/post/netmail permissions без переполнения.
- [x] 3.4 Заменить небезопасную date/address обработку; проверить TZ/DST, отрицательные offsets, параллельные вызовы и отсутствие static-buffer races R14.

## 4. Перенос JAM storage

- [x] 4.1 Перенести serializer/index/CRC на fixed-width little-endian и константы размеров; проверить independent fixtures и UBSan high-bit cases R10/R11.
- [x] 4.2 Перенести base/file owners и подданные/subpack/lastread на RAII; проверить open/allocation failure cleanup, record offsets, пустые поля и truncated data.
- [x] 4.3 Добавить checked length/offset arithmetic и ограничения allocation во всех message operations; проверить corrupt corpus, boundary uint32 values и fuzz smoke без sanitizer findings.
- [x] 4.4 Реализовать canonical/file-identity registry и совместимые byte-range locks с guard; проверить два worker и отдельный locker process, aliases и timeout/error cases R12/R13.
- [x] 4.5 Реализовать explicit commit/flush и безопасную публикацию append/update/cancel; fault-injection тестами проверить каждый write/flush failure, отсутствие ложного успеха и читаемость прежних статей.
- [x] 4.6 Перенести оставшиеся используемые jamlib API на C++23, сохранить notices и документировать format/locking guarantees; проверить отсутствие production C targets для jamlib и все storage unit tests.

## 5. Unicode, CHS и MIME

- [x] 5.1 Добавить ICU codec adapter и strict/replace политики; exhaustive byte tests стандартных кодировок и UTF-8 invalid corpus проверяют code points, обратимость представимых bytes и диагностику потерь.
- [x] 5.2 Перенести .chs parser/executor и charset resolution без изменения явных mappings и byte-pass-through правил без таблицы, добавить явный -builtin; golden/negative tests проверяют R17, приоритет custom rules и отказ отсутствующего reverse mapping.
- [x] 5.3 Перенести makechs на C++23 RAII; проверить CLI, golden generated tables и error paths, документировать поддержку custom tables.
- [x] 5.4 Перенести MIME decode/encode/folding на безопасные текстовые интерфейсы; проверить encoded-word/QP/base64 boundaries, malformed MIME и UTF-8 на каждой границе.
- [x] 5.5 Перенести flowed/quote/softcr/nonbsp/delssq/addcr pipeline; тестами закрыть R15/R16, CP866 0x8D, NBSP с индекса 0 и финальные encoded-byte limits.
- [x] 5.6 Добавить e2e read/post/reply для каждой стандартной кодировки и пользовательской CHS, strict отказ до mutation и replace; документировать selection precedence и новые conversion_errors настройки.

## 6. spdlog диагностика

- [x] 6.1 Заменить os_logwrite/showerror и protocol debug на общий spdlog logger с thread-safe sinks; проверить long/concurrent records, noecholog/loggroups/debug и закрытие R08.
- [x] 6.2 Добавить redaction до logging boundary; проверить уникальным sentinel password все file/console/debug/error outputs при успешной и неуспешной AUTHINFO, закрыть R09.
- [x] 6.3 Добавить safe bounded fallback для sink errors и flush после worker join; проверить unwritable/full sink fault injection и документировать формат/политику отказа.

## 7. Сеть, сессии и lifecycle

- [x] 7.1 Перенести sockets на move-only owners и корректные native types Winsock2/POSIX; проверить cleanup и системные errors на Linux/Windows/macOS R19/R20.
- [x] 7.2 Реализовать send-all и bounded line reader с EINTR/timeout/disconnect handling; deterministic transport tests закрывают R06/R07, включая partial writes и command injection через overlong line.
- [x] 7.3 Реализовать admission reservation, owned jthreads и координацию stop/join; barriers/failure tests закрывают R04/R05, session cap и thread-start failure.
- [x] 7.4 Реализовать async-signal-safe POSIX notification и Windows control notification с пробуждением waits; проверить R03 и shutdown <=10 секунд под load/idle/lock contention.
- [x] 7.5 Отделить NNTP dispatch/session state от FTN mapping/storage; проверить все characterization transcripts, errors missing/deleted article, ranges и argument boundaries.
- [x] 7.6 Перенести POST/reply/cancel/netmail operations на новые слои; проверить права по каждому пути, корректные CHRS/CODEPAGE и отсутствие mutation после decode/auth/commit failure.
- [x] 7.7 Документировать transport limits, shutdown deadlines и сохраненные CLI/config behavior; проверить примеры end-to-end и отсутствие оставшихся manual resource cleanup chains.

## 8. Платформы и интеграционная приемка

- [x] 8.1 Подключить native Linux GCC/Clang, macOS AppleClang и Windows MSVC build/unit/e2e CI; проверить warnings-as-errors на всех targets и документированные toolchain minima.
- [x] 8.2 Удалить OS/2 и оставшийся старый production C build, обновить README/HOWTO/examples и переход с Makefile; проверить отсутствие ссылок на поддерживаемую OS/2 и сборку обоих executable targets.
- [x] 8.3 Запустить полный unit/e2e с ASan/UBSan и отдельный TSan unit+concurrency suite; проверить ноль errors/leaks/races, 100 stress повторений и отсутствие зависших процессов.
- [x] 8.4 Включить coverage gates >=90% lines и >=85% branches для всего собственного production code; проверить отчет unit+e2e, список denominators, module gaps и native Windows adapter evidence без скрытия error branches.
- [x] 8.5 Запустить закрепленный clang-tidy для всех production translation units и header paths; проверить ноль незакрытых diagnostics и review каждого узкого исключения в матрице Guidelines.
- [x] 8.6 Выполнить финальный построчный review diff и всех модулей, закрыть R01–R20/новые findings с regression evidence; проверить отсутствие незакрытых известных bugs/warnings, лишних generated files и секретов.
- [x] 8.7 Провести rehearsal миграции/rollback на копии JAM с independent reader/locker и совместимым tosser fixture; проверить backup согласованного набора файлов, чтение/запись старого формата и документированный отчет ограничений.
