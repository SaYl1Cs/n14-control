# N14 Control — 0.2.1

## Подсветка и общий EC-контур — совместная проверка

Регулировка подсветки использует подтверждённое `valueChanged`, включая мышь,
клавиатуру и колесо. Быстрые изменения объединяются одним single-shot на 120 мс;
в контроллере хранится только последнее значение, а не растущая очередь.
EC-операции остаются асинхронными для GUI, без дополнительного фонового опроса.
`run.sh` предпочитает свежую сборку, а не старую версию из `dist`.

## Установка GUI и ярлыка

Release-бандл собирается командой `./scripts/bundle.sh` и появляется в
`dist/n14-control-0.2.1-linux-x86_64.tar.gz`. После распаковки его можно
установить одной командой `sudo ./install.sh`; она устанавливает GUI,
иконку и ярлык в меню приложений. Для системной установки вручную GUI,
иконки и ярлыка в меню приложений выполните от root:

```sh
sudo ./scripts/install-app.sh
```

Установщик размещает приложение в `/usr/local/bin/n14-control`, desktop-файл в
`/usr/share/applications/n14-control.desktop`, а иконку в стандартном каталоге
hicolor. Для установки только текущему пользователю без root:

```sh
./scripts/install-app.sh --user
```

Этот GUI-установщик не перезапускает hardware services и не выполняет EC-записи;
привилегированный стек устанавливается отдельно проверенными service-скриптами.
Установленный GUI использует системные Qt 6 libraries/plugins; для установки
нужен `desktop-file-validate` из `desktop-file-utils`. Переносимый запуск с
библиотеками из архива выполняется через `AppRun`. Архив предназначен для
целевой CachyOS/Arch-системы, универсальная совместимость Linux не заявляется.

**Физическая подсветка: найден успешный порядок (2026-10-31),
установлен в обычном broker и подтверждён из GUI.**
Пользователь подтвердил задержку на одну команду даже при правильном raw-readback:
после `0 → 100 → 0` клавиатура оставалась яркой, а один диагностический повтор
тех же `0` её погасил. Исправления GUI и точный порядок EC-обращений сами по себе
этого не устранили. Отдельные согласованные опыты с секундой **до** записи и с
двумя записями подсветки подряд в одной транзакции тоже не помогли: после конечных
`0%` клавиатура оставалась яркой. Соседняя двойная запись не внедрена как обход,
пауза после записи не добавлена.
Следующий отдельный опыт с предварительной записью нового значения **до входа
в auto**, затем обычными `20 мс → запись → восстановление вентиляторов`, прошёл
`0 → 100 → 0`: пользователь подтвердил включение и конечное полное выключение.
После согласованного переноса пользователь также проверил ползунок и промежуточные
уровни: «Всё прекрасно работает». Physical keylight sleep/resume ещё не проверен.
Обычный broker сохраняет одного EC writer-а,
одну паузу 20 мс и один restore вентиляторов; проверки исходной свежести/эпохи/GPU
передаются явно на время транзакции, без диагностических globals и новых таймеров.
Полный прогон этой сборки: 338 Qt + 10 Python проверок, без ошибок и пропусков.
См. новые наблюдения в `VALIDATION.md`; прежний 60-секундный pass не доказывает
правильное физическое применение каждой яркости.

### CPU-предел в пресетах

«Снять дополнительный предел (максимум CPU)» возвращает каждой cpufreq-policy
её аппаратный максимум; это не фиксированная частота, не разгон и не включение
Turbo. `2400 МГц` в режиме ограничения — потолок, а не требуемая текущая частота.
Turbo управляется отдельно. Полные автоматические пресеты требуют разрешённого
CPU-действия: по умолчанию Polkit запрашивает администратора для каждого запроса.
Для явно разрешённой пользователем автоматики есть **опциональный**, не включённый
по умолчанию пример `service/49-n14-cpu-automation-yaroslav.rules.example`:
только действие `org.n14control.modify-cpu` для активного локального `yaroslav`.
Разрешение относится к пользователю, а не к одному бинарнику; ограничения и
readback по-прежнему проверяет root-сервис. Удаление установленного одноимённого
правила из `/etc/polkit-1/rules.d/` возвращает штатное требование авторизации.

