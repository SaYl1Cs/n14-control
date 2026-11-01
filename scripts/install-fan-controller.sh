#!/bin/sh
# Explicit opt-in: starts real curve control through the installed EC broker.
# The controller is a system unit running as the named unprivileged user so it
# is alive before the graphical login and receives login1 sleep/resume signals.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' 'Run this installer as root after reviewing service/ and building the project.' >&2
    exit 1
fi
REPLACE_LEGACY=0
if [ "$#" -eq 0 ]; then
    N14_USER=yaroslav
elif [ "$#" -eq 2 ] && [ "$1" = '--user' ]; then
    N14_USER=$2
elif [ "$#" -eq 1 ] && [ "$1" = '--replace-legacy' ]; then
    N14_USER=yaroslav
    REPLACE_LEGACY=1
else
    printf '%s\n' 'Usage: install-fan-controller.sh [--user yaroslav|--replace-legacy]' >&2
    exit 1
fi
case "$N14_USER" in ''|*[!a-zA-Z0-9_-]*|-*) printf '%s\n' 'Invalid login' >&2; exit 1;; esac
getent passwd "$N14_USER" >/dev/null
[ "$N14_USER" = yaroslav ] || { printf '%s\n' 'The reviewed pre-login unit is pinned to user yaroslav.' >&2; exit 1; }

CONFIG=$(getent passwd "$N14_USER" | cut -d: -f6)/.config/N14/n14-control/config.json
test -f "$CONFIG"
test -x "$ROOT/build/fan-controller/n14-fan-controller"
"$ROOT/build/fan-controller/n14-fan-controller" --config "$CONFIG" --check-config

for unit in coolercontrold.service nbfc_service.service colorful-n14-adapter.service \
            colorful-n14-profile-selector.service; do
    if systemctl is-active --quiet "$unit"; then
        if [ "$REPLACE_LEGACY" -eq 1 ] && [ "$unit" != nbfc_service.service ]; then
            continue
        fi
        printf 'Refusing concurrent EC controller: %s\n' "$unit" >&2
        exit 2
    fi
done
if pgrep -x n14-control >/dev/null; then
    printf '%s\n' 'Close the old GUI before installing the single-owner controller.' >&2
    exit 2
fi
systemctl is-active --quiet n14-hardware.service
systemctl is-active --quiet n14-sensors.service

if [ "$REPLACE_LEGACY" -eq 1 ]; then
    # This is the explicit migration path. NBFC is never stopped implicitly;
    # an active NBFC process always remains a hard refusal above.
    for unit in colorful-n14-profile-selector.service coolercontrold.service \
                colorful-n14-adapter.service colorful-n14-sensors.service \
                colorful-n14-nbfc-prepare.service colorful-n14-ec-readonly.service; do
        systemctl disable --now "$unit" 2>/dev/null || true
    done
    # The old stack's sleep hook talks to its removed adapter socket. Preserve
    # it outside system-sleep for rollback, but do not leave a failing hook that
    # races the native hardware-service lid reconciliation.
    LEGACY_HOOK=/usr/lib/systemd/system-sleep/90-colorful-n14-acpi
    if [ -f "$LEGACY_HOOK" ]; then
        install -d -m700 /var/lib/n14-control/legacy
        mv -f "$LEGACY_HOOK" /var/lib/n14-control/legacy/90-colorful-n14-acpi.disabled
    fi
    sleep 1
    for unit in colorful-n14-profile-selector.service coolercontrold.service \
                colorful-n14-adapter.service colorful-n14-sensors.service; do
        if systemctl is-active --quiet "$unit"; then
            printf 'Legacy EC controller remained active: %s\n' "$unit" >&2
            exit 2
        fi
    done
fi

# Remove only the previous N14 user unit. This does not stop any unrelated
# cooling service and is harmless when the user manager is not running yet.
systemctl --user --machine="$N14_USER@.host" disable --now n14-fan-controller.service 2>/dev/null || true

install -D -m755 "$ROOT/build/fan-controller/n14-fan-controller" /usr/local/libexec/n14-fan-controller
install -D -m644 "$ROOT/service/n14-fan-controller.service" /etc/systemd/system/n14-fan-controller.service
install -D -m644 "$ROOT/service/org.n14control.Fans1.conf" /etc/dbus-1/system.d/org.n14control.Fans1.conf

# Fan control is explicitly authorized for this one local account even before
# a graphical session exists. CPU changes remain active-session-only in the
# separate rule installed by install-service.sh.
install -d -m755 /etc/polkit-1/rules.d
RULE=$(mktemp /etc/polkit-1/rules.d/.n14-fan-automation-XXXXXX)
trap 'rm -f "$RULE"' EXIT HUP INT TERM
printf '%s\n' 'polkit.addRule(function(action, subject) {' \
    '    if (action.id === "org.n14control.modify-ec" && subject.user === "yaroslav")' \
    '        return polkit.Result.YES;' \
    '});' > "$RULE"
chmod 644 "$RULE"
mv -f "$RULE" /etc/polkit-1/rules.d/49-n14-fan-automation-yaroslav.rules
trap - EXIT HUP INT TERM

systemctl daemon-reload
busctl --system call org.freedesktop.DBus /org/freedesktop/DBus \
    org.freedesktop.DBus ReloadConfig
systemctl enable --now n14-fan-controller.service
systemctl is-active --quiet n14-fan-controller.service
printf '%s\n' 'Pre-login curve controller started for yaroslav. The root hardware broker remains the only EC writer.'