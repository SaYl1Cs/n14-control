# Строгий перенос опубликованного поведения

Требование пользователя: переносить существующий функционал **строго как в
исходниках GitHub**, без самовольного изменения алгоритмов или политики отказа.

## Эталон

- Репозиторий: https://github.com/SaYl1Cs/colorful-n14-coolercontrol
- Опубликованный `main`, проверенный через `git ls-remote`:
  `4967a3e73105014fb640150587e538c6ca594941`.
- Git tree: `4bd22fa3364f9ec51e0e0d078bc6a5acbfb0998c`.
- Локальный дополнительный коммит `df9155abd687cf7bc275d0bd73cb2c967a52bc5e`
  с sleep-gate изменениями **не входит в опубликованный эталон**.
- Точные внешние ревизии CoolerControl и custom-device взяты из опубликованного
  `BUILDING.md` и зафиксированы вместе с патчами в `source-lock.json`.

Эталон — закреплённый commit, а не изменяемая ветка и не текущее установленное
состояние ноутбука. Новую версию эталона нельзя выбирать молча. Уже работающий
локальный стек при фиксации эталона не откатывается и не останавливается.

## Что сохраняется без переосмысления

- Проверки модели, BIOS/EC, контроллера и DSDT, форматы команд и данных.
- Кривые, округление, нормализация и Standard/Functions из закреплённых исходников
  producer; независимая симуляция Identity/Hysteresis/MovingAverage не замена.
- CPU-only при подтверждённом сне GPU, Max при активной GPU, ошибки/отсутствующие
  температуры, clocks, freshness, поколения и отбрасывание устаревших команд.
- Точные временные окна hold/переходов и исходная семантика сна/возобновления.
- Сериализация подсветки и вентиляторов, исходные задержки, восстановление
  отдельных targets только при исходных разрешающих условиях.
- Опубликованная политика из `NO-EMERGENCY-OVERRIDE.md`: отказ отзывает полномочия,
  но не подставляет автоматические 100%, 50%, 5% или 0%. Нормальная кривая вправе
  запросить 100%. Не добавлять собственные аварийные пороги, retries или recovery writes.

**Исходный риск сохраняется:** последняя команда, включая остановленные
вентиляторы, может оставаться неограниченно долго. После частичной EC-записи
состояние может быть разным/неизвестным. Это не гарантия тепловой безопасности.

### Явно согласованное отличие подсветки (2026-10-31)

После отрицательных физических опытов с секундой до записи и двумя записями
подряд пользователь разрешил предварительную запись нового light byte **до**
входа в auto. Отдельный опыт `0 -> 100 -> 0` физически прошёл; пользователь затем
явно разрешил перенос в обычный broker. Это дополнение к закреплённому порядку,
а не утверждение о полном соответствии исходнику или доказанном устройстве EC.
Порядок: cache mode/targets -> preload light (если manual) -> auto -> 20 ms ->
verify auto -> main light write -> один restore mode/CPU/GPU -> verify -> light read.
Проверки ORIGINAL Apply clocks (<2 s), epoch и live GPU выполняются перед preload,
main write и restore; callbacks request-local, без diagnostic globals. Ошибка
терминальна, никаких retries/recovery или postwrite delay. Подробнее и ограничения
физической проверки: `../VALIDATION.md`.

Новые функции питания/CPU/экрана, которых нет в этом репозитории, остаются
отдельными дополнениями N14 Control, а не выдаются за перенос из оригинала.

## Проверка и критерий готовности

## Текущий native-срез

`src/reference_control.{h,cpp}` переносит только чистые, не привилегированные
алгоритмы первого этапа:

| Компонент | Состояние | Ограничение |
| --- | --- | --- |
| `UpThreshold/DownThreshold/FanSpeed` schema | перенесён | duplicate-key guard действует на raw JSON boundaries; QJsonValue API сам по себе не является text loader |
| NBFC row selector | перенесён | используется только после role-specific validation |
| `producer normalize/interpolate` | перенесён | live Graph API sync не подключён; native defaults используются явно |
| CPU-only/suspended-GPU demand | перенесён | live output разрешён только после native sampler/provenance/power-mode gates |
| thermal snapshot/paired samples | перенесён | native `n14-sensors-service` публикует read-only snapshot; physical sensor access ещё не валидирован |
| fan/keylight integer conversions | перенесён | bounded CPU tach response и 12/12 raw readbacks подтверждены отдельно для предыдущего API и EC protocol 2; physical keylight/GPU RPM ещё не проверены |
| CoolerControl Graph points | перенесён в native defaults | текущие профили прочитаны read-only; live API sync ещё не подключён |
| `Standard/Functions` | перенесён в `reference_standard.{h,cpp}` | state machine подключена к opt-in GUI curve loop; live output остаётся provenance-gated |