Обнаружены также общие причины отключения кривой при переходах GPU:
смена поколения брокера и terminal-обработка краткого PM-расхождения. Внедрена
совместная правка брокера/контроллера с отдельным состоянием `waiting` и исходным
5-секундным original-age ограничением. По указанию пользователя короткое ожидание
GPU-данных не отключается только из-за высокой температуры: это явно отличается
от temperature eligibility исходной hold-policy, но не разрешает EC-записи
по старым данным. Transport/readback ошибки по-прежнему terminal;
никаких recovery writes или повторов частично выполненной записи нет.
Это требует **обновления обоих процессов**, не только GUI. Полный контракт,
источники, сквозной тест и ограничения — в `reference/EC_STACK_AUDIT.md`.
Оба процесса обновлены и сверены с запущенными executable; финальный прогон:
323 Qt + 10 Python, без отказов. Совместное 60-секундное наблюдение кривой и
команды подсветки прошло; физические переходы GPU и sleep/resume в этом окне
не выполнялись.

Дополнение 2026-11-01: журнал подтвердил остановку кривой после единичного
некорректного байта температуры GPU. В allowlist ожидания добавлены этот случай
и смена GPU power state во время PM-наблюдения. Они не создают EC-команд и не
продлевают исходное окно менее 5 секунд; после свежего валидного измерения
контроллер рассчитывает кривую заново. Ошибки транспорта, CPU и постоянная
потеря данных остаются явным отказом, а не скрытым бесконечным recovery.
Регрессии прошли. Последующий аудит релиза подтвердил совпадение установленных
service-бинарников с исправленной сборкой и работающую кривую; см. актуальную
запись в `VALIDATION.md`. GUI-установщик сам эти службы не обновляет.

## Фоновое управление кривой — обновление 2026-10-31

`n14-fan-controller` исполняет сохранённые CPU/GPU Graph + Standard/Functions
каждую секунду независимо от окна. Пассивная GUI-телеметрия остаётся на 5 секундах.
Контроллер запускается отдельным системным unit-ом от пользователя `yaroslav`
до `plasmalogin`; GUI обращается к единственному фоновому клиенту через system D-Bus
`org.n14control.Fans1`; root EC broker, lock, Polkit, provenance и readback не
обходятся. Ручная скорость и подсветка идут через тот же последовательный клиент.
Сохранение кривой/Standard перечитывается автоматически; live Mix — только Max,
Min/Mean и legacy preview filters не подменяют Standard.

После сборки и при уже установленных native hardware/sensors services:

```sh
sudo /home/yaroslav/.cline/data/workspaces/chat/n14-control/scripts/install-fan-controller.sh
systemctl status n14-fan-controller.service --no-pager
```

Если старый `CoolerControl/NBFC` stack ещё включён, его можно заменить только
явно: добавьте `--replace-legacy`. Активный `nbfc_service` при этом всегда
останавливает установку; два EC writer-а не допускаются.

Это отдельное явное включение реального управления, не часть root installer.
Сервис работает как системный unit от `yaroslav`, поэтому доступен на экране
входа и получает системные sleep/resume-сигналы до открытия графической сессии.
Настройки берутся из
`/home/yaroslav/.config/N14/n14-control/config.json`, без отдельной устаревающей копии.
Не запускайте старый GUI, CoolerControl/NBFC или отдельные write-probes параллельно.
Обычные integration/lifecycle scripts отказываются запускаться при активном
headless-контроллере. Для наблюдения работающего контура есть отдельный
`/home/yaroslav/.cline/data/workspaces/chat/n14-control/scripts/fan-curve-physical-test.py`.

Ошибка provenance/transport/readback/configuration остаётся terminal: контур
показывает отказ, не повторяет запись и не восстанавливает старый target.
В обновлённом GUI есть кнопка явного включения кривой после проверки причины.
После чистого resume выполняется новая сверка lid-кэша, восстанавливается последнее
подтверждённое состояние keylight и запускается новый расчёт со свежей provenance;
ручная EC-команда не replay-ится. Физический цикл lid close → deep suspend → AC
remove while asleep → resume прошёл: curve продолжилась, а keylight восстановился.
Если сон прервал незавершённую EC-операцию, контур остаётся terminal и требует
явного перезапуска.
Закрытие процесса не возвращает firmware automatic mode: не оставляйте
остановленный контур на низком последнем duty под нагрузкой.

Физический bounded тест фонового контура прошёл без ошибок: CPU target raw
`1–53`, CPU tach nominal `400–4600 RPM`, CPU sysfs максимум `85 °C` при стопе
`90 °C`; GPU RPM неизвестны. Подробности и ограничения — в `VALIDATION.md`.
Исторические ограничения и receipts ниже сохранены как история предыдущих builds.

