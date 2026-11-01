#!/bin/sh
# Explicit opt-in. This invokes the real application and performs EC writes.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REPORT=${1:-$ROOT/build/hardware-integration.json}
if pgrep -f '^/[^ ]*/n14-fan-controller( |$)' >/dev/null; then
    printf '%s\n' 'Refusing a second EC client while the headless curve controller is running. Use fan-curve-physical-test.py to observe it.' >&2
    exit 2
fi

test -x "$ROOT/build/app/n14-control"
test -x /usr/local/libexec/n14-hardware-service
test -x /usr/local/libexec/n14-sensors-service

if ! cmp -s "$ROOT/build/service/n14-hardware-service" /usr/local/libexec/n14-hardware-service; then
    printf '%s\n' 'Installed hardware service does not match build; install/restart n14-hardware.service first.' >&2
    exit 2
fi
if ! cmp -s "$ROOT/build/sensors/n14-sensors-service" /usr/local/libexec/n14-sensors-service; then
    printf '%s\n' 'Installed sensors service does not match build; install/restart n14-sensors.service first.' >&2
    exit 2
fi

mkdir -p "$(dirname "$REPORT")"
exec "$ROOT/build/app/n14-control" --hardware-test "$REPORT"