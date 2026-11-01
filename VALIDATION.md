## GPU observation dropout — offline fix PASS (2026-11-01)

- Read-only live inspection found all three N14 services active with zero
  restarts, but the controller had latched `failed=true`. Journal at 14:44:33
  reported `GPU EC/PM read: EC temperature byte is outside 1..119 C`; at
  14:44:34 the post-command observation stopped the curve with
  `GPU power state unknown/transitional/error`. This is a controller failure
  state, not a crashed service or evidence that Rust would fix the issue.
- The service allowlist omitted invalid GPU temperature bytes and power changing
  during the PM observation. New private-bus tests reproduced four failures
  BEFORE the fix: both missing variants at the service boundary and both in the
  real controller's post-command path. See
  `build/gpu-observation-regression-before.{log,exit}` (exit 4).
- Both variants now use the existing typed `EcSamplePending` response only after
  validated CPU/paired metadata and exactly one known GPU error with no GPU
  measurement/sample/clock. No EC operations occur while waiting. A fresh
  accepted sample produces a new curve calculation, not a replayed command.
- Existing limits are unchanged: no waiting authority without a last accepted
  observation, strict original wall/boottime age less than 5 seconds, no renewed
  clocks, and no stale-token Apply. CPU, transport open/read, malformed data,
  raw-readback/partial-write failures and expired grace remain failures. This
  does NOT guarantee continuous control through actual hardware/sensor failure;
  the original last-target thermal risk remains.
- Targeted regression run: **24 passed, 0 failed** (22 data cases plus setup and
  cleanup). Full `scripts/test.sh`: **370 Qt checks + 10 Python tests**, no
  failures or skips: core 207, service 67, hardware 61, GUI 26, sensor 9. Offscreen
  GUI smoke and saved-controller-config validation also passed. Logs and exit
  markers are under `build/gpu-observation-fix/`.
- At the initial fix audit, no live Apply, service restart or load test was performed. Installed hardware,
  controller and sensor hashes still match the pre-change receipt. Deployment
  requires user-run sudo; until then the running curve remains failed. The
  audited `scripts/update-ec-stack.sh --user yaroslav` uses refreshed verification
  receipts for this exact passing build, refuses an open GUI/write probe and
  stages binaries before an explicit coordinated hardware/controller restart.
  Deployment was later confirmed by the read-only release audit below. Extended
  physical recurrence/stability validation remains outstanding.

## GUI release 0.2.1 — archive and launcher PASS (2026-11-01)

- Release archive: `dist/n14-control-0.2.1-linux-x86_64.tar.gz`.
- The matching SHA-256 is stored in the adjacent
  `dist/n14-control-0.2.1-linux-x86_64.tar.gz.sha256` file.
- The archive contains `AppRun`, `install.sh`, the GUI binary, the SVG icon and
  `share/applications/n14-control.desktop`. The installer creates the desktop
  launcher and hicolor icon for either system (`sudo ./install.sh`) or user
  (`./install.sh --user`) installation.
- User installation was completed and verified at
  `/home/yaroslav/.local/bin/n14-control`,
  `/home/yaroslav/.local/share/applications/n14-control.desktop` and
  `/home/yaroslav/.local/share/icons/hicolor/scalable/apps/n14-control.svg`.
  The installed GUI hash equals `build/app/n14-control`; `desktop-file-validate`
  and the installed binary `--help` check passed. Binary version and bundle
  metadata are both 0.2.1; the GUI's desktop-file ID is `n14-control` for KDE
  Wayland launcher/window association.
- The final full release run passed **370 Qt checks + 19 Python tests** with no
  failures or skips. Nine new installer tests cover user install/reinstall,
  bundle layout, working-directory independence, spaces/metacharacters in HOME,
  missing binaries, invalid arguments, root refusal and version/template parity.
  Offscreen smoke execution of the installed GUI also passed.
- The GUI installer is intentionally separate from the privileged EC stack: it
  does not restart services or write EC registers. System hardware/controller
  deployment requires a separate sudo-authorized action; it was not performed
  by the GUI installer. The source and release assets are published in the
  public repository `https://github.com/SaYl1Cs/n14-control`.
- A subsequent read-only audit found the hardware/controller services already
  started at 16:23:51 MSK. Installed hardware, controller and sensors binaries
  match the current build hashes, including the GPU-observation fix. All three
  services are active and legacy CoolerControl/NBFC/adapter/selector inactive.
  Two controller observations 10 seconds apart reported `mode=curve`, ready,
  not failed/sleeping; cycles advanced 6401 -> 6411, confirmed commands 791 ->
  794, and sample IDs progressed. This release task did not perform that root
  deployment or restart. Receipt: `build/release-0.2.1-runtime-status.json`.

## Pre-login controller and physical lid/resume PASS — 2026-11-01

- The fan curve was moved from the graphical user-session unit to the native
  system unit `n14-fan-controller.service`, running as unprivileged `yaroslav`
  before `plasmalogin`. The GUI and headless controller use system D-Bus
  `org.n14control.Fans1`; `org.n14control.Hardware1` remains the only EC writer.
- The migration explicitly disabled the old CoolerControl/adapter/profile-
  selector stack. `n14-sensors`, `n14-hardware` and `n14-fan-controller` are the
  only active N14 control services; no competing writer was present.
- Pre-login installation initially exposed two deployment defects and both were
  fixed: a system-unit `%h` expanded to `/root` instead of `/home/yaroslav`, and
  the pre-login Polkit subject has no active local session. The final unit uses
  the absolute config path; EC authorization is limited to the exact `yaroslav`
  account, while CPU authorization remains local-and-active-session-only.
- Physical test: lid close → deep suspend → AC removal while asleep → lid open /
  resume. User reported fan curve and system behavior correct; keyboard lighting
  also restored after installing the new controller binary. Journal confirmed a
  post-resume `keylight=50%, raw readback=88` command and subsequent confirmed
  fan-curve commands. The post-test receipt is
  `build/power-resume-validation/post-lid-resume-keylight-20261101T004213.json`.
- Post-resume status: `mode=curve`, `ready=true`, `failed=false`,
  `automatic_retries=0`, `recovery_writes=0`, `keylight_pending_percent=null`.
  Hardware regression suite after the restore change: **61 passed, 0 failed**;
  the added `headlessCleanSleepRestoresLastConfirmedKeylight` test passed.
- The obsolete `/usr/lib/systemd/system-sleep/90-colorful-n14-acpi` hook must be
  moved out of `system-sleep` during migration because it targets the removed
  legacy adapter socket. Native lid reconciliation now runs inside the root
  hardware service using the fixed `PSTA/LIDS/_Q14` sequence and does not alter
  fan targets or replay stale EC commands.

## Asynchronous CPU policy confirmation — regression fixed, live test pending (2026-10-31)

- First real GUI preset attempts failed with immediate CPU readback mismatch;
  the first reported an UNCONFIRMED rollback. Direct single CPU-only request to
  the same normal service later confirmed all 16 policies at 2400000 kHz, with
  Turbo unchanged and a healthy fan curve. This was NOT a complete preset pass;
  panel/profile/full-automation/sleep tests remain pending.
- Upstream `drivers/cpufreq/cpufreq.c` shows sysfs limits update freq QoS and
  `cpufreq_notifier_{min,max}` schedules `policy->update`. The reference was saved
  in `build/power-resume-validation/linux-cpufreq-reference.c`; it is NOT claimed
  byte-identical to the running CachyOS 7.3 kernel. Delayed policy visibility is
  a supported interface behavior, not a proved diagnosis from the log alone.
- Three new delayed-kernel fakes reproduced both limit/removal failures and false
  rollback failure BEFORE the fix. CPU controller now confirms exact readback
  with read-only 1 ms polling, one shared 250 ms budget for all planned changes
  and a separate shared 250 ms rollback budget. No CPU write retry or blind fixed
  settle delay. EC/keylight transactions and their timing are unchanged.
