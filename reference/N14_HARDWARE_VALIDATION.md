# COLORFUL N14 24 — hardware validation checklist

## Актуализация 2026-10-31

Исторические Phase A/B и evidence 2026-10-30 ниже описывают прежнее окно
controlled validation, а не актуальное состояние установки.
Позднее выполнен bounded native application test: CPU tach `2900 → 4400` nominal
RPM между `40% → 100%`, 12/12 target/keylight readbacks, восстановлены targets
`42/42`, keylight raw `0`, mode `2`. Receipt — `../VALIDATION.md` и
`../build/hardware-integration.json`. Это не проверка GPU RPM, физической яркости,
длительной нагрузки или sleep/resume.

Новый service-managed EC protocol 2 удаляет публичные Acquire/Release methods.
После отдельного разрешения пользователя сервис и D-Bus policy обновлены вместе;
подтверждены совпадение binaries и `ec_protocol=2`, отсутствие удалённых методов
в introspection. Отдельный live test завершился с `ok=true`, `errors=[]`:
CPU tach `2900 → 4500` nominal RPM между `40% → 100%`, 12/12 raw readbacks,
точно восстановлены targets `42/42`, keylight raw `0`, mode `2`. Было 6 bounded
snapshot retries только в opt-in harness; нормальный control loop не ретраит.
Новый receipt — `../build/hardware-integration-ec-protocol2.json`, подробности и
deployment evidence — в `../VALIDATION.md`; прежний receipt сохранён отдельно.
Это не проверка длительной нагрузки, sleep/resume, restart с активным owner,
GPU RPM или физической яркости. Live harness требует точного restore
mode/targets/keylight; начальный automatic mode отвергается до fan writes,
потому что disconnect освобождает lock, но не возвращает firmware mode.
Позднее отдельно пройден один clean active-owner restart: hardware PID
`32796 → 49626`, sampler PID `32782` не изменился, тот же клиент дождался нового
unique owner и явно запросил fresh provenance после 1500 ms без EC authority.
Targets `42/42`, mode `2`, keylight raw `0` не изменились; D-Bus audit содержит
один ожидаемый Apply до restart и ноль после, retries/recovery writes — `0`.
Receipt — `../build/hardware-lifecycle-ec-protocol2-run2.json`, audit и независимые
assertions — соответствующие `.audit.log` / `.verification.json`. Это не trace
каждого ACPI packet, не sleep/load test и не in-flight/partial-write restart.
Последующая bounded thermal/load попытка остановилась на EC transaction failure;
её receipt и отдельное exact-restore свидетельство описаны в `../VALIDATION.md`.
Первая неудачная audit setup попытка сохранена отдельно; она не запускала probe,
не писала EC и не перезапускала сервис.
Offline follow-up добавил различимую диагностику queue failure в development
service: этап, причина, попытки записи, raw readback и возраст provenance.
38 fake-transport и 5 private-bus cases довели suite до 290 проверок без ошибок
или пропусков. Новый service binary не установлен; новых ApplyEc/load/sleep
операций и restart не было. Read-only service observation подтвердила прежние
targets `42/42`, mode `2`, keylight `0`; физическая причина отказа пока неизвестна.
Подробности и evidence paths — в
`/home/yaroslav/.cline/data/workspaces/chat/n14-control/VALIDATION.md`.
Любые следующие live writes/service updates требуют отдельного явного запроса.

## Исторический checklist и evidence — 2026-10-30

Статус этого документа: **ограниченный diagnostic probe выполнен 2026-10-30;
полная hardware validation и production identity validation не завершены**.

Документ фиксирует следующий этап после offline ownership/preflight и fake
transaction tests. Он не является разрешением на запись. Выполненные ниже
ограниченный probe и bounded write/readback subset были запущены только после
отдельного явного подтверждения controlled handover; для любого нового окна
требуется новое явное подтверждение. Production writer, uncontrolled curves,
broker и systemd installation остаются запрещены.

## Инварианты безопасности

- Вне явно записанного controlled handover существующий cooling stack остаётся
  запущенным и не изменяется. В выполненном окне пять unit-ов были остановлены
  штатным `systemctl stop`; автоматический restart и writer не запускались.
- Не создавались `/run/colorful-n14-control`, `ec-owner.lock`, systemd units,
  debugfs handles или device handles. Probe и bounded adapter использовали
  существующий runtime directory; кроме `permission`/`service.json` были созданы
  root-private `keylight`/provenance artifacts, перечисленные в evidence.
- Phase A не приобретает lock и не объявляет ownership. Diagnostic probe держал
  только уже существующий `session.lock` на время своих readback calls и не
  создавал broker-owned `ec-owner.lock`.
- Generic DMI `ec_firmware_release` не принимается за EC identity.
- При любом несоответствии модель/BIOS/DSDT/WMI/процессов проверка
  останавливается с deny; обходы и автоматический handover запрещены.