Второй рабочий этап единого приложения для COLORFUL N14 24 / KDE Wayland.
Нативный C++20 / Qt 6 Widgets, без Electron, WebView, Node и Python во время работы.

## Границы этой версии

**Полная замена CoolerControl ещё не валидирована.** На целевом ноутбуке выполнен
bounded application-level тест: CPU tach увеличился между 40% и 100%, targets и
подсветка восстановлены. Отдельный thermal/load gate был запущен с жёстким
лимитом, но остановился на EC transaction failure и поэтому не считается pass;
sleep/resume, GPU RPM и физическая яркость подсветки также не подтверждены.
Root CPU/EC broker устанавливается отдельно; GUI не открывает EC напрямую.
Текущий EC protocol 2 с service-managed lifecycle установлен и прошёл отдельный
bounded live test: CPU tach `2900 → 4500` nominal RPM, 12/12 raw readbacks,
точный restore targets/keylight/mode; receipt и ограничения — в `VALIDATION.md`.
Отдельно пройден один restart с активным EC-клиентом: состояние не изменилось,
D-Bus audit подтвердил отсутствие автоматического Apply после restart.
Существующее охлаждение не останавливается автоматически.

Реализовано:

- пассивная телеметрия аккумулятора каждые 5 секунд: мощность, заряд и источник питания;
- CPU package temperature из `coretemp`/`k10temp`, без чтения GPU hwmon/NVML;
- NVIDIA PCI runtime status, D3cold и суммарное время suspended;
- график разряда, среднее за 60 секунд, экспорт до 720 отсчётов в CSV;
- ручные power-saver / balanced / performance через системный D-Bus;
- отдельные автоматические правила профиля и частоты от сети/батареи;
- отдельный профиль при низком заряде;
- переключение 60/165 Гц встроенной eDP-панели, без изменения разрешения;
- проверка фактического режима после переключения;
- трей, фоновый запуск, открытие существующего экземпляра;
- редактор CPU/GPU-кривых с графиком и проверкой значений;
- симулятор Mix Max/Min/Mean и независимых фильтров Identity/Hysteresis/MovingAverage;
- атомарное сохранение настроек; автоматический профиль питания по умолчанию включён
  и выбирается приложением по AC/батарее (его можно отключить в UI).
- до 16 пользовательских пресетов: имя, системный профиль, 60/165 Гц, предел CPU,
  запрет/разрешение Turbo Boost; копирование, удаление и независимое «не изменять»;
- назначение пресетов для сети, аккумулятора и низкого заряда; простые правила
  и составные пресеты взаимоисключающие;
- CPU-сервис с Polkit, проверкой всех policies перед записью, ограничением штатным
  максимумом P/E-ядер, чтением результата и явным сообщением о неполном откате;
- N14 EC broker с active-session Polkit policy без интерактивного подтверждения,
  root-owned lock lease, pinned DMI/BIOS/SMBIOS EC/controller/
  DSDT/WMI identity, fixed ACPI transport, serialized fan/keylight queue,
  readback и fail-closed revoke;
- manual fan slider с явным checkbox ручного режима, keylight slider и
  Standard/Functions curve loop; EC lifecycle принадлежит сервису, GUI отправляет
  только provenance/command requests без acquire/release и ownership-кнопок;
- GUI показывает CPU tachometer readback в RPM и fan target readback. GPU RPM
  этим EC-протоколом не подтверждён и намеренно отображается как неизвестный;
- пауза правил на logind PrepareForSleep и повторное подтверждение питания после
  возобновления; CPU-сервис отклоняет новые записи во время подготовки ко сну;
- схема настроек 3 с native CoolerControl contract, чтением схем 1/2 и приватной
  `.schema1.bak` при первом сохранении legacy-конфигурации.

