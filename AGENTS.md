# N14 Control

Native C++20 / Qt 6 Widgets application for the COLORFUL N14 24 on KDE Wayland.
Build with `./scripts/build.sh`; test with `./scripts/test.sh`.

- No firmware/EC writes in milestones 1–2. Do not stop the existing cooling stack.
- CPU writes belong only to the separately installed, Polkit-authorized service.
- Preflight every CPU policy, clamp to hardware bounds, verify writes and report rollback failures.
- Never add arbitrary commands/paths to the hardware service API or expose its private sleep hooks.
- Never use NVML, nvidia-smi or GPU temperature polling for idle monitoring.
- Automation is opt-in; unknown AC state must never select a power policy.
- Keep hardware-independent algorithms covered by QtTest with fake sysfs trees.
- All subprocesses use fixed executable paths and argument lists, never a shell.
- Do not claim fan-control/Standard-function parity until migration is validated.
- Strict porting contract: reference/PORTING.md and reference/source-lock.json.
- Existing cooling/lighting/error behavior must match the pinned published GitHub
  commit, not local-only commits or invented fallback thresholds. Do not silently
  replace Standard with preview filters. Preserve and disclose original risks.