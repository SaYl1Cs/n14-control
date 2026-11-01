## Runtime sampler isolation and polling cost

The native read-only sampler publishes to `/run/n14-control/temperatures.json`
and holds `/run/n14-control/sensors.lock`. The legacy read-only producer, when
it is present, remains on `/run/colorful-n14-control/temperatures.json` and its
own lock. These paths must not be shared: a second producer changing the
session/generation while a transaction is being validated is a provenance
failure, not a reason to retry. The hardware service keeps the native path
read-only from its side and fails closed on stale or changed samples.

The native producer waits interruptibly for its one-second interval instead of
polling a stop flag at 50 Hz. The GUI performs passive telemetry reads on one
coalescing worker and samples the UI history every five seconds; no synchronous
sysfs/ACPI read runs on the GUI event loop. A bounded history and one pending
worker request cap memory and prevent timer backlog.

# Privileged EC/ACPI writer — implementation contract

Offline contract и production-facing implementation теперь находятся в
`reference_ownership`, `reference_transaction`, `reference_broker` и
`service/service.cpp`. Root service умеет создать только фиксированный lock file,
получить Polkit authorization, проверить production identity, держать lease,
передать provenance-bound command в одну очередь и отозвать authority при
ошибке. Bounded live application-level write/tach test предыдущего API выполнен
2026-10-31 (см. `VALIDATION.md`). Service-managed EC protocol 2 затем отдельно
установлен и прошёл bounded live test: CPU tach `2900 → 4500` nominal RPM,
12/12 raw readbacks и точный restore targets/keylight/mode. Это не проверка
длительной нагрузки, sleep/resume или restart с активным EC owner; этот документ
не заменяет physical validation.

Отдельный последующий clean active-owner restart проверен native backend probe и
D-Bus audit: один явный no-op CPU target command до restart, ни одного Apply после,
новый unique service owner, сброс EC-active и свежее явное наблюдение; targets,
mode и keylight не изменились. Receipt — в `VALIDATION.md`. Это не проверка
in-flight/partial-write restart, sleep/resume или thermal load. Generation counter
broker-а process-local: смену процесса нельзя определять только по его росту.

## Текущий IPC/lifecycle contract — EC protocol 2

### Единственный фоновый клиент (2026-10-31)

Уточнение после общего EC-аудита: чистая смена validated sampler epoch получает
новое broker generation до расчёта demand. Только typed no-write GPU transition
может перейти в bounded `waiting` с исходным 5-секундным original-age окном;
по явному указанию пользователя для этого observation-only ожидания нет
температурного cutoff исходной hold-policy. Никакая запись по старым данным
не разрешается. Generic observation
и все реальные write/readback ошибки остаются terminal. Брокер и клиент должны
обновляться вместе. Детальный контракт и сквозные тесты:
`EC_STACK_AUDIT.md`. Проверки replay/epoch в transaction queue не ослаблены.

В рабочем режиме protocol-2 клиентом служит unprivileged `n14-fan-controller`
в пользовательском systemd-сеансе. Он исполняет Graph/Standard каждую секунду,
читает сохранённый конфиг и предоставляет GUI только ограниченные session-bus
методы `Status`, `SetMode(manual, duty)` и `SetKeylight(percent)`. Там нет
произвольных команд, путей, регистров или передаваемой GUI provenance. GUI
больше не запускает собственный EC-loop. Root transport/lock/Polkit остаются
исключительно в `n14-hardware.service`. Два экземпляра фонового клиента не могут
зарегистрировать одно session-bus имя; резервирование имени предшествует
созданию аппаратного клиента.

Любой observation/write/readback/config error останавливает автоматические
запросы до явного действия пользователя. Смена root service owner также
terminal. Нет automatic process restart или recovery writes. После чистого
resume допускается только новый расчёт по новой provenance, не повтор старого
target. Если сон прервал операцию, автоматическое продолжение запрещено.
Демонстрационные/test harnesses по-прежнему имеют отдельный явный backend;
write-probes не запускаются одновременно с рабочим фоновым контуром.

