#!/usr/bin/env python3
"""Opt-in bounded CPU load; observes the running single-owner controller only.

Never sends EC commands, changes the curve, restarts the controller or restores
a target. A failed observation stops load and records failure, without retries.
"""
import argparse
import json
import multiprocessing
import subprocess
import time
from pathlib import Path


def controller_status():
    result = subprocess.run(
        ["/usr/bin/busctl", "--system", "--timeout=3", "--json=short", "call",
         "org.n14control.Fans1", "/org/n14control/Fans1",
         "org.n14control.Fans1", "Status"],
        check=True, capture_output=True, text=True, timeout=4,
    )
    state = json.loads(json.loads(result.stdout)["data"][0])
    if state["failed"] or state["mode"] != "curve" or not state["ready"]:
        raise RuntimeError("Running controller is not in healthy curve mode: " + str(state))
    age_ms = time.time() * 1000 - state["status"]["sampled_at_unix_ms"]
    if not 0 <= age_ms <= 2500:
        raise RuntimeError(f"Controller snapshot is not fresh: age_ms={age_ms}")
    return state


def bounded_load(stop, seconds):
    # One worker at ~10% duty. Its independent deadline also stops it if the
    # observer stalls or disappears. No high-priority or GPU workload.
    deadline = time.monotonic() + seconds
    value = 1
    while not stop.is_set() and time.monotonic() < deadline:
        cycle = time.monotonic()
        while not stop.is_set() and time.monotonic() < min(deadline, cycle + 0.0025):
            value = (value * 1664525 + 1013904223) & 0xffffffff
        stop.wait(max(0.0, min(deadline, cycle + 0.025) - time.monotonic()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--physical", action="store_true", required=True)
    parser.add_argument("receipt", type=Path)
    args = parser.parse_args()
    if not args.receipt.is_absolute() or args.receipt.exists() or not args.receipt.parent.is_dir():
        parser.error("receipt must be a new absolute path in an existing directory")
    report = {"schema": 1, "test": "bounded load of running headless curve controller",
              "ok": False, "errors": [], "samples": [], "automatic_retries": 0,
              "recovery_writes": 0, "ec_commands_sent_by_probe": 0,
              "audit_scope": "cached controller status and CPU sysfs; no privileged D-Bus/ACPI trace",
              "workers": 1, "worker_duty_percent": 10, "requested_seconds": 8,
              "test_stop_cpu_c": 90, "load_started": False}
    stop = multiprocessing.Event()
    worker = None
    try:
        sensors = [path for path in Path("/sys/class/hwmon").glob("hwmon*")
                   if (path / "name").read_text().strip() in ("coretemp", "k10temp")]
        if not sensors:
            raise RuntimeError("No supported CPU temperature sensor")
        sensor = sensors[0]
        critical = float((sensor / "temp1_crit").read_text()) / 1000
        report["hardware_critical_cpu_c"] = critical
        if critical <= report["test_stop_cpu_c"]:
            raise RuntimeError("Test stop temperature must be below CPU critical")
        start = time.monotonic()
        while time.monotonic() - start < 18:
            elapsed = time.monotonic() - start
            cpu = float((sensor / "temp1_input").read_text()) / 1000
            supplies = list(Path("/sys/class/power_supply").glob("*"))
            ac = any((p / "type").read_text().strip() == "Mains"
                     and (p / "online").read_text().strip() == "1" for p in supplies)
            if not ac or cpu >= report["test_stop_cpu_c"]:
                report["guard_stop"] = {"cpu_sysfs_c": cpu, "external_power": ac,
                                        "elapsed_seconds": round(elapsed, 3)}
                raise RuntimeError(f"Test guard stopped load: ac={ac}, cpu_c={cpu}")
            state = controller_status()
            report["samples"].append({"elapsed_seconds": round(elapsed, 3),
                                      "cpu_sysfs_c": cpu, "controller": state})
            if elapsed >= 4 and worker is None:
                worker = multiprocessing.Process(target=bounded_load, args=(stop, 8))
                worker.start()
                report["load_started"] = True
            time.sleep(0.25)
        report["ok"] = True
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        report["errors"].append(str(error))
    finally:
        stop.set()
        if worker is not None:
            worker.join(timeout=2)
            if worker.is_alive():
                worker.terminate()
                worker.join(timeout=2)
        report["load_stopped"] = worker is None or not worker.is_alive()
        if report["samples"]:
            states = [s["controller"]["status"] for s in report["samples"]]
            report["cpu_target_raw_range"] = [min(s["cpu_target_raw"] for s in states),
                                               max(s["cpu_target_raw"] for s in states)]
            report["cpu_tach_raw_range"] = [min(s["cpu_tach_raw"] for s in states),
                                             max(s["cpu_tach_raw"] for s in states)]
            report["final_controller"] = report["samples"][-1]["controller"]
        temperatures = [s["cpu_sysfs_c"] for s in report["samples"]]
        if "guard_stop" in report:
            temperatures.append(report["guard_stop"]["cpu_sysfs_c"])
        if temperatures:
            report["max_cpu_sysfs_c"] = max(temperatures)
        with args.receipt.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
        print(json.dumps({key: value for key, value in report.items()
                          if key not in ("samples", "final_controller")}, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())