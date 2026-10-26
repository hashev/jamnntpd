# Tasks

## 1. Исходная точка и реестр

- [x] 1.1 Зафиксировать SHA, toolchain/dependency versions и команды в docs/testing/plan.md; выполнить существующие 10 групп CTest без изменения assertions и сохранить исходные результаты.
- [x] 1.2 Создать acceptance-matrix.json с ID/требованием/вариантом/входом/oracle/test selector/platform/status/evidence; сопоставить все 15 требований, все команды и каждый элемент таблиц config.cpp; проверка — нет unmapped функций.
- [x] 1.3 Добавить валидатор матрицы и тесты его отказов на duplicate ID, неизвестный selector, обязательный skip и passed без evidence; проверить, что испорченная матрица отклоняется.
- [x] 1.4 Разобрать конфликт POST 503/441 по specs, историческим transcript и документации; записать обоснованное решение в plan.md, изменения контракта вынести на согласование, ожидаемый ответ закрепить регрессионным сценарием.

## 2. Независимые fixtures и транспортный harness

- [x] 2.1 Добавить обозримые пустые/deleted/gapped/mixed-charset/unknown-subfield/lastread fixtures и детерминированный генератор большого корпуса; проверить независимым jam_oracle и сохранить происхождение данных в plan.md.
- [x] 2.2 Дополнить Client raw-byte/duplex режимами с deadlines и ограничением read size; тестами проверить, что harness обнаруживает неверные CRLF/dot terminator/лишние байты и EOF, а не нормализует их.
- [x] 2.3 Сделать cleanup/failure artifacts воспроизводимыми: timeout и exception завершают subprocess и освобождают файлы/порты; тест harness failure подтверждает отсутствие дочерних процессов и наличие seed/transcript/snapshot.

## 3. NNTP-команды, состояния и доступ

- [x] 3.1 Реализовать CMD/SES матрицу через unit плюс настоящий executable: все 18 команд, LIST варианты, unknown/empty/mixed-case, current/number/id, NEXT/LAST/XOVER границы; проверить точные коды/framing/QUIT и обновить traceability.
- [x] 3.2 Добавить переходы auth/group/current, failed re-login, потерю прав, options-only и изоляцию двух сессий; модель и e2e должны согласиться по состоянию после каждого шага, включая отказы.
- [x] 3.3 Реализовать ACL матрицу guest/read-only/post-only/Alice/Bob/посторонний и strictnetmail on/off для всех способов доступа; verify отсутствия приватного содержимого и недопустимых изменений JAM, документировать допустимую семантику counters.

## 4. POST, FTN и настройки

- [x] 4.1 Покрыть обязательные/неверные headers, unsupported MIME/control/crosspost, read-after-post/restart и 240 только после commit; сверить все четыре JAM файла независимым decoder и заполнить POST evidence.
- [x] 4.2 Покрыть reply ancestry, first/middle/last cancel, foreign/non-local/sent/locked/no-cancel/repeated cancel и TOCTOU; проверить связи, counters, атрибуты и отсутствие мутации при отказе.
- [x] 4.3 Добавить CLI/config parameterized проверки всех aliases, путей, числовых границ, precedence/quotes/comments, immediate -create, unknown/duplicate/missing values; документировать поведение и verify exit/config/TCP outcomes.
- [x] 4.4 Для каждой boolean/default/user option проверить default/on/off и наблюдаемый эффект; покрыть FTN headers/origin/tearline/NOTE/reply addressing/notification и обязательные межопционные цепочки; verify wire/stored bytes, pairwise отчёт остальных сочетаний.

## 5. Кодировки, CHS, MIME и makechs

- [x] 5.1 Расширить ENC unit/e2e на UTF-8 и все 9 legacy encodings в обе стороны, все определённые byte mappings, псевдографику и special symbols; проверить независимые Unicode vectors и сохранность round-trip.
- [x] 5.2 Проверить precedence aliases/default/force/CHRS/CODEPAGE, automatic/label-only/-builtin/custom и несовместимые правила; verify содержимое/metadata и неизменность базы при strict отказе.
- [x] 5.3 Дополнить CHS level1/2/extended, deletion/expansion/asymmetry/keepsoftcr и malformed tables; для каждого verify bytes либо filename/line diagnostic, без выдуманного inverse.
- [x] 5.4 Дополнить MIME folding/encoded-word/QP/base64/7bit/8bit, malformed sequences и L-1/L/L+1 final-byte границы, flowed/NBSP/quotes; verify Unicode boundaries, framing и replacement diagnostic.
- [x] 5.5 Покрыть UTIL CLI, карты/ASCII overrides/fallbacks/supplementary/error paths/Unicode paths; проверить generated CHS независимым parser и native EOL, обновить руководство запуска.

## 6. JAM, отказы и эксплуатационная целостность