Внутри исходников добавлен изолированный native reference-срез
`src/reference_control.{h,cpp}`: строгая схема `UpThreshold/DownThreshold/FanSpeed`,
stateful NBFC-переходы, нормализация/interpolation producer-а, GPU sleep semantics
и точные EC/keylight integer conversions. `src/reference_standard.{h,cpp}` теперь
переносит state machine CoolerControl `Standard/Functions`: initial history,
response delay, inclusive deviance, threshold hopping, `only_downward`, asymmetric
step limits, extreme bypass, continuation of clamped ramps, safety latch и
сохранение состояния. `src/reference_thermal.{h,cpp}` добавляет проверку paired
sampler metadata, freshness/clock, `active/D0` и `suspended/D3cold`.
Standard-поля уже подключены к отдельному settings/UI-контракту. Добавлен
read-only `reference_runtime.{h,cpp}` gate: он проверяет freshness, paired
metadata, session/generation/sample ordering и выдаёт typed `ThermalInput` для
Graph/Standard. Добавлен отдельный read-only `RuntimeSnapshotProvider`: по
явному вызову он читает `/run/colorful-n14-control/temperatures.json` только
через `O_NOFOLLOW`/descriptor checks, с проверками root UID/GID, private
`RuntimeThermalSamplerLoop` добавляет отдельный QTimer loop с injected clocks,
timeout и restart semantics. Root service дополнительно использует тот же
validated snapshot gate перед каждой EC transaction; GUI получает provenance
только через D-Bus, не читая private runtime file.

`reference_ownership.{h,cpp}` теперь также содержит отдельный read-only
`EcReadOnlyProbe`: injected filesystem/process interfaces и их POSIX adapters
собирают metadata runtime directory/lock, DMI, DSDT, WMI и `/proc` process
names. Интерфейсы не имеют операций создания, записи, `flock` или device-open.
Probe не объявляет lock held, не подтверждает sleep state, EC controller ID или
ACPI transport; при отсутствии отдельного проверенного owner/identity gate
`evaluateEcOwnership()` остаётся fail-closed. Реальный lock acquisition,
identity source, RAII lease и `EcBroker` теперь реализованы и покрыты fake/service
tests. Отдельные bounded live broker/write/tach receipts предыдущего API и
EC protocol 2 описаны в `VALIDATION.md`; полный production lifecycle всё ещё
требует отдельных живых проверок.
Generic DMI `ec_firmware_release` выводится только как диагностическое поле и
не заполняет authoritative EC identity.

`service/sensor_sampler.{h,cpp}` добавляет отдельный root read-only producer:
он читает только EC offsets `0x56/0x58`, дважды подтверждает PCI
`active/D0` или `suspended/D3cold`, публикует paired snapshot атомарным rename
и удерживает собственный `sensors.lock`. `reference_power.{h,cpp}` повторно
проверяет GPU power mode непосредственно перед provenance выдачей и EC write;
неподтверждённый или изменившийся mode отзывает authority.

`reference_transaction.{h,cpp}` добавляет serialized queue для
provenance-bound fan/keylight commands: pinned register order `0x50/0x54/0x55/0x5b`,
bounded logical conversions, sample/session/generation freshness fences,
readback checks и terminal revoke без recovery writes. `FakeAcpiEcTransport` покрывает
offline behavior; `PosixEcRegisterTransport` доступен только root broker-у и не
принимает arbitrary register/path/method.

Переход к физической проверке описан отдельно в
[`reference/N14_HARDWARE_VALIDATION.md`](reference/N14_HARDWARE_VALIDATION.md).
Это checklist, а не разрешение на hardware writes: Phase A read-only, Phase B
controlled protocol validation и остановка существующих cooling services остаются
раздельными этапами.

Семантика preview-фильтров документирована ниже. Они остаются отдельным GUI
симулятором и **не заменяют** перенесённый Standard reference, runtime sampler
или аппаратный протокол.

## Запуск

Из исходников на этой системе:

```sh
/home/yaroslav/.cline/data/workspaces/chat/n14-control/scripts/build.sh
/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/app/n14-control
# Native read-only sampler (ставится отдельно systemd unit-ом):
/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/sensors/n14-sensors-service
```

Собранный переносимый архив создаётся `scripts/bundle.sh`; распаковать его можно
в произвольный пользовательский каталог и запустить `AppRun`. Qt и плагины входят
в архив. Полностью статическая сборка не заявляется.

```sh
# Только чтение телеметрии; дисплей и права root не нужны:
./build/app/n14-control --probe
# Только чтение D-Bus / KScreen:
./build/app/n14-control --probe-desktop
# Read-only ownership/identity отчёт; ожидаемый deny не означает ошибку probe.
# Команда не создаёт lock, не вызывает flock и не открывает ACPI transport.
./build/app/n14-control --ownership-preflight > ownership-preflight.json
# Демо с временной конфигурацией, без применения аппаратных настроек:
./build/app/n14-control --demo
# Запуск без окна, с иконкой в трее:
./build/app/n14-control --background
```