- Публичные EC методы: `GetEcProvenance()` и `ApplyEc(json)`. Отдельные
  `AcquireEc`/`ReleaseEc` удалены, а не перенесены в скрытый GUI flow.
- Первый запрос проходит bounded noninteractive Polkit, pinned identity и lock
  preflight до открытия transport. Lifecycle целиком принадлежит root service;
  GUI не управляет descriptor-ами и не останавливает конкурирующие сервисы.
- Авторизация связана с unique D-Bus caller до его disconnect. Конкурирующий
  caller отклоняется, в том числе во время незавершённой авторизации. Sleep и
  исчезновение caller до завершения Polkit не могут открыть broker задним числом.
- `GetEcProvenance` возвращает проверенную identity, generation и snapshot.
  `ApplyEc` не синтезирует provenance, даже если это первый запрос сессии.
- После revoke новый запрос вновь проходит identity/lock preflight и получает
  новое поколение. Cached authorization не означает разрешения повторить старую
  частичную запись. Старые targets автоматически не восстанавливаются.
- Disconnect, sleep, invalid command владельца, failed transport/write/readback
  закрывают broker. Transient thermal observation failure не пишет в hardware.
  Освобождение lock не переключает firmware mode и не сбрасывает duty.
- Клиент на `PrepareForSleep(true)` и unique-owner change сбрасывает только
  локальное EC-active/generation/epoch/Standard состояние; lease остаётся задачей
  сервиса. Session revision отсекает поздние provenance/Apply replies и уже
  поставленное в очередь post-command observation, не переотправляя команду.
  Bounded load probe при любой ошибке прекращает собственную нагрузку без
  компенсации; отдельный `--restore-report` — новая явная операция, а не retry.
- Базовый `Capabilities.protocol=1` для CPU сохранён, EC требует `ec_protocol=2`.
  Новый GUI/harness не посылает EC requests старому сервису.

## Диагностика отклонённой транзакции — offline follow-up

Development service сохраняет причину queue failure в D-Bus error как
`diagnostics=<compact JSON>` и пишет тот же JSON с journal marker
`N14_EC_TRANSACTION_REJECTED`. Success reply и публичные методы не меняются.
JSON содержит `stage`, `register`, `expected_raw`, `actual_raw`, `write_attempts`,
`hardware_state_uncertain`, bounded `reason`, sample ages на UNIX/BOOTTIME,
`sample_id`, `calculation_epoch` и `broker_owner_generation`.

Этап устанавливается **до** соответствующего transport call. Счётчик увеличивается
до write, потому что исключение после write не доказывает отсутствие записи.
Readback mismatch сохраняет фактический byte; неизвестные raw values равны `-1`.
Failure после хотя бы одной попытки записи помечается uncertain и остаётся
terminal. Нулевой счётчик относится только к этой команде и не доказывает, что
состояние после предыдущей ошибки известно. Это не trace ACPI packets, не повод
для автоматического retry/restore и не новый capability клиента.

Все transport failure boundaries fan/keylight и передача diagnostics через
private D-Bus покрыты offline tests. Этот development build ещё **не установлен**;
причина старого физического отказа неизвестна. Следующие deployment/live writes
требуют отдельного явного разрешения; sleep/load gates не считаются пройденными.

## Граница компонентов

GUI-side components остаются unprivileged/read-only относительно EC:

1. `RuntimeSnapshotProvider` читает только опубликованный JSON snapshot.
2. `RuntimeThermalSamplerLoop` принимает только validated thermal samples и
   вычисляет dry-run input; он не получает hardware handle.
3. Graph/Standard layers вычисляют demand и отправляют только typed command через D-Bus.
4. `Window` не открывает EC; владельцем EC является отдельный root D-Bus service.
5. `evaluateEcOwnership()` проверяет только заранее собранные facts; он сам не
   выполняет OS probes и не приобретает ownership.

Будущая схема должна иметь одного отдельного владельца:

```text
unprivileged UI / dry-run loop
        -> typed, provenance-bound demand
        -> private IPC
        -> root-only EC broker
        -> one serialized ACPI/EC transaction queue
```