- [x] 6.1 Дополнить JAM read/append/cancel/lastread и corrupt companion/extents/offset/count/cycle сценарии; verify независимые records, untouched bytes и безопасный отказ без unbounded allocation.
- [x] 6.2 Проверить достижимые ordinals read/write/flush/resize/lock, recoverable rollback и постоянную quarantine отдельными процессами; verify snapshots всех файлов, отказ 240 и поведение reopen/restore/restart.
- [x] 6.3 Дополнить межпроцессные lock/timeout/stop и file identity alias сценарии; verify bounded wait и отсутствие публикации при конфликте, явно отметить native недоступные механизмы с альтернативой.
- [x] 6.4 Расширить OPS independent tosser/backup rehearsal с lastread и restart; verify точное восстановление четырёх файлов и документировать stop-all-writers процедуру без crash-durability обещаний.

## 7. Сеть, lifecycle и диагностика

- [x] 7.1 Проверить fragmented/coalesced/raw requests, dot stuffing, command 998/line 1000/POST 20000 границы, injection и EOF каждой фазы; verify отсутствие хвостовых команд и incomplete commits.
- [x] 7.2 Проверить short/zero writes, backpressure, медленное чтение, deadline и остановку waits через existing socket APIs/probes; verify bounded outcomes без увеличения текущих deadlines.
- [x] 7.3 Дополнить max-1/max/max+1 и thread startup failures, одну/две базы и external contention; verify уникальные номера/полную историю успешных операций и shutdown <8 s во всех 100 раундах.
- [x] 7.4 Покрыть SIGINT/SIGTERM/Windows CTRL_BREAK при idle/read/post/lock/startup, проверить flush после join и отсутствие leaked workers/handles; документировать непроверяемые console close/logoff ограничения.
- [x] 7.5 Дополнить LOG flags/parallel records/terminal controls/sink failures; verify отсутствие credential sentinel во всех sinks, ровные записи и bounded fallback, сохранить платформенные причины applicability.

## 8. Генеративные и длительные проверки

- [x] 8.1 Реализовать seeded parser/JAM/codec генераторы и модельные последовательности сессий; verify минимум 1000 cases/семейство и 100 сессий PR, сохранение seed/reproducer, regression fixtures для найденных ошибок.
- [x] 8.2 Добавить extended профиль 10000 cases/семейство и 1000 сессий; verify детерминированный повтор seed и контролируемый лимит времени/памяти, описать команды в plan.md.
- [ ] 8.3 Реализовать 30-минутный mixed-load профиль с 1/5/32 клиентами, двумя базами по 10000 сообщений и external locker; сохранить latency/throughput/resource временные ряды и проверить JAM invariants после штатного stop/restart.
- [ ] 8.4 Добавить warm-up/quiescent проверки fd/handles и 100 connect/disconnect cycles, RSS trend и leak diagnostics; verify нет роста native resources, без ложного требования точного возврата allocator RSS.
- [ ] 8.5 Выполнить реальный 5-минутный idle timeout в extended профиле каждой ОС; verify закрытие в документированном bounded window и отсутствие изменения production default.

## 9. CI и внешняя совместимость

- [x] 9.1 Подключить новые CTest labels/матрицу к native Linux GCC/Clang, macOS AppleClang, Windows MSVC/clang-cl; verify все обязательные сценарии выполняются, прежние группы не удалены, skip не превращается в pass.
- [x] 9.2 Обеспечить отдельные ASan+UBSan/TSan и Linux/Windows whole-production coverage ≥90% lines/85% branches; проверить CI failures на недостигнутый gate, без исключений own files/error paths.
- [x] 9.3 Добавить CI запуск clang-tidy согласованного профиля, formatting и standalone header checks; verify warnings-as-errors и toolchain versions, не отключать семейства диагностики.
- [ ] 9.4 Добавить workflow_dispatch extended native jobs с 60-минутным budget/job и выгрузкой JUnit/CTest/transcripts/seeds/snapshots/metrics; выполнить по одному полному прогону каждой ОС и проверить артефакты.
- [x] 9.5 Провести Thunderbird и slrn (или эквивалентный терминальный клиент) read/post/reply/cancel/reconnect на disposable server; записать реальные версии/ОС/результаты и обезличенный transcript, отсутствие клиента учитывать как blocker соответствующей строки.

## 10. Итоговая приёмка

- [ ] 10.1 На одном принятом source SHA выполнить всю PR/native/sanitizer/static/coverage и extended матрицу; сверить отчёты по commit и платформам, никаких повторных runs для сокрытия нестабильных отказов.
- [ ] 10.2 Сформировать docs/testing/acceptance.md с evidence для каждого обязательного ID, найденными дефектами и исправлениями/regressions; verify 100% обязательных строк passed, нет unmapped/failed/blocked и спорных ожиданий.
- [ ] 10.3 Проверить final diff, отсутствие production data/secrets/неожиданных generated artifacts и запустить openspec validate comprehensive-functional-testing --strict; completion только после выполнения критериев design.md.
