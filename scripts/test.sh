#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python3 -B "$ROOT/tests/test_reference.py" -v
python3 -B "$ROOT/tests/test_fan_curve_probe.py" -v
python3 -B "$ROOT/tests/test_install_app.py" -v
"$ROOT/scripts/build.sh"

# The ownership report is intentionally fail-closed: a metadata-only probe
# cannot hold the broker lock, so exit 2 is the expected result. Validate the
# JSON contract without creating a runtime directory or touching hardware.
OWNERSHIP_PREFLIGHT="$ROOT/build/ownership-preflight.json"
set +e
"$ROOT/build/app/n14-control" --ownership-preflight > "$OWNERSHIP_PREFLIGHT"
PREFLIGHT_STATUS=$?
set -e
[ "$PREFLIGHT_STATUS" -eq 2 ]
python3 - "$OWNERSHIP_PREFLIGHT" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    report = json.load(stream)
assert report["schema"] == 1
assert report["mode"] == "read-only"
assert report["hardware_writes"] is False
assert report["lock_acquired"] is False
assert report["identity_authority"] == "pinned_injected_source_required"
assert report["decision"]["control_allowed"] is False
assert report["facts"]["identity"]["ec_version"] == ""
assert "query_accessible" in report["facts"]["lock_file"]
assert "dsdt_readable" in report["facts"]
PY

mkdir -p "$ROOT/build/tests"
cd "$ROOT/build/tests"
qmake6 "$ROOT/tests/tests.pro"
make -j2
./n14-tests
mkdir -p "$ROOT/build/service-tests"
cd "$ROOT/build/service-tests"
qmake6 "$ROOT/tests/service.pro"
make -j2
dbus-run-session -- sh -c 'export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"; exec "$1"' sh ./n14-service-tests
mkdir -p "$ROOT/build/lifecycle-probe"
cd "$ROOT/build/lifecycle-probe"
qmake6 "$ROOT/tests/lifecycle.pro"
make -j2
mkdir -p "$ROOT/build/load-probe"
cd "$ROOT/build/load-probe"
qmake6 "$ROOT/tests/load.pro"
make -j2
mkdir -p "$ROOT/build/hardware-tests"
cd "$ROOT/build/hardware-tests"
qmake6 "$ROOT/tests/hardware.pro"
make -j2
dbus-run-session -- sh -c 'export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"; exec "$1"' sh ./n14-hardware-tests
mkdir -p "$ROOT/build/gui-tests"
cd "$ROOT/build/gui-tests"
qmake6 "$ROOT/tests/gui.pro"
make -j2
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software LIBGL_ALWAYS_SOFTWARE=1 ./n14-gui-tests
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software LIBGL_ALWAYS_SOFTWARE=1 \
    "$ROOT/build/app/n14-control" --smoke-test "$ROOT/build/smoke.png"
mkdir -p "$ROOT/build/sensor-tests"
cd "$ROOT/build/sensor-tests"
qmake6 "$ROOT/tests/sensor.pro"
make -j2
./n14-sensor-tests