- После transport error, partial write или failed readback нельзя выполнять
  recovery write или retry неизвестного состояния.

## Phase A — read-only target report

Эта фаза не меняет систему и может быть выполнена без установки сервиса:

```sh
set +e
/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/app/n14-control \
    --ownership-preflight > ownership-preflight.json
status=$?
set -e
test "$status" -eq 2
```

Код `2` ожидаем: metadata-only probe не держит broker lock, не имеет sleep
gate и не получает pinned EC identity. Проверить в JSON:

| Поле | Требование | Если не выполнено |
| --- | --- | --- |
| `mode` | `read-only` | остановить проверку |
| `hardware_writes` | `false` | остановить проверку |
| `lock_acquired` | `false` | это не должно становиться `true` в Phase A |
| `facts.identity.vendor` | `COLORFUL` | deny |
| `facts.identity.model` | `N14 24` | deny |
| `facts.identity.bios` | `2.07` | deny |
| `facts.identity.dmi_ec_firmware_release` | только записать как evidence | не использовать для allow |
| `facts.identity.ec_version` | пусто без production identity source | не подставлять DMI |
| `facts.identity.controller_id` | пусто без production identity source | не подставлять `55:71` вручную |
| `facts.identity.dsdt_sha256` | pinned hash из policy | deny при mismatch |
| `facts.lock_file.query_accessible` | `true` для полного metadata report | отдельно отличать permission denied от missing |
| `facts.identity.wmi_guid` | pinned WMI GUID | deny при mismatch |
| `facts.competing_owners` | пустой список | deny, не останавливать процесс автоматически |
| `facts.acpi_endpoint_present` | только endpoint evidence | не считать transport verified |
| `facts.read_only_transport_verified` | `false` у текущего generic probe | не превращать в `true` вручную |

Если `query_accessible=false` или `dsdt_readable=false`, это означает
`permission denied`/недоступность чтения, а не подтверждённое отсутствие объекта.
Не исправлять это созданием файлов или изменением permissions; при необходимости
получить только read-only evidence отдельный оператор может запустить тот же
probe с заранее согласованным read-only privilege context.

Сохранить JSON и hostname/kernel/BIOS evidence вместе, но не редактировать
facts для получения allow. Текущий `--ownership-preflight` специально не
проверяет EC controller `55:71` и ACPI method через live transport: для этого
нужен отдельный production pinned identity source, который нельзя безопасно
закончить без target-specific validation.

## Ограниченный diagnostic probe — выполнен 2026-10-30

После read-only evidence и явного разрешения был остановлен только
подтверждённый ownership stack:

- `colorful-n14-profile-selector.service`;
- `cc-plugin-custom-device.service`;
- `coolercontrold.service`;
- `colorful-n14-adapter.service`;
- `colorful-n14-sensors.service`.

`colorful-n14-nbfc-prepare.service` остался `active (exited)`,
`nbfc_service.service` был и остался inactive. PID `857` больше не удерживал
`/run/colorful-n14-control/session.lock`; lock-файл не удалялся.

Запущена ровно команда:

```sh
cd /home/yaroslav/.cline/data/workspaces/chat/colorful-n14-coolercontrol
pkexec /usr/bin/python3 /home/yaroslav/.cline/data/workspaces/chat/colorful-n14-coolercontrol/prepare.py \
    probe --acknowledge-risk
```

Без `--launch` NBFC/writer не запускался. Diagnostic indexed reads через
`\\_SB.AMW0.WMBA` вернули:

| Read | Result |
| --- | --- |
| `0x02E:0x20` | `0x55` |
| `0x02E:0x21` | `0x71` |
| `0x302:0x5C` | `0x23` = 3500 RPM nominal |
| `0x302:0x55` | `0x24` = command 36/70 = 51.43% |

GPU physical RPM не подтверждался. Не выполнялись writes к mode `0x50`, fan
targets `0x54`/`0x55` или keylight `0x5B`; read `0x302:0x55` выше является
только diagnostic command readback. Probe создал permission artifact с режимом
`read`, а не `control`.

После probe все пять unit-ов оставлены `inactive`, matching processes и
`session.lock` holders отсутствуют. Повторный metadata-only ownership preflight
вернул ожидаемый exit `2`, `facts.competing_owners` пуст, `hardware_writes` —
`false`, `lock_acquired` — `false`. `prepare.py check` вернул exit `0`.

Evidence:

- `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/pre-handover-read-only-20261030T193810Z.txt`;
- `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/controlled-handover-stop-20261030T193810Z.log`;
- `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/diagnostic-probe-20261030T193907Z.log`;
- `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/post-probe-evidence-20261030T193907Z.txt`;
- `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/post-probe-read-only-preflight-20261030T193907Z.json`;
- `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/diagnostic-probe-system-info-20261030T193907Z.txt`.

Полученный `55:71` является diagnostic readback, а не новым production pinned
identity source. Поэтому policy/source и production broker не обновлялись.

