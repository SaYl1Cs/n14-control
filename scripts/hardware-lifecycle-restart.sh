#!/bin/sh
# Explicit opt-in companion for tests/lifecycle.pro. Run separately via Polkit.
# Fixed restart, RTC-armed logind suspend or passive load-test audit.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
trace=0
if [ "$#" -ge 1 ] && [ "$1" = --trace ]; then trace=1; shift; fi
action=restart
if [ "$#" -eq 2 ] && [ "$1" = --sleep ]; then action=sleep; shift; fi
if [ "$#" -eq 2 ] && [ "$1" = --load ]; then action=load; shift; fi
if [ "$#" -ne 1 ]; then
    printf '%s\n' 'Usage: hardware-lifecycle-restart.sh [--trace] [--sleep|--load] NEW_RECEIPT_NAME' >&2
    exit 2
fi
case "$1" in ''|*[!a-zA-Z0-9_-]*) printf '%s\n' 'Invalid receipt name' >&2; exit 2;; esac
if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' 'Run this reviewed restart/audit companion through Polkit as root.' >&2
    exit 2
fi
REPORT="$ROOT/build/$1.json"
test ! -e "$REPORT"
test ! -e "$REPORT.ready"
test -x "$ROOT/build/lifecycle-probe/n14-hardware-lifecycle-probe"
cmp "$ROOT/build/service/n14-hardware-service" /usr/local/libexec/n14-hardware-service
cmp "$ROOT/build/sensors/n14-sensors-service" /usr/local/libexec/n14-sensors-service
if pgrep -x 'n14-control|nbfc_service|coolercontrold' >/dev/null ||
   pgrep -f '^/[^ ]*/n14-fan-controller( |$)' >/dev/null; then
    printf '%s\n' 'Refusing restart validation while a GUI or competing controller is running.' >&2
    exit 2
fi
systemctl is-active --quiet n14-hardware.service
systemctl is-active --quiet n14-sensors.service
if [ "$action" = sleep ]; then
    test -c /dev/rtc0
    test "$(cat /sys/class/rtc/rtc0/device/power/wakeup)" = enabled
    test -z "$(cat /sys/class/rtc/rtc0/wakealarm)"
fi
old_pid=$(systemctl show n14-hardware.service --property=MainPID --value)
sensors_pid=$(systemctl show n14-sensors.service --property=MainPID --value)
AUDIT=$(mktemp -d /run/n14-lifecycle-audit-XXXXXXXX)
monitor_pid=
tracer_pid=
armed_alarm=
finish() {
    result=$?
    trap - EXIT HUP INT TERM
    set +e
    if [ -n "$tracer_pid" ]; then
        kill -INT "$tracer_pid" 2>/dev/null
        wait "$tracer_pid"
        printf '%s\n' ACPI_SYSCALL_TRACE_BEGIN
        cat "$AUDIT/acpi.strace"
        printf '%s\n' ACPI_SYSCALL_TRACE_END
    fi
    if [ -n "$armed_alarm" ] && [ "$(cat /sys/class/rtc/rtc0/wakealarm)" = "$armed_alarm" ]; then
        /usr/bin/rtcwake --device /dev/rtc0 --mode disable
    fi
    if [ -n "$monitor_pid" ]; then
        kill -TERM "$monitor_pid" 2>/dev/null
        wait "$monitor_pid"
    fi
    printf '%s\n' DBUS_AUDIT_JSON_BEGIN
    cat "$AUDIT/messages.jsonl"
    printf '%s\n' DBUS_AUDIT_JSON_END DBUS_AUDIT_STDERR_BEGIN
    cat "$AUDIT/monitor.stderr"
    printf '%s\n' DBUS_AUDIT_STDERR_END
    rm -rf -- "$AUDIT"
    exit "$result"
}
trap finish EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
/usr/bin/timeout --kill-after=2 170 /usr/bin/busctl --system --json=short \
    --match="type='method_call',interface='org.n14control.Hardware1'" \
    --match="type='signal',interface='org.freedesktop.login1.Manager',member='PrepareForSleep'" \
    monitor \
    > "$AUDIT/messages.jsonl" 2> "$AUDIT/monitor.stderr" &