Закрытие окна при наличии трея скрывает его; автоматика продолжает работать.
Для выхода выберите «Завершить» в меню трея. Выход не восстанавливает предыдущий
профиль/частоту/пределы CPU: остаётся последнее выбранное состояние. Без трея фоновый запуск
отклоняется, а закрытие окна завершает приложение. Запускать GUI через sudo нельзя.

## Системные интерфейсы, не поставляемые приложением

- Linux sysfs и пользовательская сессия KDE Wayland;
- действующий `power-profiles-daemon` и системная политика D-Bus/Polkit;
- `/usr/bin/kscreen-doctor`, входящий в установленный KDE;
- совместимая базовая система: glibc, графический стек, D-Bus.

На целевом ноутбуке эти интерфейсы уже обнаружены. Переносимый архив устраняет
необходимость отдельно устанавливать библиотеки Qt, но не может заменить ядро,
драйверы, композитор или системные полномочия. При отказе доступа приложение
показывает ошибку, не вызывает sudo/pkexec и не пытается обходить политику системы.

## Пользовательские профили и CPU-сервис

Вкладка «Пользовательские профили» редактирует **пресеты N14**, а не внутреннее
устройство трёх режимов power-profiles-daemon. Сохранение не применяет настройки
немедленно; включённая автоматика может выбрать изменённый пресет после двух
последовательных наблюдений. Кнопка «Применить сохранённый пресет» использует
последнюю сохранённую версию и отключает обе системы автоматизации.

Начальные примеры (автоматика выключена): сеть — performance/165 Гц/максимум CPU;
аккумулятор — power-saver/60 Гц/2400 МГц; низкий заряд — power-saver/60 Гц/1600 МГц.
Это редактируемые примеры, не рекомендации оптимальных частот для всех задач.
«Не изменять» не возвращает предыдущую настройку. Например, если батарейный
пресет ограничил CPU, сетевой должен явно убрать этот предел или задать другой.
Turbo Boost настраивается отдельно; разрешение не гарантирует фактический boost.
Частотный предел ограничивает **всю частоту CPU, включая boost**, не фиксирует её
и не является отдельным аппаратным лимитом только для turbo-диапазона.

Запись CPU, fan и keylight производится только отдельным системным сервисом.
Его код и конфигурация находятся в `service/`; сборка создаёт также
`build/service/n14-hardware-service`. До установки доступны редактор/демо и
чтение состояния. EC command требует отдельного Polkit action, broker lock и
validated provenance; при недоступном сервисе аппаратная команда не выполняется.

### EC D-Bus contract: service-managed lifecycle

`Capabilities` сохраняет базовый `protocol=1` и добавляет `ec_protocol=2`.
EC-клиент отказывается работать со старым сервисом, CPU-интерфейс не меняется.
Публичные EC методы — только `GetEcProvenance()` и `ApplyEc(json)`;
`AcquireEc`/`ReleaseEc` удалены из introspection и D-Bus policy.

Первый запрос выполняет неинтерактивный Polkit check
`org.n14control.modify-ec`, identity/preflight и захват единственного broker lock.
Успешная авторизация привязана к живому unique D-Bus caller, а не к каждой команде.
Другой caller не получает состояние или право записи действующего владельца.
`GetEcProvenance` возвращает identity, поколение broker и validated snapshot;
`ApplyEc` принимает только совпадающую provenance. Прямой `ApplyEc` без предыдущего
наблюдения не создаёт фиктивный snapshot и не пишет в EC.

Disconnect, sleep, invalid command текущего владельца и transport/readback failure
отзывают broker authority без recovery writes. Новый запрос после revoke повторяет
preflight и получает новое поколение; старая команда не воспроизводится.
Освобождение lease **не** выбирает firmware automatic mode и не меняет targets.
При transient thermal observation failure новая команда не отправляется;
обычный backend ничего автоматически не ретраит. Установку/обновление сервиса
следует выполнять отдельно, при закрытом GUI.

**Явная установка на целевой машине** из исходного проекта, после сборки и
проверки файлов `service/` (GUI запускать через sudo нельзя):