- Persistent mismatch still fails, reporting expected/actual bytes; rollback
  failures retain detailed cause. New timeout regression checks bounded duration,
  precise mismatch diagnostics, exactly one attempt plus one rollback write.
- CPU work is synchronous and remains serialized in the root service; do not
  replay stale EC commands. Existing single-owner/freshness guards remain active.
- Artifacts: `build/cpu-async-readback/`. Full suite and live GUI confirmation
  are required before declaring this CPU/preset issue fixed.

## Full power presets / CPU / resume validation — in progress (2026-10-31)

- User requested live power-profile and CPU-ceiling tests plus unplugging AC
  while asleep. Explicitly chose full presets: AC=performance/165 Hz/hardware
  ceiling; battery=power-saver/60 Hz/2400 MHz. Existing saved AC mapping pointed
  to low-battery/400 MHz although preset automation was OFF. Corrected the mapping
  by saved preset identity, preserving names, CPU values and all cooling settings.
- Explained `hardware` CPU mode: write each policy's `cpuinfo_max_freq` to
  `scaling_max_freq`, not fixed-clock/overclock/Turbo enable. Actual machine
  ceilings were 3.6/4.7/4.9 GHz; Turbo is separate and remains unchanged (`-1`).
- Added regressions that FAILED before fixing: peripheral supplies distorted
  laptop battery/AC data (12% laptop + 95% mouse -> 54%); pending CPU authorization
  blocked existing EC-owner observations and writes with `LimitsExceeded`.
  Fixtures only; no live CPU/EC request was used to reproduce these defects.
- Fixes: ignore `scope=Device` for all laptop power telemetry; permit only the
  already active, same EC owner to continue normal fresh commands during CPU
  authorization. Sleep, competing-owner, transport and freshness gates remain.
  Added a demo GUI resume regression that selects battery preset after two
  settled observations, not the manually selected pre-sleep AC preset.
- Full standard pipeline **343 Qt + 10 Python**, no failures/skips/warnings
  (core 203, service 45, hardware 60, GUI 26, sampler 9). See
  `build/power-resume-validation/release.{log,exit}`. A first full-run failure
  came from reusing a fixture client deliberately disconnected by an earlier test;
  fixed with a dedicated CPU client, retained the failed log, then reran ALL.
- User explicitly authorized the optional action permission for active local
  `yaroslav`, not a general root/binary permission. Installed
  `/etc/polkit-1/rules.d/49-n14-cpu-automation-yaroslav.rules`, root:root 0644,
  from `service/49-n14-cpu-automation-yaroslav.rules.example`. Default policy
  XML unchanged. The bounded service still checks request paths/ranges/readback.
- New broker installed/running PID 1793564, SHA-256
  `46ed800042fe340b67316179bb3d48467731b1bed90c9092a6b04ded35d0012d`;
  GUI restarted alone as PID 1801025, SHA-256
  `9cdc37e7d83a524a4915ddde284eb1bc7acf610d9e3540d2d68dfb597100fea8`.
  Controller/sampler binaries and fan settings unchanged. Preserved single EC
  owner; installation sent no CPU/light test command. Automation is temporarily
  OFF for manual tests; full automation enablement and real sleep test pending.
- Evidence: `build/power-resume-validation/{before/,red-core.log,red-service.log,
  deployment.log,gui-relaunch.json,manual-ready-20261031T183624.json}`. The unusual
  saved ID `battery` currently names the user's AC preset; it was NOT renamed.

## Production preload-before-auto migration — installed, GUI physical PASS (2026-10-31)

- After the successful diagnostic endpoint experiment below, the user explicitly
  authorized permanent migration and a GUI test. Production source now preloads
  the NEW light byte before auto, retains one 20 ms prewrite wait, and restores
  mode/independent CPU/GPU targets once. Fan-command register sequence is unchanged.
- The diagnostic thread-local hook was NOT copied to production. A synchronous,
  request-local `KeylightContinuationGuard` is passed explicitly through broker
  and queue. It runs before preload, main light write and fan return, and is not
  retained or called for fan commands. Original sample clocks are checked before
  AND after read-only snapshot/live-PM I/O, so an I/O stall cannot rejuvenate write
  authority. No timer, delay after writing, retry or recovery write was added.
- Added queue/broker ordering/guard/failure regressions and real service tests
  for GPU changes at all three continuation boundaries, changed snapshot epoch,
  and original sample expiry during a delayed check despite a NEWER snapshot.
  Full standard pipeline and authorized deployment receipts live under
  `build/keylight-preload-production/`.
- Standard pipeline: **338 Qt + 10 Python checks**, zero failures/skips or compiler
  warnings; `release.exit` is `0` (core 201, service 43, hardware 60, GUI 25,
  native sampler 9). Earlier 327/323 totals describe earlier builds, not this one.
- Production deployment: `deployment-20261031T181035.json`, installer exit `0`.
  Installed and RUNNING broker PID 1630598 both match SHA-256
  `87740f14cad45d0102bbc4bf241aed447641cdbdb02011880735524a7eec7bac`.
  Existing known session-bus GUI preserved, user-controller binary unchanged,
  sampler not restarted, no installer light commands or automatic recovery.
- User physically tested the GUI slider and reported **"Всё прекрасно работает"**.
  Actual captured commits include 10/20/30/100, additional intermediate levels,
  0, then further changes to 50; do NOT relabel them as an exact 25/75 script.
  All 17 captured commands used preload-before-auto with six write attempts;
  no transaction rejections. Curve remained healthy with no pending light intent.
  Evidence: `gui-journal.log`, `gui-commits.json`, `gui-observation.json` in the
  same production receipt folder. Physical slider response is now confirmed;
  photometric calibration and physical keylight sleep/resume remain unvalidated.
- This intentionally supersedes the earlier strict pinned-order requirement
  for lighting only. Authorization and physical evidence are recorded below;
  `reference/PORTING.md` and `reference/EC_WRITER_DESIGN.md` disclose the difference.

## Authorized preload-before-auto experiment — physical endpoint PASS (2026-10-31)

- User explicitly authorized an isolated preload of the NEW light byte **before
  the single transition into automatic fan mode**, followed by the normal 20 ms
  prewrite wait, auto verification, normal light write and single fan restoration.
  This is a disclosed ordering deviation from pinned source, NOT a production
  change or an automatic retry after failure. No postwrite wait is added.
- Purpose: test an automatic-mode-entry latch hypothesis after both the one-second
  prewait and two adjacent writes failed. The cached, hash-matching DSDT shows
  `WMBA -> WSIO` performs indexed I/O; it does not establish the EC's internal
  brightness-application behavior. The latch hypothesis remains unproven.
- Artifacts: `build/keylight-preload/`. Read-only guards before preload, normal
  light write and manual restore retain ORIGINAL Apply clocks (<2 s), matching
  sampler/GPU epoch and live GPU PM. Guard/transport failures terminate without
  further writes, retries, or automatic service/controller recovery.
- Receipt: `build/keylight-preload/physical-20261031T175853.json`. Sequence
  `0 -> 100 -> 0` completed through the existing controller. Each transaction had
  a preload before auto, one 20 ms sleep, a normal light write, and one fan return
  (six total write attempts). Measured prewrite windows: 21.245 / 20.996 /
  21.081 ms; admission sample ages: 27 / 163 / 55 ms. Epoch generation 186 was
  unchanged. No transport/freshness failure or extra postwrite wait occurred.
- **User physically confirmed: "Зажглась на 100% и теперь полностью погашена".**
  Observation sidecar: `build/keylight-preload/physical-observation.json`.
  Unlike the previous failed experiments, this is a physical ENDPOINT pass,
  not merely a raw-readback pass. It supports an order-dependent application
  hypothesis; the internal EC mechanism is not proven. Intermediate levels,
  continuous slider operation and sleep/resume remain unvalidated for this path.
