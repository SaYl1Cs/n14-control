"""Probe safety tests use only fake sensors and mocked session-bus replies."""
import contextlib
import importlib.util
import io
import json
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "fan-curve-physical-test.py"
SPEC = importlib.util.spec_from_file_location("fan_probe", SCRIPT)
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


class ProbeTests(unittest.TestCase):
    def test_fresh_cached_status_is_read_only(self):
        state = {"failed": False, "mode": "curve", "ready": True,
                 "status": {"sampled_at_unix_ms": 1000}}
        reply = types.SimpleNamespace(stdout=json.dumps({"data": [json.dumps(state)]}))
        with patch.object(probe.subprocess, "run", return_value=reply) as run, \
                patch.object(probe.time, "time", return_value=1):
            self.assertEqual(probe.controller_status(), state)
            self.assertEqual(run.call_args.args[0][-1], "Status")

    def test_stale_cached_status_is_not_a_pass(self):
        state = {"failed": False, "mode": "curve", "ready": True,
                 "status": {"sampled_at_unix_ms": 1000}}
        reply = types.SimpleNamespace(stdout=json.dumps({"data": [json.dumps(state)]}))
        with patch.object(probe.subprocess, "run", return_value=reply), \
                patch.object(probe.time, "time", return_value=4):
            with self.assertRaisesRegex(RuntimeError, "not fresh"):
                probe.controller_status()

    def test_guard_records_actual_temperature_before_starting_load(self):
        original_glob, original_read = Path.glob, Path.read_text
        contents = {"/fake-cpu/name": "coretemp", "/fake-cpu/temp1_crit": "100000",
                    "/fake-cpu/temp1_input": "91000", "/fake-ac/type": "Mains",
                    "/fake-ac/online": "1"}

        def glob(path, pattern):
            if str(path) == "/sys/class/hwmon":
                return iter([Path("/fake-cpu")])
            if str(path) == "/sys/class/power_supply":
                return iter([Path("/fake-ac")])
            return original_glob(path, pattern)

        def read(path, *args, **kwargs):
            if str(path) in contents:
                return contents[str(path)]
            return original_read(path, *args, **kwargs)

        with tempfile.TemporaryDirectory() as directory:
            receipt = Path(directory) / "guard.json"
            with patch.object(Path, "glob", glob), patch.object(Path, "read_text", read), \
                    patch("sys.argv", [str(SCRIPT), "--physical", str(receipt)]), \
                    patch.object(probe.multiprocessing, "Process") as worker, \
                    patch.object(probe.subprocess, "run") as bus, \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(probe.main(), 1)
            report = json.loads(receipt.read_text())
            self.assertFalse(report["load_started"])
            self.assertEqual(report["max_cpu_sysfs_c"], 91)
            self.assertEqual(report["guard_stop"]["cpu_sysfs_c"], 91)
            worker.assert_not_called()
            bus.assert_not_called()


if __name__ == "__main__":
    unittest.main()