```sh
# Установить стандартную Polkit policy (EC backend не открывает password dialog):
sudo /home/yaroslav/.cline/data/workspaces/chat/n14-control/scripts/install-service.sh

# Альтернатива: разрешить CPU/EC указанному активному локальному пользователю,
# в том числе без пароля для заранее подтверждённых фоновых запросов:
sudo /home/yaroslav/.cline/data/workspaces/chat/n14-control/scripts/install-service.sh --allow-active-user yaroslav
```

Используйте **один** нужный вариант. Второй добавляет узкое правило Polkit
правило `/etc/polkit-1/rules.d/49-n14-control-cpu.rules`, не выдаёт произвольный root-доступ.
Повторная установка без флага сохраняет ранее добавленное правило. Для отзыва
этого разрешения администратор удаляет именно этот файл. Авторизация `auth_admin`
по умолчанию не подходит для непрерывной фоновой автоматизации: фоновые запросы
не показывают пароль и при отсутствии разрешения завершаются ошибкой.

Установка копирует root-owned бинарник в `/usr/local/libexec/`, D-Bus policy,
Polkit actions и systemd unit. Сервис использует **системные Qt Core/DBus**,
а не пользовательские библиотеки переносимого GUI; на целевой системе они уже
установлены. Переносимый GUI сам не устанавливает и не обновляет root-сервис.
Установка/запуск/остановка сервиса не меняют значения CPU, не останавливают
CoolerControl и не выполняют EC/ACPI-транзакций. Пакеты и загрузчик не меняются.

Порядок применения пресета: подтверждённый системный профиль → CPU → подтверждённая
частота экрана. Внутри CPU-операции есть предварительная проверка, readback и
ограниченный откат записанных атрибутов. Между разными системными сервисами общей
атомарности нет: при ошибке последующего этапа предыдущий мог уже примениться.
Это показывается в журнале; автоматических бесконечных повторов нет. Сервис не
удерживает настройки постоянной перезаписью и не исключает внешние CPU-тюнеры.
Реальное применение на этой машине и физические sleep/unplug циклы пока не проверены.

Только чтение возможностей CPU, без root и записи:

```sh
/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/service/n14-hardware-service --probe
```

Отключение/удаление сервиса не восстанавливает ранее записанные пределы CPU:
сначала примените нужный профиль с режимом «максимум CPU» и нужным состоянием
Turbo Boost либо перезагрузите систему. Для остановки именно этого сервиса:

```sh
sudo systemctl disable --now n14-hardware.service
```

RCU Lazy на исследуемом ядре уже включён (`Y`), а sysfs-параметр имеет режим 0444:
оперативное переключение недоступно. Изменения загрузчика не реализованы.
Поддержка MSHybrid/iGPU-only/dGPU-only пока не подтверждена: выбор PRIME и
runtime-PM не выдаются за аппаратный MUX. Общие требования: [REQUIREMENTS.md](REQUIREMENTS.md).

## Отдельная opt-in проверка restart

`scripts/test.sh` собирает также `build/lifecycle-probe/n14-hardware-lifecycle-probe`.
Этот headless native probe использует настоящий `HardwareBackend`, не запускает
GUI и не получает root. Он требует исходный manual mode `2`, один раз повторяет
точно текущий CPU target, затем ждёт внешнего restart и проверяет новый owner,
сброс EC-active, 1500 ms без автоматического reacquire и явное свежее наблюдение.
При ошибке probe не ретраит и не пишет recovery targets. Существующий receipt
или ready-marker не перезаписывается.

Только после отдельного разрешения владельца, при закрытом GUI и совпадающих
installed/build binaries, в первом терминале запустите reviewed companion:

```sh
/usr/bin/pkexec /home/yaroslav/.cline/data/workspaces/chat/n14-control/scripts/hardware-lifecycle-restart.sh hardware-lifecycle-manual-20261031
```

После `AUDIT_READY` в течение 90 секунд запустите во втором терминале:

```sh
/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/lifecycle-probe/n14-hardware-lifecycle-probe /home/yaroslav/.cline/data/workspaces/chat/n14-control/build/hardware-lifecycle-manual-20261031.json
```

Для каждого нового окна используйте новое одинаковое имя в обоих терминалах.
Companion ограничен одним `n14-hardware.service`, sampler не перезапускает и EC
команд не отправляет. После окончания он печатает JSON audit между
`DBUS_AUDIT_JSON_BEGIN/END`. Успех требует не только `ok=true`, но и проверки
audit: ровно один ожидаемый Apply до restart и ни одного после. Это проверка
D-Bus boundary, а не независимый trace всех ACPI packets или thermal/load pass.
Первый живой receipt и исправленная audit readiness-проверка описаны в `VALIDATION.md`.