## Bounded write/readback subset — выполнен 2026-10-30

После отдельного `prepare.py arm` с подтверждёнными sensors/EC ID был выполнен
только bounded transport test. NBFC, CoolerControl daemon, plugin и selector не
запускались; adapter был запущен отдельно как serial owner и остановлен после
теста.

Fan target sequence:

| Step | Mode | CPU/GPU targets | CPU tach raw | Temperatures |
| --- | ---: | ---: | ---: | --- |
| Before | `2` | `36/36` | `35` | CPU `49°C`, GPU `40°C` |
| 40% command | `2` | `28/28` | `27` after 5 s | CPU `49°C`, GPU `40°C` |
| Full command | `2` | `70/70` | `44` after 5 s | CPU `49°C`, GPU `40°C` |
| Restored | `2` | `36/36` | `44` | readback matched |

Это подтверждает register write/readback и CPU tach response, но не заменяет
длительное thermal/load validation или независимое физическое наблюдение обоих
вентиляторов. Raw `0` намеренно не использовался.

Keyboard-light transaction:

- исходный register `0x5B`: raw `0` / `100%`;
- test transaction: raw `88` / `50%`, readback `50%`;
- exact original raw `0` восстановлен, readback `100%`;
- fan targets при transaction оставались `36/36`.

При возврате fan targets adapter обнаружил отсутствие authenticated producer/
provenance assignment и fail-closed оставил firmware в automatic mode `0`, не
делая recovery write. Journal зафиксировал `stale/invalid fan targets; no fan
override` и `handover expired or invalid; invalidating control without fan
override`. Текущее post-test raw state: mode `0`, targets `36/36`, keylight raw
`0`; permission после post-write probe возвращён в режим `read`.

Readback register не является физическим измерением яркости или гарантией
вращения GPU fan. Physical keyboard-light observation, authenticated producer
assignment, broker-owned lock, sleep/resume lifecycle и production identity
source всё ещё не подтверждены.

Write-test evidence:

- `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/controlled-protocol-thermal-gate-20261030T195239Z.json`;
- `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/controlled-protocol-arm-20261030T195312Z.log`;
- `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/controlled-protocol-fan-test-20261030T195345Z.log`;
- `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/controlled-protocol-keylight-test-20261030T195549Z.log`;
- `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/controlled-protocol-keylight-restore-20261030T195633Z.log`;
- `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/controlled-protocol-keylight-partial-state-20261030T195722Z.log`;
- `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/controlled-protocol-postwrite-preflight-20261030T195901Z.json`;
- `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/controlled-protocol-final-state-20261030T195901Z.json`.

## Phase B — full controlled protocol validation (не запускать автоматически)

Полная фаза допустима только после отдельного подтверждения владельца системы,
подтверждения окна наблюдения и плана возврата к действующему стеку. Перед
началом должны быть зафиксированы:

1. явное разрешение на hardware writes;
2. явное разрешение на controlled shutdown/handover существующих cooling
   services, если он вообще нужен;
3. оператор, окно наблюдения и способ немедленно прекратить эксперимент;
4. новый ownership proof, а не только результат Phase A;
5. отсутствие параллельного ACPI/EC writer-а.

Порядок проверок должен быть журналируемым и останавливаться на первом
несоответствии:

1. подтвердить EC identity `55:71`, BIOS/EC contract, pinned DSDT и
   `\\_SB.AMW0.WMBA` отдельным reviewed source;
2. подтвердить packet format и transport index `0x302` на read-only/controlled
   diagnostic fixture;
3. подтвердить mode register `0x50`, fan targets `0x54`/`0x55` и keylight
   register `0x5B` с обязательным readback;
4. подтвердить keylight transition order и 20 ms settle callback;
5. наблюдать фактическую реакцию CPU/GPU fans и keyboard lighting, не выдавая
   fake transport result за physical response;
6. проверить sleep preparation/resume, broker restart, lock loss, IPC disconnect,
   stale/replayed provenance и partial failure;
7. после каждого отказа подтвердить terminal revoke и отсутствие recovery write.

До завершения этой фазы нельзя реализовывать production writer, live broker,
automatic handover или systemd installation.

## Evidence record

Минимальная запись должна содержать:

- timestamp, hostname, kernel, BIOS и текущие cooling processes;
- неизменённый JSON Phase A;
- подтверждённый EC identity source и DSDT/WMI hashes;
- точные transport packets, readbacks и порядок событий;
- наблюдаемые fan/light results отдельно от transport results;
- sleep/resume/restart/partial-failure outcomes;
- explicit statement, что не было незапланированных retries, recovery writes или
  systemd changes; approved service stop/handover и его state transitions должны
  быть записаны отдельно.

Текущий статус: `bounded-protocol-write-readback-complete /
production-hardware-unvalidated`.
До завершения полной фазы `evaluateEcOwnership()` должен оставаться
fail-closed.