Для Standard добавлен hash-locked oracle из CoolerControl `5.0.1` и
`43` сравнительных сценария (`1980` циклов), включая текущие CPU/GPU функции,
изменение delay/poll rate, startup history, latch, `only_downward`, ramp
continuation, zero/100 boundaries и failsafe normalization. Это подтверждает
чистый алгоритм на одинаковых offline-входах. Runtime freshness/clock/epoch,
duplicate JSON, paired samples, native producer lifecycle и live power-mode
recheck теперь проверяются отдельными native boundaries. Физическая реакция
подсветки/GPU и устойчивость после сна всё ещё требуют target-specific validation;
bounded CPU tach test не заменяет длительную нагрузку. Новый EC protocol 2
установлен и имеет отдельный live receipt (`2900 → 4500` nominal RPM, 12/12
readbacks, точный restore); предыдущий live receipt к нему не относится.

### Native IPC lifecycle

`AcquireEc`/`ReleaseEc` удалены из публичного API. `GetEcProvenance` запускает
service-side authorization/preflight автоматически, `ApplyEc` по-прежнему
принимает только совпадающую validated provenance. GUI не держит lease и не
открывает EC. Sleep/disconnect/failure revoke остаются внутри сервиса, без
recovery write или replay старой команды. Это изменение IPC boundary, а не
новая firmware/failure policy. Отдельные bounded fan/tach и clean active-owner
restart receipts protocol 2 находятся в `VALIDATION.md`; полного hardware parity
pass, physical sleep/resume или in-flight restart validation они не означают.
Broker generation process-local; новый unique D-Bus owner сбрасывает клиентские
generation/epoch/Standard state даже при меньшем счётчике replacement-а.
Клиент также сбрасывает локальное состояние на `PrepareForSleep(true)` и
отбрасывает поздние ответы прежней session revision. Это не владение EC в GUI:
сервис по-прежнему единолично управляет lock/transport. Физическая bounded-load
попытка завершилась transaction rejection и отдельным успешным explicit restore;
причина отказа пока неизвестна, thermal parity pass не объявляется.

## Read-only local snapshot provider
`RuntimeSnapshotProvider` использует контракт native producer-а
`/run/colorful-n14-control/temperatures.json`. Producer находится в
`service/sensor_sampler.{h,cpp}` и запускается отдельным `n14-sensors.service`;
broker сам snapshot не создаёт и не изменяет.
Для каждого чтения provider открывает все directory components от `/`
через `O_DIRECTORY|O_NOFOLLOW`, а файл — через `openat(...|O_NOFOLLOW)`, затем
проверяет `fstat`, regular-file type, root UID/GID, private mode (без group/other
доступа и special bits) и лимит `16384` байт. Абсолютные пути с `.`/`..` отклоняются. Чтение
идёт с уже открытого descriptor-а; поэтому producer, публикующий через temp-file
и `rename`, даёт consumer-у либо старый полный файл, либо новый полный файл.
Provider ничего не пишет, не переименовывает, не блокирует и не читает clocks.

Тесты дополнительно покрывают исчезнувший файл, symlink, traversal, non-regular
file, owner/mode rejection, oversize, truncated JSON, atomic replacement и replay
после replacement. Отдельные loop-тесты проверяют timeout fail-closed, stop/start
reset, timer polling и принятие нового producer session. Тестовая policy
отключает только root UID/GID identity checks для `QTemporaryDir`; production
default остаётся root-owned/private.

`scripts/test-reference.py` читает объекты **закреплённого** коммита из локальной
Git-базы, проверяет tree и blob IDs, извлекает неизменённые файлы в отдельный
временный каталог `build/` и запускает выбранные исходные offline-тесты с
подменённым аппаратным доступом. Рабочее дерево исходного проекта не используется.
Установщики и сервисы эталона не запускаются. Python нужен только для разработки.

```sh
python3 -B /home/yaroslav/.cline/data/workspaces/chat/n14-control/scripts/test-reference.py
```

Отчёт с SHA-256 каждого файла: `build/reference-verification.json`. Для проверки
другой локальной Git-базы используйте `--repository` с её абсолютным путём.
`--verify-only` проверяет файлы, **не** выдаёт результат за прошедшие тесты.
Вне этого workspace сначала нужно самостоятельно получить закреплённый репозиторий;
скрипт не скачивает код и не переключает ветки.

Прошедшие тесты исходника доказывают воспроизводимость **эталона**, а golden-
сравнение — только соответствие чистого native Standard алгоритма. Каждый
следующий модуль должен дополнительно проходить проверку отказов, freshness,
таймеров и последовательности аппаратных операций. До этого N14 Control не
получает владение EC, а полная runtime-совместимость не заявляется. Preview-
фильтры GUI не являются альтернативой Standard.