- **58 isolated diagnostic Qt checks passed**, including all transaction failure
  boundaries and original-age/epoch/live-PM guard stops. Re-ran unchanged normal
  binaries: **327 Qt + 10 Python**, no failures/skips. See this diagnostic folder's
  `tests.{log,exit}` and `baseline-tests.{log,exit}`; both exit files contain `0`.
  Normal broker was restored and running executable hash verified as
  `765a9ef0e6fa3b0e5baaa7bf14a5d0e1d60ef78180e4685fcf2c87fa0ae74836`;
  controller resumed healthy in curve mode, no pending light intent. Production
  sources, artifacts and user configuration remain unchanged. No preload path
  has yet been installed permanently.
- Separately, the user identified `btop` as the GPU wake source and swap activity
  as the cursor-stutter cause. NVIDIA was observed `suspended/D3cold` after btop;
  no GPU, swap or compositor configuration was changed for these diagnoses.

## Isolated keylight diagnostics — long prewait and adjacent double-write ineffective (2026-10-31)

- The user authorized an isolated **1000 ms BEFORE light write, zero additional
  wait after** experiment. Receipt:
  `build/keylight-prewait-1s/physical-20261031T172712.json`. The sequence through
  the existing controller was `0 -> 100 -> 0`; measured prewrite windows were
  1000.707 / 1000.591 / 1000.650 ms. Raw readbacks were 175 / 0 / 175.
- The user explicitly confirmed the final keyboard was **still bright**.
  Thus increasing the prewrite delay to one second did NOT fix the physical lag;
  the software/protocol pass in that receipt is not a physical pass.
- The normal 20 ms broker was restored; installed and running executable hashes
  were checked, and the controller resumed healthy in curve mode. The experiment
  did not renew the original sample clocks or relax the two-second write gates.
- Subsequently the user explicitly authorized trying **two consecutive writes
  of the same light value within ONE transaction**. This narrowly overrides the
  earlier rejection of double-write workarounds for this experiment only; it is
  a disclosed difference from pinned source, not a retry after an error.
  Isolated sources/tests/install receipt live in `build/keylight-double-write/`.
  Preserve the single normal 20 ms prewrite wait and single fan-mode/target
  restoration. No postwrite wait, repeated fan transaction, or recovery write.
  The experiment is now complete; production sources remain unchanged.
- Double-write receipt:
  `build/keylight-double-write/physical-20261031T173819.json`. All three logical
  commands (`0 -> 100 -> 0`) committed, each with exactly two light writes and
  six total register-write attempts (one auto transition, two light writes,
  one manual/CPU/GPU restoration). Prewrite windows were 20.298 / 20.521 /
  20.307 ms; admission sample ages were 43 / 178 / 67 ms. Epoch generation 183
  was unchanged and raw readbacks were 175 / 0 / 175. No extra postwrite wait
  or terminal failure occurred.
- The user reported **"Снова осталась яркой после конечных 0%"**. Physical
  application FAILED despite the protocol pass. User-observation sidecar:
  `build/keylight-double-write/physical-observation.json`. Two adjacent light
  writes inside one mode bracket do NOT reproduce the previously successful
  separate repeat command. The reason for that difference is unproven; do not
  label a mode/latch hypothesis as a confirmed diagnosis.
- Restored the original broker from a hash-verified root-owned backup; both
  installed and running normal executable hashes matched
  `765a9ef0e6fa3b0e5baaa7bf14a5d0e1d60ef78180e4685fcf2c87fa0ae74836`.
  The controller resumed in curve mode with `failed=false`, no pending light
  intent. No additional lighting command was sent after the last 0%.
- Offline validation: **50 diagnostic Qt checks** passed, covering exact order,
  both light-write failure boundaries, uncertain writes, all other transaction
  failures, unchanged fan commands, and authority rejection. The synthetic
  one-write-lag model is a hypothesis test, NOT physical hardware evidence.
  Diagnostic build/tests exit `0`: `build/keylight-double-write/tests.{log,exit}`.
  Re-ran unchanged normal test binaries: **327 Qt + 10 Python**, no failures or
  skips (`build/keylight-double-write/baseline-tests.{log,exit}`). These totals
  are separate from the 50 diagnostic checks, not a full diagnostic-stack pass.
  Original production source/artifact checksums remain unchanged.

## Physical keylight follow-up — unresolved one-command lag (2026-10-31)

This follow-up supersedes any inference below that the raw register or an
approximate intermediate physical brightness proves correct physical application.

- The running GUI was verified against its build (SHA-256
  `8d6620f374100d8b70dd46424eeed25dfe32013de6b3aa9d1f15208050d50e68`).
  Added real mouse-press/move/release tests with cached readback interleaving,
  and FanClient/controller/backend sequence tests. Both send the NEW value.
- A single 25% request had correct raw `132` but the user reported physically
  off (the previous 0%). Receipt: `build/keylight-lag-25-20261031T161430.json`.
- Restored the pinned automatic-mode check AFTER the 20 ms prewrite settle.
  A 75% request (raw `44`) was described as bright, but that approximate answer
  was NOT a successful endpoint test. After `0 -> 100 -> 0` the user still saw
  the previous full brightness. Receipt:
  `build/keylight-mode-guard-endpoints-20261031T162503.json`.
- Removed extra interleaved mode/light/fan reads, matching the pinned register
  order: cache mode/targets; write auto; wait 20 ms; read auto; write light;
  write manual/CPU/GPU; read manual/CPU/GPU; finally read light. Exact-order
  tests failed before this repair. Final fault-injection rows cover every
  operation in the restored sequence, including uncertain writes, with no
  automatic retry/recovery. **327 Qt + 10 Python checks passed**, zero failures
  or skips (`build/keylight-register-order-release.{log,exit}`). This is source
  conformance, NOT a physical fix: the user again saw full brightness after
  `0 -> 100 -> 0`, despite final raw `175`. Receipt:
  `build/keylight-register-order-endpoints-20261031T164000.json`.
- Broker 1067796 running hash was verified by the root-authorized installer as
  `70a4a6066f084c9894e8a81f6a2395801068a2ba8537521e2d2ef903acda0af5`.
  GUI stayed open, the sampler/configuration/Polkit were not changed, and the
  curve remained operational with `failed=false` throughout these light tests.
  See `build/keylight-register-order-deployment.{log,exit}` and
  `build/keylight-register-order-verified.sha256` for this stage, not later builds.
- A single EXPLICIT diagnostic repeat of the same 0% (raw `175` before and after)
  then physically extinguished the keyboard, confirmed by the user. This
  separates one-command lag from simple scale inversion. It is NOT a fix or
  permission to implement automatic double-writes. Receipt:
  `build/keylight-repeat-zero-diagnostic-20261031T164330.json`.
- User rejected speculative post-write delay: do not add one. The broker's
  production `sleep_for(20 ms)` callback is connected, not the fake queue's
  no-op default. Added steady-clock sleep/prewrite timing capture, logged ONLY
  after transaction I/O, without new EC operations or additional waits. The
  actual broker callback is covered by a fake-transport test requiring at least
  20 ms actual sleep. Measurement/diagnosis is pending; do not label it a fix.
- The source's published 20 ms experiment measured an actual 20.48--20.60 ms
  prewrite window including Python/ACPI overhead. That range is evidence, not
  a proven hardware readiness threshold. No new prewrite threshold is inferred.

## Joint EC/keylight repair — earlier evidence (2026-10-31, 16:02)

This section supersedes earlier process IDs, test totals and "root broker
unchanged" notes below. Those sections and receipts describe earlier stages,
not the final running stack. Full source/policy audit:
`reference/EC_STACK_AUDIT.md`.