## Bounded thermal/load gate

`scripts/test.sh` также собирает `build/load-probe/n14-hardware-load-probe`.
Реальный `/sys`-прогон требует явного `--physical`; fake-sysfs входы разрешены
только на private D-Bus. Probe проверяет сеть, CPU critical temperature и
температурный stop limit, не добавляет retry/recovery write и после успешных
операций восстанавливает исходные CPU/GPU raw targets отдельными командами.
При неизвестном/частично применённом EC состоянии он останавливает только свою
нагрузку и завершает работу без компенсирующей записи; восстановление тогда
делается отдельным явным `--restore-report` с новой provenance.
Потоки имеют собственный deadline независимо от задержек D-Bus. Потеря сети,
температуры или превышение тестового stop limit тоже завершают прогон без EC
компенсации. Restore принимает только корректный protocol-2 receipt с целыми,
точно представимыми targets и проверяет readback после каждой из двух команд;
если текущие mode/keylight отличаются от receipt, запись не начинается.

Пример новой физической проверки (только после отдельного разрешения):

```sh
/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/load-probe/n14-hardware-load-probe \
  --physical --seconds 30 --max-cpu-c 88 --workers 4 --duty-percent 75 \
  /home/yaroslav/.cline/data/workspaces/chat/n14-control/build/NEW-load.json
```

Не переиспользуйте receipt. Физический прогон `hardware-load-ec-protocol2-20261031T1256+0300`
остановился через 2.271 s после EC transaction failure; его исходные `42/42`,
mode `2`, keylight `0` были затем восстановлены отдельным явным receipt
`hardware-load-ec-protocol2-20261031T1256+0300-restore.json`. Этот результат не
является thermal pass.

## Автоматизация

Правила применяются только после двух последовательных наблюдений источника
питания. Опрос раз в 5 секунд даёт обычную задержку 5–10 секунд, без busy loop.
Неизвестный источник питания сбрасывает подтверждение. Изменение настроек также
сбрасывает подтверждение. При ошибке нет бесконечных повторов записи: изменить
правило/переключить автоматику для повторной попытки.

На переходе источника/порога приложение делает одну попытку для каждого правила;
оно не борется с другими программами за постоянное удержание профиля. Ручное
переключение внутри приложения отключает соответствующую автоматику. Профиль и
частота имеют независимые флажки. При неизвестном проценте заряда применяется
обычный батарейный профиль, но не низкозарядный.

При подключённом БП ток батареи не показывает потребление всего ноутбука.
`Not charging` + 0 Вт не означает нулевое потребление системы. График и среднее
используют только отсчёты `Discharging`. `current_now` × `voltage_now` переводятся
из микроамперов/микровольт в ватты; `power_now` из микроватт. Отсутствующее значение
не подменяется нулём. При нескольких батареях мощности суммируются только при
наличии данных у всех, процент заряда — простое среднее (не оценка общей энергии).

## Текущий legacy preview и native reference-срез

Текущий экран приложения всё ещё является legacy preview и не выдаётся за CoolerControl
Standard. Его начальные точки CPU/GPU совпадают с текущими CoolerControl Graph profiles:
CPU `0→0, 60→0, 61→10, 75→54, 90→100, 100→100`; GPU `0→0, 50→0, 65→55,
75→100, 100→100`. Диапазон duty — **0–100%**, включая намеренный результат 0%.
Правка таблицы обновляет график; «Сохранить» пишет только настройки N14 Control.
Допускаются 2–64 точки, строго возрастающая температура, неубывающая команда,
последняя команда 100%. Интерполяция линейная; вне диапазона крайнее значение.

CoolerControl Graph native layer использует весь диапазон duty **0–100%**. Отдельный
legacy NBFC threshold validator сохраняет исходное ограничение `FanSpeed 40–100%`,
CPU ≤90°C, GPU ≤80°C и hysteresis-таблицу; это не ограничение CoolerControl Graph.
Графы, freshness/provenance и отсутствие аппаратной записи проверяются отдельно;
перенесённый Standard/Functions остаётся reference-only, а runtime gate не
получает источник snapshot автоматически и не выполняет аппаратных записей.

Фильтры применяются к **симулируемой температуре перед кривой**:

- Identity: без изменения;
- Hysteresis: новое значение принимается, когда отклонение от последнего принятого
  достигает заданного порога; первое значение принимается сразу;
- MovingAverage: среднее последних N отсчётов.

Предпросмотр работает раз в секунду только на открытой вкладке кривых; изменение
входа также добавляет отсчёт. Это отсчёты, не гарантированное временное окно.
Параметры и CPU/GPU-фильтры независимы. При отсутствии температуры фильтр
сбрасывается. Спящая GPU исключается из Mix; отсутствующая температура активной
GPU делает результат недоступным, а не нулевым. Min/Mean пока исключительно
симуляционные варианты и не разрешены для аппаратного управления.

## План следующих этапов

Существующие функции охлаждения и подсветки переносятся **строго как в
опубликованных исходниках GitHub**, без самостоятельной замены политики отказа,
алгоритмов или Standard. Закреплённый commit, исходные риски и проверки описаны
в [контракте переноса](reference/PORTING.md).

1. После bounded live test EC protocol 2 и одного clean active-owner restart
   отдельно проверить длительную нагрузку, sleep/resume и restart в окне
   незавершённой операции; выполненные bounded tests не заменяют эти проверки.
2. Сохранить preflight, serialized queue, identity/provenance gates и исходную
   failure policy. Реализация root broker уже существует;
   [контракт writer-а](reference/EC_WRITER_DESIGN.md) описывает её границы.
3. Подсветка уже проходит через ту же EC-очередь. Независимая запись запрещена;
   raw readback не является физическим измерением яркости.
4. Только подтверждённые оптимизации с доступностью, описанием и откатом.
   D3cold не гарантируется одной записью `power/control=auto`; MUX-переключение
   возможно только при наличии проверенного интерфейса прошивки.

Прежняя политика «без автоматической аварийной подстановки скорости» не меняется
этой версией. По прямому требованию пользователя новый перенос должен сохранять
именно опубликованное поведение, включая сохранение последней команды при отказе.
Это может оставить вентиляторы остановленными неограниченно долго и не является
гарантией тепловой безопасности. Перед включением требуется проверить эквивалентность.

## Разработка и проверки

Подтверждённая среда: Qt 6.12, qmake6, make, C++20. Новые пакеты на ноутбук
не устанавливаются. Для сборки нужны dev-инструменты; для запуска архива — нет.

```sh
./scripts/build.sh
./scripts/test.sh
```

QtTest проверяет алгоритмы, настройки, режимы дисплея и фальшивые деревья sysfs.
Текущие результаты и границы живых проверок: [VALIDATION.md](VALIDATION.md).
GUI smoke-test проходит по всем вкладкам в демо через offscreen software backend,
сохраняет снимки в `build/`, не переключает реальные профили/частоту.
Проверка на фальшивом железе не заменяет реальный тест перехода AC/батарея.

### Реальная hardware-проверка

Это отдельный opt-in тест: обычный `scripts/test.sh` намеренно не пишет в EC.
После установки и перезапуска обоих system services, при закрытом обычном GUI,
запускается тот же application-side D-Bus backend:

```sh
./scripts/hardware-integration-test.sh build/hardware-integration.json
```

Тест отказывается писать при старом установленном service binary/EC protocol,
если EC уже занят другим GUI или если начальный fan mode не равен manual `2`.
Это необходимо для точного restore: disconnect освобождает lease, но не возвращает
firmware automatic mode `0`. Первый status-запрос сам проходит service-side
авторизацию; test harness не вызывает acquire/release.
Тест последовательно проверяет `both=0/40/100`, отдельные `cpu=40` и `gpu=60`,
`keylight=0/50/100`, после каждой операции читает raw target и CPU tach, ждёт
5 секунд для физического отклика и восстанавливает исходные значения. Для
прохождения speed assertion CPU tach обязан увеличиться между 40% и 100%; JSON
содержит raw readback, nominal CPU RPM (`tach_raw * 100`), факт tach-отклика
40%→100%, `restored_to_initial`, `control_state_restored`, версию EC protocol и
bounded transient-snapshot retries. Смена исходного режима не считается restore.
GPU RPM этим EC transport не выдаётся и не
подменяется command/readback процентом.

Настройки: каталог `QStandardPaths::AppConfigLocation` (его абсолютный путь виден
на вкладке «Система и журнал»). История в памяти не переживает перезапуск, пока
пользователь не экспортирует CSV. Сервис и автозагрузка сами не устанавливаются.