monitor_pid=$!
ready=0
for attempt in $(seq 1 30); do
    kill -0 "$monitor_pid"
    # JSON mode has no text readiness banner. Require an actual harmless
    # Capabilities call to appear in the audit before allowing the EC probe.
    /usr/bin/busctl --system --timeout=3 call org.n14control.Hardware1 \
        /org/n14control/Hardware1 org.n14control.Hardware1 Capabilities > "$AUDIT/handshake.log"
    sleep 0.1
    if grep -Eq '"member"[[:space:]]*:[[:space:]]*"Capabilities"' "$AUDIT/messages.jsonl"; then ready=1; break; fi
done
test "$ready" -eq 1
if [ "$trace" -eq 1 ]; then
    test "$action" != restart
    test -x /usr/bin/strace
    /usr/bin/strace -ttt -yy -s 512 -e trace=openat,read,write,close \
        -P /proc/acpi/call -o "$AUDIT/acpi.strace" -p "$old_pid" \
        2> "$AUDIT/strace.stderr" &
    tracer_pid=$!
    sleep 0.3
    kill -0 "$tracer_pid"
    test "$(awk '/^TracerPid:/ {print $2}' "/proc/$old_pid/status")" = "$tracer_pid"
fi
printf '%s\n' AUDIT_READY
if [ "$action" = load ]; then
    ready=0
    for attempt in $(seq 1 160); do
        kill -0 "$monitor_pid"
        if [ -f "$REPORT" ]; then ready=1; break; fi
        sleep 1
    done
    test "$ready" -eq 1
    test "$(systemctl show n14-hardware.service --property=MainPID --value)" = "$old_pid"
    test "$(systemctl show n14-sensors.service --property=MainPID --value)" = "$sensors_pid"
    systemctl is-active --quiet n14-hardware.service
    systemctl is-active --quiet n14-sensors.service
    printf '%s\n' LOAD_AUDIT_COMPLETE
    exit 0
fi
ready=0
for attempt in $(seq 1 90); do
    kill -0 "$monitor_pid"
    if [ -e "$REPORT" ]; then
        printf '%s\n' 'Probe finished before reaching restart-ready; no restart performed.' >&2
        exit 1
    fi
    if [ -f "$REPORT.ready" ]; then ready=1; break; fi
    sleep 1
done
test "$ready" -eq 1
test ! -e "$REPORT"
grep -Eq '"action"[[:space:]]*:[[:space:]]*"'"$action"'"' "$REPORT.ready"
test "$(systemctl show n14-hardware.service --property=MainPID --value)" = "$old_pid"
if [ "$action" = sleep ]; then
    /usr/bin/rtcwake --device /dev/rtc0 --mode no --seconds 25
    armed_alarm=$(cat /sys/class/rtc/rtc0/wakealarm)
    case "$armed_alarm" in ''|*[!0-9]*) printf '%s\n' 'RTC wake alarm was not armed; suspend refused.' >&2; exit 1;; esac
    test "$armed_alarm" -gt "$(date +%s)"
    /usr/bin/rtcwake --device /dev/rtc0 --mode show
    printf 'SUSPEND_BEGIN hardware_pid=%s sensors_pid=%s rtc_alarm=%s\n' "$old_pid" "$sensors_pid" "$armed_alarm"
    /usr/bin/busctl --system call org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager Suspend b false
else
    printf 'RESTART_BEGIN old_hardware_pid=%s sensors_pid=%s\n' "$old_pid" "$sensors_pid"
    systemctl restart n14-hardware.service
fi
systemctl is-active --quiet n14-hardware.service
systemctl is-active --quiet n14-sensors.service
new_pid=$(systemctl show n14-hardware.service --property=MainPID --value)
if [ "$action" = sleep ]; then test "$new_pid" = "$old_pid"; else test "$new_pid" != "$old_pid"; fi
test "$(systemctl show n14-sensors.service --property=MainPID --value)" = "$sensors_pid"
printf 'RESTART_COMPLETE new_hardware_pid=%s sensors_pid=%s\n' "$new_pid" "$sensors_pid"
ready=0
for attempt in $(seq 1 60); do
    kill -0 "$monitor_pid"
    if [ -f "$REPORT" ]; then ready=1; break; fi
    sleep 1
done
test "$ready" -eq 1
sleep 0.5
printf '%s\n' RESTART_AUDIT_COMPLETE