- The original slider regression is reproduced and fixed: committed
  `valueChanged`, 120 ms single-shot coalescing, one latest controller intent,
  post-command raw verification and no rollback from stale cached GUI state.
  The rebuilt launcher no longer prefers the outdated dist GUI.
- Two common-boundary defects were found after isolated keylight success:
  clean sampler epochs did not rotate broker generation; short GPU PM observation
  mismatches terminal-failed the complete curve. New epochs rotate before demand
  calculation. Typed no-write waiting uses the original sample clocks (<5 s);
  stale Apply tokens and uncommitted Standard state are discarded.
- Initial coordinated physical deployment still stopped at CPU 92 C because
  the port retained the source `cool_for_hold()` temperature cutoffs. That failed
  live attempt is preserved, not relabeled as success:
  `build/ec-stack-initial-deployment.log` and
  `build/ec-stack-before-hot-grace-deploy.json` (last confirmed targets 70/70).
  Per the user's explicit instruction to continue irrespective of temperature,
  the final observation-only wait has no temperature cutoff. This is an explicit
  source-policy difference, NOT permission to write from old data, an emergency
  target or a retry after any uncertain write. The strict original-age bound
  and terminal transport/readback policy remain intact.
- Final **323 Qt checks + 10 Python checks**, no failures/skips; exit `0` in
  `build/ec-stack-final-tests.exit`, full log
  `build/ec-stack-final-tests.log`. Qt totals: core 193, service 38, hardware 59,
  GUI 24, native sampler 9. No compiler warnings in the final log.
- The service suite now runs FanClient -> FanController -> HardwareBackend ->
  real HardwareService/transaction queue -> fake EC, at CPU 92 C through three
  interleaved GPU transitions and lighting edits, clean sleep/resume and a
  terminal transport fault. Only synthetic fixture DSDT identity is normalized
  by a test-only bridge; production commands/provenance/readback/failure handling
  are exercised unchanged. No tests access the production system bus.
- Final coordinated deployment succeeded (`build/ec-stack-hot-grace-deployment.exit`
  = `0`): broker PID **830156**, user controller PID **830162**. The running
  `/proc/PID/exe` hashes, not only installed files, match their final builds.
  Sampler PID **414039** was not restarted. User configuration hash remains
  `080eb0908ce90ff50dbe8470e15da968507abfbbf4e41b4d91b8d3faa27a241a`.
- Privileged **read-only** audit `build/ec-stack-root-audit.json` confirms one
  EC ownership flock held by broker 830156, schema-2 snapshot age 153 ms with no
  errors and confirmed D3cold, matching running broker executable, and inactive
  CoolerControl/NBFC/legacy write adapter. It issued zero EC writes.
- Joint physical observation: `build/ec-stack-live-20261031T160156.json` = pass,
  120 cached status observations over 60 s, no artificial load. Curve cycles
  advanced **36 -> 96**, confirmed operations **12 -> 30**, and one 50% lighting
  intent was submitted through the SAME controller. Final light raw **88**,
  pending intent null, curve still active, raw targets 44/44 and tach 34 at the
  receipt endpoint. No retry, recovery write or competing EC client was added.
- Healthy-controller process CPU/RSS measured across that window:

  | Process | CPU (% of one core) | RSS before -> after |
  | --- | ---: | ---: |
  | controller | 0.1160% | 16508 -> 16500 KiB |
  | root broker | 0.1658% | 19416 -> 19456 KiB |
  | native sampler | 0.0663% | 2740 -> 2720 KiB |

  Total native stack ~0.35% of one core; no sustained RSS growth. This measures
  the native background processes, not the unopened GUI or legacy sampler.
  Earlier idle measurements of a failed/stopped curve are NOT substituted for
  this healthy observation.
- Limitations: GPU stayed at sampler generation 181 / D3cold in the physical
  window, so clean PM transitions are presently verified by the joint fake-EC
  test, not by a new physical transition. No new physical sleep/lid-cache test.
  Prior user-confirmed physical brightness change after a controller command is
  preserved; new physical mouse/keyboard interaction with the rebuilt GUI still
  needs user confirmation. BIOS lid-cache reconciliation is not implemented.

Final source/binary hashes: `build/ec-stack-verified.sha256`. Older red tests,
thermal-stop receipts, rejected deployments and backup binaries are retained.

## Headless curve-control repair and live validation (2026-10-31, 15:00 onward)

- Root cause of the stopped curve loop: `Window::acceptSample()` was its only
  scheduler. No GUI meant no curve updates; disconnect releases ownership but
  leaves the last manual-mode target in firmware.
- Added a single unprivileged headless client, `n14-fan-controller`, with a 1 s
  Standard cadence independent of the 5 s passive UI telemetry. It reuses the
  existing protocol-2 backend, pinned graphs/Functions, Polkit and root broker.
  GUI fan/keylight controls now use its session-bus facade; the GUI never starts
  a competing EC loop. Duplicate controller startup is refused before EC access.
- Existing user settings are reloaded without a separate root-owned copy.
  Changed curves reset the old graph processors; invalid/missing configurations
  stop control, not fall back to defaults. Only live Mix Max is supported.
- Failure is terminal until an explicit user mode request: no timer retry,
  automatic process restart, compensating target or emergency override. Raw
  success replies must exactly match the expected conversion. Clean resume
  calculates a fresh demand; interrupted operations and service-owner changes
  require explicit restart. These new lifecycle paths are offline-tested only.
- An early bounded prototype was stopped after its test, leaving raw `1`;
  a later passive check saw CPU `93 °C`. It is **not** a successful deployment.
  A separate explicit restore returned raw `42/42` with fresh readback before
  further work: `build/fan-explicit-restore-20261031T145110.json`. No load was
  started during that high-temperature observation. The final controller is
  intentionally left running instead of leaving a low static test target.
- Installed/enabled the final **user** service with PID `570196`, zero restarts.
  The existing root hardware/sensors PIDs remained `414043/414039`; their built
  and installed binaries still match. Legacy read-only sampler was not changed;
  CoolerControl, NBFC and the legacy write adapter remain inactive.
- Bounded physical test observed the running controller without sending EC
  commands: one CPU worker at ~10% duty, 8 s deadline, 18 s observation window,
  stop at `90 °C` below CPU critical `100 °C`. Pass: CPU sysfs max `85 °C`,
  target raw `1–53`, CPU tach raw `4–46` / nominal `400–4600 RPM`; load stopped.
  Receipt: `build/fan-curve-live-20261031T150102.json`. This is not a calibrated
  RPM sweep or long thermal stress test, and does not validate GPU RPM.
- After more than two minutes of continuous operation, a separate 20.06 s
  observation passed: mode `curve`, no errors, same broker generation `29`,
  ~`0.05%` process CPU and `16476 KiB` RSS. Receipt:
  `build/fan-curve-soak-20261031T150305.json`.
- New receipts use cached controller status, CPU sysfs and accessible journals.
  Privileged system-bus monitoring was denied, so they are **not** independent
  D-Bus/ACPI packet audits. No new physical suspend was performed; the keyboard
  brightness-after-sleep report remains a separate issue.
- A later attempt (`build/fan-curve-fresh-20261031T150617.json`) was stopped at
  CPU `91 °C` BEFORE its worker started. This is not another load pass and was
  not an EC rejection. No further physical load was started. The controller
  remained active and requested raw `70` when its CPU snapshot reached `93 °C`.
- Full offline suite: **304 Qt checks + 10 Python checks, zero failures**;
  `build/fan-curve-release-tests.log`, exit `0`. Includes headless thermal response,
  stable output suppression, terminal failures, exact raw readback, duplicate
  refusal, graph hot reload, invalid config, sleep/resume and GUI facade tests.
  Three Python probe checks also cover cache freshness and a hot-start refusal
  with no worker or D-Bus observation; guard-stop temperatures now contribute to
  the receipt's maximum instead of being omitted from the successful samples.

