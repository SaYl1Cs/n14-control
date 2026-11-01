# Coordinated EC audit — 2026-10-31

## Observed faults, not a hardware-parity claim

**Follow-up:** physical one-command lighting lag remains reproduced after the
GUI fixes, post-settle mode confirmation and exact pinned register-order repair.
The user saw full brightness at the end of `0 -> 100 -> 0` with correct final
raw `175`; a single explicit repeat of 0% extinguished it. Do not use that
repeat as a workaround or call raw-readback tests a physical pass. The user
rejected post-write delay. See the newest `VALIDATION.md` section and its
separate failed physical receipts. Production timing observation adds no EC
access/wait and is not a claim of repair.

1. The keyboard slider used `sliderReleased` with tracking disabled. Qt commits
   `value()` after that signal, so the command used the previous brightness.
   Keyboard/wheel edits did not send a command at all. `valueChanged` now sends
   the committed value; readback updates use `QSignalBlocker`.
2. The launcher preferred an older `dist` binary over the local rebuild. It now
   prefers the rebuilt application, avoiding a stale direct-EC GUI.
3. A busy fan operation rejected a lighting request. The controller now holds
   at most one latest lighting intent, obtains fresh provenance when dispatching
   it, and completes post-command observation before dispatching another one.
4. A clean GPU/sampler epoch change retained the old broker owner generation.
   The transaction queue correctly rejected the next command before any write:
   `transaction sampler changed without a new broker owner generation`.
5. An ordinary GPU PM transition between the sampler and the broker's live PM
   read was treated like a terminal EC failure. After the successful 50% light
   transaction, fan commands continued; 14 seconds later a PM observation mismatch
   stopped the controller. Thus the light transaction itself did not fail.

The original failure receipts are retained under the project's build directory.
CPU sysfs reached 100 C while the old controller was latched at raw 19/19.
The user explicitly authorized one curve re-enable; no automatic recovery write
was issued. A later PM mismatch latched the replacement controller as well.
Do not interpret an `active` systemd process as an operational curve: inspect
`failed`, `mode`, snapshot clocks and confirmed command progress.

## Source contract

The immutable integration reference remains commit
`4967a3e73105014fb640150587e538c6ca594941`:

