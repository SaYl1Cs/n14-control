#!/bin/sh
# Explicit coordinated deployment, not an automatic recovery/restart loop.
# No EC register writes, firmware-mode restoration or load probes in this script.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ "$(id -u)" -eq 0 ] || { printf '%s\n' 'Root authorization is required.' >&2; exit 2; }
[ "$#" -eq 2 ] && [ "$1" = '--user' ] || { printf '%s\n' 'Usage: update-ec-stack.sh --user LOGIN' >&2; exit 2; }
LOGIN=$2
case "$LOGIN" in ''|*[!a-zA-Z0-9_-]*|-*) exit 2;; esac
UID_VALUE=$(id -u "$LOGIN")
[ "$UID_VALUE" -ne 0 ]
[ "$LOGIN" = yaroslav ]
USER_HOME=$(getent passwd "$LOGIN" | cut -d: -f6)
[ "$(cat "$ROOT/build/ec-stack-final-tests.exit")" = 0 ]
# Validate the exact binaries that completed the offline release run.
sha256sum --check --strict "$ROOT/build/ec-stack-verified.sha256"
"$ROOT/build/fan-controller/n14-fan-controller" \
    --config "$USER_HOME/.config/N14/n14-control/config.json" --check-config
for unit in coolercontrold.service nbfc_service.service colorful-n14-adapter.service; do
    if systemctl is-active --quiet "$unit"; then
        printf 'Refusing concurrent EC writer: %s\n' "$unit" >&2; exit 2
    fi
done
if pgrep -x n14-control >/dev/null || pgrep -f '^/[^ ]*/n14-(hardware-load-probe|hardware-lifecycle-probe|keylight-probe)( |$)' >/dev/null; then
    printf '%s\n' 'Close live GUI/write probes before coordinated deployment.' >&2; exit 2
fi
/usr/bin/busctl --system --json=short call org.n14control.Fans1 \
    /org/n14control/Fans1 org.n14control.Fans1 Status | /usr/bin/python3 -c '
import json,sys
state=json.loads(json.load(sys.stdin)["data"][0])
assert not state["busy"] and not state["sleeping"], "Controller operation/sleep in progress"
'
HARDWARE_STAGE=$(mktemp /usr/local/libexec/.n14-hardware-update-XXXXXX)
CONTROLLER_STAGE=$(mktemp /usr/local/libexec/.n14-controller-update-XXXXXX)
trap 'rm -f "$HARDWARE_STAGE" "$CONTROLLER_STAGE"' EXIT HUP INT TERM
install -m755 "$ROOT/build/service/n14-hardware-service" "$HARDWARE_STAGE"
install -m755 "$ROOT/build/fan-controller/n14-fan-controller" "$CONTROLLER_STAGE"
STAMP=$(date +%Y%m%dT%H%M%S)
cp -p /usr/local/libexec/n14-hardware-service "/usr/local/libexec/n14-hardware-service.before-$STAMP"
cp -p /usr/local/libexec/n14-fan-controller "/usr/local/libexec/n14-fan-controller.before-$STAMP"
printf '%s\n' 'Explicitly replacing the whole EC client/broker stack; no firmware automatic-mode restoration.'
systemctl stop n14-fan-controller.service
mv -f "$HARDWARE_STAGE" /usr/local/libexec/n14-hardware-service
mv -f "$CONTROLLER_STAGE" /usr/local/libexec/n14-fan-controller
systemctl restart n14-hardware.service
systemctl start n14-fan-controller.service
printf '%s\n' 'Updated broker and single-owner controller. Saved curve starts with fresh provenance; inspect Status before any lighting/load test.'