## Physical validation after diagnostic deployment (2026-10-31)

- Native sampler was deployed with a private `/run/n14-control` snapshot and
  lock, leaving the existing legacy producer on its separate runtime path.
- A 5-second physical Standard load used one worker at 10% duty, completed five
  cycles and three confirmed EC transactions, then restored CPU/GPU raw targets
  to `42/42` with fresh readback. `automatic_retries=0`,
  `recovery_writes=0`, and `control_state_restored=true`.
- Physical sleep/resume used an armed RTC wake. BOOTTIME confirmed
  `9.156051468` seconds suspended; EC authority was inactive during sleep,
  no automatic command was observed, and the final state remained mode `2`,
  CPU/GPU `42/42`, keylight raw `0`.
- A previous pre-isolation load stopped on
  `transaction sampler changed without a new broker owner generation`. The
  independent audit showed the native and legacy producers shared one snapshot
  path. The path split above removed that conflict; the isolated run completed.
- The GUI passive telemetry path is now asynchronous and coalescing, the
  native sampler interval wait is interruptible without 50 Hz polling, and the
  GUI history sampler is 5 seconds rather than 1 second. Offline validation:
  `294` Qt checks plus `7` Python checks, all passed.

# Local validation — 2026-10-31

Target: COLORFUL N14 24, CachyOS 7.3.0-4, KDE Wayland, Qt 6.12.0.

## Strict published-source reference

- User explicitly requires existing functionality to retain the published GitHub
  behavior, including its no-emergency-override policy. No alternative failure
  policy was introduced. The reference-only tests below did not transfer hardware
  ownership; later bounded live evidence is recorded separately.
- `main` was checked with `git ls-remote`: `4967a3e73105014fb640150587e538c6ca594941`.
  The local unpublished sleep-gate commit is excluded from this reference.
- All **75 pinned Git blobs** were verified and extracted independently of the
  source working tree. The source lock records the exact dependency commits and
  integration patches; upstream dependency suites themselves were not rerun here.
- Selected unchanged source suites: **156 passed** (fan failures, lighting,
  hot-load curves, lifecycle, sleep, sleeping-GPU readings and protocol).
- Source-lock extraction unit tests: **7 passed**, covering uncommitted/untracked
  files, unpublished commits, replace objects, wrong trees and symlinks.
- These are tests of the reference and its extractor. They are **not proof of a
  completed runtime fan-control port**; no runtime cooling ownership changed.
- Native reference slice: **9 original + 43 Standard golden scenarios / 1980
  cycles** are included in the core
  executable (strict threshold schema, stateful hysteresis, producer normalization/
  interpolation, GPU sleep/missing-input behavior, exact fan/keylight conversions,
  thermal snapshot/provenance validation, hash-locked Standard/Functions state
  transitions, and the read-only runtime provenance gate). The native app does not
  open EC; only the separately installed root service contains the transport. The
  service/client lifecycle changes are tested offline; separate bounded live
  receipts for the previous API and service-managed EC protocol 2 appear below.
- Logs: `build/reference-tests.log`, `build/reference-tests.status`,
  `build/reference-verification.json`, `build/strict-port-native-tests.log`.

## Live application hardware integration — 2026-10-31, previous EC API

This receipt predates the service-managed EC protocol 2 change. It proves the
bounded fan/tach behavior of that earlier build, not a live pass for the new API.
The JSON/log have been preserved rather than overwritten by an offline result.

- `scripts/hardware-integration-test.sh build/hardware-integration.json` was
  executed against the active installed `n14-hardware.service` and
  `n14-sensors.service`; the script verified that installed service binaries
  matched the current build.
- The test used `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/app/n14-control`,
  acquired EC through the application D-Bus path, and performed real writes:
  `both=0/40/100`, `cpu=40`, `gpu=60`, `both=40`, and `keylight=0/50/100`.
  All 12 commands had matching raw EC readback and the final restore returned
  fan targets `42/42`, keylight raw `0`, manual mode `2`,
  `restored_to_initial=true`.
- The live run passed (`ok=true`) with a 5000 ms physical settle: CPU tach was
  raw/nominal `25/2500` at `0%` (target raw `1`), `29/2900` at `40%`
  (target raw `29`) and `44/4400` at `100%` (target raw `70`). The strict
  physical assertion `tach(100%) > tach(40%)` passed. GPU RPM was not claimed
  because the pinned EC transport exposes only GPU command target readback.
- During the run the sampler temporarily reported `runtime snapshot unavailable`
  five times;
  the opt-in integration harness performed only bounded observation/action retries
  for that transient provenance failure, with no retry or recovery write in the
  normal control loop. The final report records the retry count and still has
  `control_state_restored=true`.
- The first run exposed and the app-side harness fixed a status-publication/
  `ecBusy` ordering race. Equal tach at 40% and 100% remains a hard failure even
  when both raw command registers read back correctly.

## Service-managed EC protocol 2 — offline follow-up

- Removed public `AcquireEc`/`ReleaseEc` methods, their D-Bus policy entries,
  client methods, GUI ownership controls and harness acquisition/release phases.
- First provenance request performs asynchronous noninteractive Polkit and broker
  preflight; direct Apply still requires a matching validated observation.
  Successful authorization is session-bound, while broker generations change on
  revoke/restart. No old target is replayed on reacquisition.
- Service tests cover removed-method rejection/introspection, authorization deny
  and retry, concurrent callers, pending-authorization sleep/disconnect races,
  missing snapshot, sleep/resume, partial write and status-read failure.
- New `tests/test_hardware.cpp` exercises the real application backend against
  a fake D-Bus service on a private bus: initial command, ready/busy ordering,
  generation/Standard-state reset, no retry after failure, identity mismatch and
  legacy-service rejection. Subprocess checks also confirm that the live harness
  refuses initial automatic mode and a legacy service without sending commands.
  These tests have no hardware transport.
- The opt-in live harness now requires exact mode/target/keylight restoration.
  Initial automatic mode is rejected before writes because disconnect releases
  authority, not firmware mode. A mode-0-to-mode-2 handoff cannot make it green.
- This offline follow-up did not install/restart services or perform live EC
  writes. The client requires `Capabilities.ec_protocol=2`; the subsequent,
  separately authorized deployment and live validation are recorded below.
- Final full-suite log: `build/ec-lifecycle-final-tests.log` (exit `0`, **223 checks**).
  Targeted development logs: `build/ec-lifecycle-service-tests.log`,
  `build/ec-lifecycle-backend-tests.log`. Live-wrapper binary-mismatch refusal:
  `build/ec-lifecycle-hardware-preflight.log` (exit `2`, no EC commands).

## Service-managed EC protocol 2 — deployment and live validation, 2026-10-31

- After explicit user authorization, deployment ran through the desktop Polkit
  agent with the GUI closed and competing cooling controllers inactive. The
  previous binaries, D-Bus/Polkit policy and systemd units were backed up under
  `/var/lib/n14-control/deployment-backups/ec-protocol2-vbOU9c7y` (root-private).
  The one-shot helper `build/ec-protocol2-deploy.sh` then installed the matching
  service/configuration files and restarted both native services. Deployment
  itself issued no EC commands or CPU-setting changes; receipt:
  `build/ec-protocol2-deploy.log`, `.exit` (`0`).
- Both installed binaries and the D-Bus/Polkit policy match the built/source
  files. The running service reports `Capabilities.ec_protocol=2`; introspection
  exposes `GetEcProvenance` and `ApplyEc`, not `AcquireEc`/`ReleaseEc`.
  Read-only check: `build/ec-protocol2-postdeploy-check.log`.