- [keylight transaction, conversions, sensor grace and cool hold gates](https://github.com/SaYl1Cs/colorful-n14-coolercontrol/blob/4967a3e73105014fb640150587e538c6ca594941/coolercontrol_adapter.py)
- [snapshot freshness / SENSOR_GRACE = 5 seconds](https://github.com/SaYl1Cs/colorful-n14-coolercontrol/blob/4967a3e73105014fb640150587e538c6ca594941/thermal_state.py)
- [source keylight/lid tests](https://github.com/SaYl1Cs/colorful-n14-coolercontrol/blob/4967a3e73105014fb640150587e538c6ca594941/tests/test_keylight.py)

No register, conversion or 20 ms prewrite settle was replaced. The follow-up
restores source-order final readbacks rather than the extra interleaved reads;
readback/partial-write failure remains terminal. The earlier user-confirmed
50% physical change (`raw=88`) is insufficient to prove every transition or
exclude another firmware issue. Native lid-refresh or automatic post-sleep
brightness restoration is still not implemented or claimed here.

## Roles and checks

| Component | Responsibility | Shared-boundary checks |
| --- | --- | --- |
| GUI / `FanClient` | Session-bus intent and cached state only | No EC provenance calls/writes, async D-Bus, 120 ms one-shot coalescing, no rollback from stale cached slider value |
| `n14-fan-controller` | Single hardware client, 1 Hz saved curves + manual/light intents | Latest-value light queue, fan cadence priority, post-command readback, no duplicate instance |
| `n14-hardware.service` | Only indexed ACPI/EC writer | Polkit/caller identity, pinned machine identity, lifetime lock, exact register whitelist, serialized transactions, live GPU PM check |
| `n14-sensors.service` | Native read-only EC temperature producer | Atomic private `/run/n14-control` snapshot, clocks/session/generation/paired measurements; no NVML or GPU wake request |
| Legacy `colorful-n14-sensors` | Read-only producer still installed | Separate `/run/colorful-n14-control` snapshot; not mistaken for a second control writer |
| CoolerControl / NBFC / legacy adapter | Potential competing writers | Must remain inactive during native control/deployment |
| Physical probes | Separate opt-in test clients | Never run concurrently with the live controller |

Process/unit observations found one native writer and one controller. This is
not a privileged packet audit or proof against an arbitrary unrelated writer.

## Clean transitions vs actual failures

A fully accepted new producer epoch obtains a new broker generation BEFORE
returning provenance. Old tokens and Standard state cannot cross that boundary.
This contains no recovery write, retry of a failed transaction or firmware-mode
restoration. The transaction queue's epoch/replay checks are unchanged.

The protocol adds one typed no-write response,
`org.n14control.Error.EcSamplePending`. It is used only for a known native GPU
observation gap or a live PM mismatch before any EC write is submitted.
The exact native producer errors accepted here are unconfirmed GPU power,
power changing during the PM observation or EC sample, and an EC temperature
byte outside 1..119 C. The last case discards an invalid sensor value; it does
not declare a real temperature or prove a PM transition. The envelope must have
exactly one matching GPU error, no GPU measurement/sample/clock, valid CPU data
and validated paired metadata. GPU transport open/read failures are not eligible.
It requires the last accepted observation's ORIGINAL wall/boottime age to be
strictly below 5 seconds. An observation gap
invalidates the current Apply token. Waiting never renews the old sample clocks.

**Explicit policy difference requested by the user:** the initial port retained
the source's `cool_for_hold()` CPU <80 C / active GPU <70 C eligibility. A live
post-update transition at CPU 92 C then stopped the curve despite confirmed
raw 70/70. The user instructed continued work irrespective of high temperature;
the observation-only wait therefore no longer has those temperature cutoffs.
The original 5-second age bound, live-PM recheck and prohibition of ALL writes
during waiting remain. This is not exact source hold-policy parity, a new
thermal threshold, an emergency target or permission to write from stale data.

The controller exposes `waiting` separately from `failed`, performs only the
ordinary 1 Hz observation while waiting, and calculates a NEW curve demand on
fresh data. Standard state advanced by an unsubmitted demand is discarded, even
if the producer epoch did not change. One latest light intent may remain queued;
sleep/owner change/real failure clears it. Manual fan commands are not replayed.

Generic busy/missing/stale/schema/permission/readback/transport failures are NOT
converted into this response. Expired original-age eligibility is terminal. A
partial write remains terminal with no retry, compensation, hidden 100%/50%/0%
or firmware automatic-mode restoration. The original last-target thermal risk
therefore remains; short observation waiting is not a thermal safety guarantee.

The 2026-11-01 14:44:34 live stop and subsequent source audit exposed two missing
native error variants in the original allowlist. Private-bus regressions reproduce both terminal
post-command stops before the fix and verify fresh-data continuation after it.
They also verify old-token invalidation, no grace without an accepted sample,
non-renewing original-age expiry, and rejection of CPU/transport/malformed data.

## Joint validation

The private-bus test `wholeEcStackSerializesLightingGpuTransitionsSleepAndTerminalFailure`
runs FanClient -> FanController -> HardwareBackend -> actual HardwareService ->
actual transaction queue -> fake EC registers. Only the synthetic DSDT identity
receipt is normalized in a test-only bridge; commands, provenance, errors and
readbacks are not rewritten. No test bridge is compiled into production apps.
It exercises three PM transitions interleaved with coalesced light edits, clean
sleep/resume, and a terminal keylight write failure stopping both lighting and
the curve. Additional tests cover repeated epochs, pre-write races, stale-token
rejection, strict original-age expiry at cool/high temperatures, wrong raw readback,
late replies, duplicate processes and ordinary GUI input paths.

The update requires broker and controller deployment TOGETHER. The old broker
cannot provide typed transition waiting or correct new-epoch generations.
`scripts/update-ec-stack.sh --user LOGIN` is an explicitly authorized coordinated
update, checks the release receipt/hashes, refuses live GUI/write probes and
other writers, and does not restart the sampler or change Polkit policy. It
stops the client once, atomically replaces both binaries, restarts the broker,
then starts the client with fresh provenance. A deployment error is not retried
and must be inspected; do not assume stopping the client restores cooling.

## Final execution evidence

`build/ec-stack-final-tests.log` and exit receipt report **323 Qt + 10 Python**,
zero failed/skipped checks. Includes the joint stack test with CPU **92 C**;
its no-write GPU waits do not terminal-fail solely because of temperature.

Final broker/controller deployment succeeded and the running executable hashes
match their builds. `build/ec-stack-root-audit.json` verifies the broker's one
flock, fresh native snapshot and inactive competing writer units, without an EC
write. The 60-second live receipt
`build/ec-stack-live-20261031T160156.json` passed with 60 additional curve cycles
and a 50% light command (`raw=88`, queue cleared). CPU use for the three native
background processes totaled approximately 0.35% of one core; RSS remained
stable. Exact counters and per-process numbers are in `VALIDATION.md`.

GPU remained D3cold in that physical observation: do not claim it newly proves
hardware GPU transitions or physical sleep/resume. Physical slider interaction
also still needs user confirmation, independently of the passing Qt input tests
and the earlier user-confirmed physical brightness change.