Ни UI, ни runtime sampler, ни отдельный keylight path не должны открывать
`/sys/kernel/debug/ec/ec0/io`, `/proc/acpi/call` или индексированный fan
transport для записи.

## Не смешивать sensor и control transports

Из pinned N14 contract следуют разные интерфейсы:

- read-only temperature producer использует EC input `0x56` (CPU) и `0x58`
  (GPU), включая suspend-aware `active/D0` и `suspended/D3cold` semantics;
- будущий control adapter использует проверенный ACPI method
  `\_SB.AMW0.WMBA` и его индексированный transport `0x302`;
- fan targets и keylight registers не являются temperature offsets и не должны
  быть доступны через общий произвольный register API;
- exact model/BIOS/EC/DSDT/WMI checks обязательны до любого control operation.

Нельзя считать успешное чтение temperature byte доказательством права на fan
write. Нельзя заменять pinned ACPI protocol прямой записью в `ec0/io` или
универсальным debugfs writer-ом.

## Exclusive ownership

Будущий broker получает право на control только если одновременно выполнены
все условия:

1. root-owned private runtime directory `/run/colorful-n14-control` имеет
   ожидаемые owner/group/mode;
2. broker открывает root-owned `ec-owner.lock` через `O_NOFOLLOW|O_CLOEXEC`,
   проверяет regular file, owner/group, link count и private mode, затем держит
   `flock(LOCK_EX|LOCK_NB)` на descriptor **всё время жизни процесса**;
3. известные конкурирующие владельцы (действующий CoolerControl adapter,
   legacy `nbfc_service` и другие явно подтверждённые ACPI controllers)
   обнаружены как остановленные или handover завершённым; broker не останавливает
   их автоматически;
4. identity preflight подтверждает только поддержанный N14 24 / BIOS 2.07 /
   EC 2.0 / controller `55:71` / pinned DSDT и WMI contract;
5. sleep/shutdown state допускает transaction.

`flock` координирует только участников, которые используют тот же lock. Он не
доказывает отсутствие произвольной внешней ACPI-программы, поэтому lock нельзя
выдавать за универсальный hardware lease. При неопределённости broker должен
отказаться от control, а не пытаться “починить” конфликт остановкой чужого
сервиса или записью в EC.

Lock acquisition и identity checks выполняются сервисом до первой EC-записи.
`EcReadOnlyProbe` собирает только metadata-факты
через injected filesystem/process interfaces и POSIX adapters; отдельный
`EcOwnershipLease` проверяет уже существующий lock и удерживает descriptor
в lifetime broker-а. Probe намеренно не удерживает lock,
а EC controller ID/transport приходит только от injected pinned identity source.
Broker integration реализована. Автоматический старт broker-а по IPC не обходит
preflight и не выполняет handover чужих cooling services.

## Typed command boundary

Будущий IPC не должен принимать от UI голый `duty` или raw register/value.
Команда должна содержать как минимум:

- role/target (`cpu`, `gpu`, `both_fans`, `keylight`);
- bounded logical value и преобразование по pinned contract;
- sampler session, generation, sample ID и original sample clocks;
- calculation/assignment epoch;
- broker owner/session generation;
- explicit sleep/handover state.

Broker повторно проверяет provenance, ordering, freshness и ownership перед
каждой transaction. Ответ “accepted by IPC” не является подтверждением записи;
успехом считается только подтверждённый protocol readback, если это требуется
закреплённым источником.

Offline `reference_transaction.{h,cpp}` уже моделирует эту границу: typed
provenance, bounded fan/keylight values, one serialized queue, pinned fake
register order, readback и terminal revoke после partial failure. Это не live
broker и не доказательство физического fan/light response.

Очередь должна быть общей для fan и keylight. Независимая запись подсветки
запрещена: на N14 изменение brightness связано с firmware fan mode и должно
проходить через того же единственного владельца и тот же serializer.

## Failure and lifecycle policy

До первой записи требуются identity, ownership, sleep state, fresh validated
sample и разрешённый transaction. При любой из следующих ситуаций authority
отзывается:

- missing/stale/replayed/mismatched thermal provenance;
- producer session/generation rollback;
- потеря lock или обнаружение конкурирующего владельца;
- sleep preparation, broker restart, IPC disconnect или expired deadline;
- invalid command, failed write, partial write или failed readback.

В соответствии с закреплённой no-emergency-override policy отказ **не** должен
автоматически писать 100%, 50%, 5% или 0%, не должен ретраить неизвестную
частичную запись и не должен восстанавливать кэшированную старую target без
нового подтверждения. Последняя hardware command может оставаться в firmware
неограниченно долго; это исходный риск, а не гарантия thermal safety.

После broker restart нельзя принимать сохранённые targets как подтверждённые.
Нужны новый ownership proof, новый validated snapshot и protocol readback.
Повторное чтение того же snapshot не продлевает deadline.

## Порядок будущей transaction

**Обновление подсветки 2026-10-31:** пользователь разрешил перенос физически
проверенного preload-before-auto порядка. Если исходный режим manual, очередь
кэширует отдельные targets, проверяет continuation guard и записывает новый
light byte ДО единственного перехода auto. Затем одна пауза 20 мс, проверка auto,
guard, обычная light write, guard и однократное восстановление режима/targets.
Guard передаётся явно на время синхронного `submit`, не хранится в очереди и не
вызывается для fan commands. Он проверяет исходный sample age до/после read-only
I/O, epoch и live GPU; свежий snapshot не обновляет часы исходной команды.
Любой отказ прекращает дальнейшие записи и отзывает authority. Нет postwrite
delay, повтора всей fan transaction или дополнительного фонового опроса.
Это согласованное отличие от pinned source; физически подтверждены пока только
диагностические endpoints, а не sleep/resume или все промежуточные уровни.

Порядок должен быть зафиксирован до реализации и проверен offline:

1. проверить owner/lock, identity, sleep state и typed provenance;
2. проверить bounds logical duty/keylight и получить только разрешённый pinned
   protocol packet;
3. сериализованно выполнить одну ACPI transaction;
4. получить требуемый readback;
5. только после полного успеха обновить broker state и provenance fence;
6. при исключении сохранить authority revoked и неизвестное состояние, без
   самовольной аварийной записи или retry.

Если операция подсветки требует временного firmware mode transition или
settling delay, это должно быть отдельной state machine с bounded deadline,
а не скрытым `sleep()` внутри произвольной записи. Параметры из upstream
diagnostics (включая порядок mode/lighting и 20 ms pre-write delay) должны быть
подтверждены fake protocol tests и отдельной hardware validation перед
включением writer-а.

## Обязательные gates до реализации

Следующий writer milestone нельзя начинать с live service. Offline facts,
decision evaluator и fake transport уже добавлены; оставшиеся gates:

1. target-specific validation of lock path, stale-file policy and lifecycle
   integration; offline contention/release/symlink/mode/missing-file behavior
   is already covered without creating a lock;
2. production pinned identity source implementation and physical mismatch tests
   for DMI, BIOS, EC ID, DSDT, WMI and transport path; generic DMI/endpoint
   metadata нельзя считать EC identity;
3. live broker provenance/session/generation IPC and queue integration, включая
   restart, sleep races and descriptor-loss handling;
4. service sandbox/Polkit/IPC tests с bounded requests и private endpoint;
5. physical N14 validation of the pinned ACPI protocol, firmware mode changes,
   fan response and keylight response;
6. только затем — отдельный human-reviewed plan для read-only preflight on the
   target machine. Установка, остановка существующих cooling services и любые
   hardware writes остаются отдельным явно подтверждённым этапом.

Для подготовки этого этапа `n14-control --ownership-preflight` может вывести
read-only JSON-отчёт. Он намеренно не приобретает lock, не создаёт runtime-файл,
не открывает `/proc/acpi/call` или debugfs transport и не использует generic DMI
`ec_firmware_release` как EC identity. Код выхода `2` означает ожидаемый
fail-closed deny отчёта, а не разрешение на запуск writer-а.