- `scripts/hardware-integration-test.sh` ran with the new report path
  `build/hardware-integration-ec-protocol2.json`, from `11:19:53` to `11:21:05`
  (`+03:00`), without overriding the initial-mode gate. It exercised the actual
  application backend and installed privileged service; `ec_protocol=2`,
  process exit `0`, `ok=true`, `errors=[]`.
- All **12/12** raw readbacks matched: nine test commands (`both=0/40/100`,
  `cpu=40`, `gpu=60`, `both=40`, `keylight=0/50/100`) and three restore commands.
  With a 5000 ms settle, CPU tach evidence was:

  | Both-fan request | Target raw | CPU tach raw | CPU nominal RPM |
  | --- | ---: | ---: | ---: |
  | `0%` | `1` | `25` | `2500` |
  | `40%` | `29` | `29` | `2900` |
  | `100%` | `70` | `45` | `4500` |

  The strict assertion `tach(100%) > tach(40%)` passed. Initial and final control
  state matched exactly: CPU/GPU targets `42/42`, keylight raw `0`, manual mode
  `2`. All four restoration flags are true; `automatic_to_manual_handoff=false`.
- There were **6 bounded transient snapshot retries**, with
  `runtime snapshot unavailable: No such file or directory` recorded in the log
  and two error-level notices retained in the JSON. This is not a warning-free
  sampler run. These retries belong only to the opt-in harness; normal control
  still has no automatic retry or recovery write. The final `errors` array is
  empty and exact restoration succeeded.
- Evidence: `build/hardware-integration-ec-protocol2.json`, `.log`, `.exit`,
  `.started`, `.finished`. Independent assertions checked all readbacks, the
  physical tach inequality and exact control-state restoration:
  `build/ec-protocol2-receipt-verification.json`. SHA-256 checks confirm that
  the previous-API JSON/log were not overwritten:
  `build/ec-protocol2-previous-evidence.sha256.json`.
- A fresh `scripts/test.sh` after deployment passed **223 checks**, no failures
  or skips: `build/ec-protocol2-postdeploy-tests.log`, `.exit` (`0`). Both native
  services remained active with the deployment PIDs and `NRestarts=0`.
- This bounded protocol-2 receipt does **not** validate sustained thermal load,
  sleep/resume, service restart with an active EC owner, physical GPU RPM or
  physical keylight brightness. The deployment restart is not a lifecycle stress
  test. A later, separate active-owner restart receipt follows below. No full
  cooling/Standard parity or thermal safety claim is made.

## Active-owner service restart — 2026-10-31

- Added the standalone native `tests/lifecycle.pro` / `hardware_lifecycle_probe.cpp`
  probe, linked against the same `HardwareBackend` as the GUI. It is not installed
  into the privileged service and adds no service API or GUI ownership control.
  It requires initial manual mode `2` and an exactly representable current CPU
  target. It reasserts only that target, waits for readback, then keeps its unique
  D-Bus connection alive across one externally authorized service restart.
- Four new private-bus tests cover service-owner loss with a lower replacement
  broker generation, calculation-epoch/Standard reset without command replay,
  the probe's complete replacement-owner sequence, automatic-mode refusal and
  missing-snapshot refusal without retry. Full offline suite:
  `build/ec-restart-offline-tests.log`, `.exit` (`0`, **227 checks**).
  Invalid CLI paths and existing-receipt preservation were also checked.
  The full suite was repeated after the live run: `build/ec-restart-final-tests.log`,
  `.exit` (`0`, **227 checks**, no failures/skips).
- The explicitly invoked `scripts/hardware-lifecycle-restart.sh` companion runs
  separately through Polkit. It monitors only calls to the hardware interface,
  verifies an actual read-only `Capabilities` handshake in the audit, waits for
  the probe-ready marker, and restarts only `n14-hardware.service`. It does not
  issue CPU/EC commands, restart the sampler, alter policies or deploy binaries.
- The first audit setup attempt stopped before starting the probe or restarting
  either service: JSON-mode `busctl` does not print the expected text banner.
  That failure is preserved as `build/hardware-lifecycle-ec-protocol2.audit.log`,
  `.audit.exit` / `.overall.exit` (`1`); no EC commands were issued. The readiness
  handshake fix was checked on a private bus in
  `build/ec-restart-audit-handshake.log`, then used with a new receipt name.
- The live run from `12:23:31` to `12:23:38` (`+03:00`) passed: probe and companion
  exits `0`, `ok=true`, `errors=[]`, **0 retries / 0 recovery writes**. The sole
  explicit command was `cpu=59%`, read back raw `42`, preserving the initial
  target. Hardware service PID changed `32796 → 49626`, D-Bus owner
  `:1.119 → :1.154`; the same client `:1.152` survived. Sampler PID remained
  `32782`. These process/connection values are evidence for this run only.
- The backend became EC-inactive on owner loss, stayed inactive during the
  post-registration quiet window, and reacquired provenance only when explicitly
  requested. Audit timestamps show **1503.077 ms** before that fresh observation.
  Initial/final mode `2`, CPU/GPU targets `42/42` and keylight raw `0` were exactly
  unchanged. The final CPU tach readback was raw `42` (nominal `4200 RPM`).
- Independent audit assertions found **5 Capabilities, 4 GetEcProvenance and
  1 ApplyEc** calls. The only Apply was before owner loss; **0 ApplyEc** calls
  occurred after restart, and no ApplyCpu/AcquireEc/ReleaseEc appeared. This is
  evidence against automatic D-Bus command replay, not an independent trace of
  every ACPI packet or proof of behavior during a partial transport failure.
- Successful receipt prefix: `build/hardware-lifecycle-ec-protocol2-run2`:
  `.json`, `.json.ready`, `.log`, `.audit.log`, `.context.json`, `.verification.json`,
  `.exit`, `.audit.exit`, `.overall.exit`, `.started`, `.finished`. SHA-256 checks
  confirm that the application/service binaries and both earlier fan-test
  JSON/log pairs were unchanged. Both services remain active.
- Broker generation counters are process-local, not a globally increasing restart
  identifier. The replacement's unique D-Bus owner and the backend owner-change
  reset are part of the boundary; the fake replacement test explicitly permits
  a generation reset from `9` to `1` without reusing calculation history.
- No **successful** sustained thermal-load or sleep/resume gate is recorded. This
  single clean restart does not validate restart during an in-flight/partial
  write, repeated restart races, physical GPU RPM or physical keylight brightness.

## Bounded physical thermal/load attempt — 2026-10-31

- Added `tests/load.pro` and `tests/hardware_load_probe.cpp`. The probe requires
  `--physical` for real `/sys`, uses explicit duration/worker/duty/temperature
  limits, stops only its own CPU workers, performs no retries or recovery writes,
  and has a separate explicit `--restore-report` path for a fresh-provenance
  restoration after an uncertain transaction.
- Offline private-bus coverage includes normal bounded load/restore, hot-CPU
  refusal before EC access, sleep signal/late-reply invalidation and a late
  post-command observation. Post-run hardening adds failure without compensation,
  independent worker deadlines, strict restore-source validation and per-command
  readbacks, restore-failure termination and in-flight replies across a replacement
  D-Bus owner. Final offline counts are recorded below. These later client/probe
  revisions have not been rerun against physical hardware; installed service and
  sampler binaries were unchanged throughout.
- The explicitly bounded physical attempt used external power, initial CPU
  temperature `66 C`, CPU critical `100 C`, stop limit `88 C`, four workers at
  75% duty and a requested 30 s. It performed one confirmed Standard command
  (`both=35%`, raw `25`), then at `2.271 s` received an EC transaction failure;
  the service revoked authority and the probe stopped its workers. Exit `1`,
  `automatic_retries=0`, `recovery_writes=0`; no thermal pass is claimed.
