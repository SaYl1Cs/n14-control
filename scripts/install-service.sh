#!/bin/sh
# Explicit opt-in. Does not stop/replace any cooling service or apply CPU settings.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ALLOW_USER=
if [ "$#" -ne 0 ]; then
    if [ "$#" -ne 2 ] || [ "$1" != '--allow-active-user' ]; then
        printf '%s\n' 'Usage: install-service.sh [--allow-active-user LOGIN]' >&2; exit 1
    fi
    ALLOW_USER=$2
    case "$ALLOW_USER" in ''|*[!a-zA-Z0-9_-]*|-*) printf '%s\n' 'Invalid login' >&2; exit 1;; esac
    getent passwd "$ALLOW_USER" >/dev/null
fi
if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' 'Run this installer as root after reviewing service/ and building the project.' >&2
    exit 1
fi
test -x "$ROOT/build/service/n14-hardware-service"
test -x "$ROOT/build/sensors/n14-sensors-service"
install -D -m755 "$ROOT/build/service/n14-hardware-service" /usr/local/libexec/n14-hardware-service
install -D -m755 "$ROOT/build/sensors/n14-sensors-service" /usr/local/libexec/n14-sensors-service
install -D -m644 "$ROOT/service/org.n14control.Hardware1.conf" /etc/dbus-1/system.d/org.n14control.Hardware1.conf
install -D -m644 "$ROOT/service/org.n14control.policy" /usr/share/polkit-1/actions/org.n14control.policy
install -D -m644 "$ROOT/service/n14-hardware.service" /etc/systemd/system/n14-hardware.service
install -D -m644 "$ROOT/service/n14-sensors.service" /etc/systemd/system/n14-sensors.service
if [ -n "$ALLOW_USER" ]; then
    install -d -m755 /etc/polkit-1/rules.d
    RULE=$(mktemp /etc/polkit-1/rules.d/.n14-control-XXXXXX)
    trap 'rm -f "$RULE"' EXIT HUP INT TERM
        printf "polkit.addRule(function(action, subject) { if (action.id === 'org.n14control.modify-ec' && subject.user === '%s') return polkit.Result.YES; if (action.id === 'org.n14control.modify-cpu' && subject.local && subject.active && subject.user === '%s') return polkit.Result.YES; });\n" "$ALLOW_USER" "$ALLOW_USER" > "$RULE"
    chmod 644 "$RULE"
    mv -f "$RULE" /etc/polkit-1/rules.d/49-n14-control-cpu.rules
    trap - EXIT HUP INT TERM
fi
systemctl daemon-reload
busctl --system call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus ReloadConfig
systemctl enable n14-hardware.service
systemctl enable n14-sensors.service
systemctl restart n14-sensors.service
systemctl restart n14-hardware.service
printf '%s\n' 'Installed CPU/EC service. No EC writes or CPU settings were applied during installation.'