- The failed receipt is
  `build/hardware-load-ec-protocol2-20261031T1256+0300.json` with log/exit and
  preflight JSON. The retained, independently reobserved control state was raw `25/25`, mode `2`,
  keylight `0`, so the harness did not guess a recovery write. A separately
  invoked restore probe acquired fresh provenance and applied CPU/GPU `42/42`;
  `build/hardware-load-ec-protocol2-20261031T1256+0300-restore.json` is
  `ok=true`, two explicit completions, exact mode/keylight/target restoration.
- The installed service masks the transaction rejection reason. The record does
  not distinguish transport/readback failure from provenance/freshness rejection
  or establish which packets, if any, the rejected transaction wrote. No
  independent D-Bus/ACPI audit was captured for this run; the probe's retry/write
  counters are not an independent packet trace. This is not evidence of safe
  sustained Standard control. Diagnose the rejection before another load run;
  do not run it as a retry.
- Two unintended one-second physical probes were invoked during CLI checks
  (`12:48:57` and `12:53:18`, `+03:00`), the second using the old executable while
  the opt-in guard was being rebuilt. Both receipts report successful exact
  restoration, but neither has an independent audit. Evidence was copied without
  changing the originals into `build/hardware-load-development-smoke-{1,2}.*`;
  the SHA-256 preservation manifest is
  `build/hardware-load-development-smoke-preservation.json`. The final binary
  refuses a real `/sys` run without `--physical` before any D-Bus request; a
  dedicated regression test covers this refusal. These smoke runs are not
  sustained thermal validation.

## EC rejection diagnostics — offline follow-up, 2026-10-31

- Located the masking boundary: the transaction queue already returned its
  rejection reason, but the service replaced it with a generic D-Bus failure.
  The development build now returns compact `diagnostics` JSON in that failure
  and emits the same JSON with journal marker `N14_EC_TRANSACTION_REJECTED`.
  No IPC methods, Polkit permissions or ownership responsibilities were added.
- Diagnostics distinguish validation, conversion, initial observation, write,
  readback and settling failures. They include the fixed register, expected and
  observed raw values (`-1` when unavailable), write-attempt count, sample age
  on both clocks, sample ID, calculation epoch and broker-owner generation.
  Reason text is bounded to 512 characters and normalized onto one line.
- A write attempt is counted **before** calling the transport: an exception can
  happen after firmware has accepted a write. `hardware_state_uncertain=true`
  conservatively marks an unsuccessful transaction with at least one attempt;
  `false` only means this transaction attempted no writes, not that earlier
  hardware state is known. Counts are not ACPI packet-trace evidence.
  Validation policy, packet encoding, operation ordering, settling delay,
  terminal revocation and the prohibition on retries/compensation are unchanged.
- Added **38** fake-transport failure cases covering every fan/keylight read and
  write boundary, mismatched readbacks, exceptions after completed fake writes,
  settling and final keylight readback. Added **5** private-bus cases that verify
  the caller receives distinct owner/freshness, write, readback-transport and
  readback-mismatch reasons. Every case verifies terminal failure without retry
  or compensation. Existing stale/replay cases also assert zero write attempts.
- Full offline suite: **190 core + 31 D-Bus service + 35 backend/harness + 21 GUI
  + 6 native-sampler = 283**, plus **7 source-lock tests = 290 passed**,
  no failures/skips. Shell syntax checks passed; no compiler warnings were found.
  Evidence prefix:
  `/home/yaroslav/.cline/data/workspaces/chat/n14-control/build/ec-rejection-diagnostics-20261031T132229+0300`
  (`-tests.log`, `-tests.exit`, `-verification.json`).
- A service-owned `GetEcProvenance` observation confirmed raw targets `42/42`,
  mode `2`, keylight `0`. No `ApplyEc`, load probe, suspend, deployment or service
  restart was performed. Installed hardware/sampler services remain at PIDs
  `49626`/`32782`, with unchanged binary hashes. The newly built hardware-service
  binary deliberately differs from the still-installed binary.
- **The old physical failure remains undiagnosed.** This change cannot recover
  diagnostics that the previous service discarded. Next physical work requires
  separate explicit approval to deploy the diagnostic service and perform a
  bounded, independently audited diagnostic gate. It must not silently repeat
  the failed thermal run or restore an uncertain result. Sustained thermal and
  physical sleep/resume validation remain incomplete.

## Version 0.2 validation scope

- Previously recorded `scripts/test.sh` with the active-owner restart, sleep and load-probe
  regression coverage:
  **152 core + 26 D-Bus service + 35 application-backend/harness + 21 GUI + 6
  native-sampler = 240 passed**, plus **7 source-lock extraction tests**, giving
  **247 checks**, no failures/skips. Final log:
  `build/thermal-sleep-hardened-final-tests.log`, `.exit` (`0`).
- Runtime gate tests reject stale/future/replayed samples, generation rollback,
  invalid paired metadata and fabricated suspended-GPU temperatures; accepted
  samples feed only dry-run Graph/Standard calculations. Provider tests cover missing,
  symlink, traversal, non-regular, owner/mode, oversize, truncated, atomic
  replacement and replayed files. The standalone dry-run loop covers timeout,
  lifecycle and producer-session restart. Native sampler tests additionally cover
  paired EC observations, active/D0 versus suspended/D3cold transitions, mode
  generations, failed GPU observation and atomic private publication. The root
  service rechecks live GPU power state immediately before provenance response and
  before transaction submission; injected service tests cover the same gate without
  hardware. The service owns broker startup; the client has no public ownership
  lifecycle calls.
- `reference/EC_WRITER_DESIGN.md` and `reference_ownership.{h,cpp}` define and
  test injected-facts preflight, a metadata-only read-only probe with injected
  filesystem/process interfaces, an injected lock backend, a temporary-file
  RAII ownership lease, and an in-memory fake transport. The probe does not
  acquire the lock or open an ACPI/device transport; the standalone lease never
  creates a lock file. The fake-only transaction queue covers provenance fences,
  serialized fan/keylight ordering, readback and terminal partial-failure
  behavior. `reference_broker` and the root service now implement the production
  contract. Separate bounded native EC receipts for the previous API and protocol
  2 are recorded above; the separately approved legacy diagnostic probe below
  remains historical evidence, not evidence for protocol 2.
- `n14-control --ownership-preflight` now emits a schema-1 JSON report and is
  checked by `scripts/test.sh`. Its expected exit code is `2`: the report is
  successful read-only collection, while control remains denied because no lock
  is acquired and no pinned injected EC identity is present. Generic DMI
  `ec_firmware_release` is retained only as diagnostic metadata; it does not
  populate authoritative EC identity.
- `reference/N14_HARDWARE_VALIDATION.md` records the target-specific procedure,
  completed limited diagnostic readback and bounded write/readback subset. Full
  controlled protocol validation, production identity pinning, production writer,
  uncontrolled target writes and systemd installation remain explicit later gates.
- Root-authorized read-only Phase A was run through `pkexec` and recorded in
  `build/pkexec-read-only-preflight.json`. It acquired no lock and performed no
  hardware writes. The probe initially exposed a short-read bug for sysfs ACPI
  files: a single 4096-byte read produced a false DSDT hash. The bounded read
  loop now matches direct `sha256sum` and the pinned DSDT hash
  `4aab3c89fdaac8ab9e20d31cff3fe0a18b83877442520eb294429ff9237bef06`.
  The initial root report detected active owners `adapter_server.py`,
  `colorful-n14-coolercontrold`, `colorful-n14-custom-device` and
  `profile_selector_service.py`, therefore correctly denied. After the
  separately approved controlled stop, the post-probe report has an empty
  `competing_owners` list, but the result remains fail-closed because no broker
  lock or pinned production EC identity is present.
- The pinned reference `prepare.py check` was also run through `pkexec` with
  `python3 -B` and passed in read-only mode; its evidence is in
  `build/reference-prepare-check.log`. It confirmed the same DMI/BIOS/SMBIOS
  EC/DSDT values and only reported that `/proc/acpi/call` is present.
- An initial `prepare.py probe` attempt was made only after the read-only checks
  and stopped before any ACPI call because PID `857` already held
  `/run/colorful-n14-control/session.lock`. No process was killed and no lock or
  runtime artifact was removed during that attempt.
- After explicit approval, only the five documented cooling/control units were
  stopped through systemd. `prepare.py probe --acknowledge-risk` then completed
  four diagnostic indexed reads: controller `55:71`, CPU tachometer `3500 RPM`
  nominal and GPU command `36/70` (`51.43%`). GPU physical RPM was not verified.
  No `--launch` option was used; NBFC, fan-target writes, keylight writes and
  production broker were not started. The probe log and post-stop evidence are
  in `build/diagnostic-probe-20261030T193907Z.log` and
  `build/post-probe-evidence-20261030T193907Z.txt`.
- The probe wrote only a read-mode diagnostic permission artifact and did not
  remove `/run/colorful-n14-control/session.lock`. Services remain stopped,
  `session.lock` is unheld, and the post-probe metadata-only preflight still
  returns the expected deny (`exit 2`). The readback `55:71` is diagnostic
  evidence only; it was not promoted to production pinned identity.
- After a separate thermal gate (`CPU 53°C`, `GPU 44°C`), an explicitly approved
  bounded write/readback test used the current guarded transport, not the stale
  direct fan harness. Fan targets `28/28` and `70/70` both read back correctly;
  CPU tach raw values were `27` and `44`, and the original `36/36` targets were
  restored. Raw `0` was deliberately not used.
- The same bounded adapter transaction wrote keylight raw `88` (`50%`), verified
  the `50%` readback, and restored the original raw `0` (`100%`). The adapter
  correctly refused to restore manual fan mode without an authenticated
  producer/provenance assignment, leaving firmware in automatic mode `0` with
  targets `36/36` and issuing no recovery fan write. Journal evidence records
  `stale/invalid fan targets; no fan override` and `handover expired or invalid`.
  This is protocol/readback evidence, not physical keyboard-light or GPU-RPM
  validation. The write-test logs are under `build/controlled-protocol-*.log`.
- The adapter and sensor units were then stopped, the permission was returned to
  `read` by a post-write diagnostic probe, and the final metadata-only preflight
  remained fail-closed (`exit 2`) with no competing owners. No production broker,
  CoolerControl daemon/plugin, NBFC writer, sleep-cycle test or identity-policy
  update was performed.
- Settings schema 3 round-trips native CPU/GPU Standard independently from the
  legacy preview filters and migrates schema 1/2 with native defaults.
- Standard oracle inputs are read only from the local files whose SHA-256 values
  are recorded in `reference/standard-source-lock.json`; regeneration is offline
  and uses fake status histories. No live CoolerControl API, service restart or
  EC/ACPI write is part of this validation.
- Offscreen smoke test renders all six tabs; the private-authority integration
  test confirms the real pkcheck subprocess/reply path against fake hardware only.
- Added preset/schema migration, per-policy CPU-bound checks, fake-sysfs writes,
  rollback after partial failure and mismatch, and explicit rollback-failure tests.
- GUI tests cover preset editing without application, duplication/deletion,
  automation exclusivity, simulated sequential application, and sleep callbacks.
- D-Bus tests run in `dbus-run-session` with the system-bus address redirected to
  that PRIVATE bus. They use a temporary fake sysfs tree, not the machine's CPU.
  They cover malformed requests before authorization, missing authorization,
  non-export of the private sleep handler, automatic EC authorization, caller
  isolation, provenance requirements and the sleep gate. A private mock Polkit
  authority exercises the successful
  authorization/CPU-application branch; no test authorizes a live EC write.
- Real CPU detection is read-only: i7-13620H, 16 intel_pstate policies, P-policy
  maximum 4700 MHz, E-policy maximum 3600 MHz, Turbo Boost allowed.
- RCU Lazy is `Y`; its attribute is read-only (0444). No bootloader edits or
  speculative runtime writes were attempted.
- Systemd unit is verified using a generated local test copy whose ExecStart
  points to the built binary. Production unit retains the root-owned install path.
  Shell syntax and D-Bus/Polkit XML are checked separately.
- In the earlier offline validation, passwordless sudo was unavailable and the
  service was not installed. The separately authorized Polkit deployment and
  bounded native EC write windows above supersede that installation status;
  actual CPU writes and broad systemd sandbox behavior remain unvalidated on
  physical hardware.
- That earlier offline run did not start a native EC writer, alter NVIDIA
  runtime-PM/GPU modes, restart cooling services or install packages. The later
  native live receipts above must not be attributed to those offline logs.

Current result logs: `build/stage2-tests.log`, `build/stage2-tests.status`.
Release recheck: `build/release-02-tests.log`, `build/release-02.status`,
`build/release-02-integrity.log`, `build/release-02-smoke.log`.
Provider/loop/ownership follow-up: `build/ownership-final-validation.log`.
Read-only probe recheck: `build/read-only-probe-full-validation.log`.
Offline lease/transaction recheck: `build/transaction-full-validation.log`.
Combined continuation check: `build/continued-validation.log`.
The older results below describe the 0.1 baseline, not a new 0.2 performance claim.

## Version 0.1 baseline (historical)

- Native release binary built with qmake6 / GCC, `-O2`, no new system packages.
- Core QtTest suite: **47 passed**, 0 failed, 0 skipped.
- GUI QtTest suite: **8 passed**, 0 failed, 0 skipped; all hardware actions use
  the explicitly simulated backend. Offscreen plugin size-hint warnings are expected.
- Offscreen smoke test renders all four tabs and captures PNGs.
- Portable package passes the same smoke test with an isolated HOME, software
  rendering and bundled Qt libraries. `ldd` confirms Qt resolves inside the bundle.
- Read-only real D-Bus/KScreen probe finds `performance`, `eDP-1`, 165 Hz and
  both 60/165 Hz modes at current 2560×1600 resolution.
- Real telemetry is read without root/NVML/nvidia-smi. On AC, the battery reports
  Not charging/0 W or a tiny discharge; the UI does not equate this with laptop power.
- Actual GUI started from the portable package. At startup no user configuration
  was created and both automation flags remained off.
- One 15-second live GUI sample: **0.20% of one logical CPU**, RSS **70,568 KiB**
  (~69 MiB), PSS **59,233 KiB** (~58 MiB). This is a short measurement, not an
  upper bound or an idle/battery-life guarantee. Curve-preview tab was not active.
- Existing cooling services retained PIDs 854/856/864 with zero restarts;
  original `colorful-n14-coolercontrol` source checkout remained unchanged.

## Version 0.1 unvalidated scope (historical)

- Real profile/rate switching and unplug/replug automation: implementation tested
  with fake inputs/demo, but actual settings deliberately not changed during build.
- Idle D3cold preservation during this run: GPU was already active (D0) during
  the real probe. Passive access selection alone is not proof of no wakeups.
- Battery runtime improvement, multi-battery accuracy or long unattended operation.
- Direct EC fan control, runtime use of the exact Standard reference, keyboard
  lighting, privileged optimization toggles and MUX switching remain outside this
  validation; the isolated Standard algorithm itself is covered by the golden suite.

## Current remaining hardware gates

- Sustained CPU/GPU thermal-load control and unattended Standard operation.
- Physical sleep/resume and restart during in-flight/partial operations; one clean
  active-owner restart is recorded above, not a general lifecycle stress pass.
- GPU physical RPM, keyboard-light brightness observation, battery/unplug cycles,
  real CPU/profile/display policy application, privileged optimizations and MUX.

Logs and screenshots remain under the project's ignored `build/` directory.
Generated binaries and archive remain under `dist/`; these are local development
artifacts for this distribution, not a universal/static or public release.