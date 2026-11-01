#!/bin/sh
# Install the GUI and register its desktop launcher and icon.
# This does not install or restart the privileged hardware services.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

MODE=system
if [ "$#" -eq 1 ] && [ "$1" = '--user' ]; then
    MODE=user
elif [ "$#" -ne 0 ]; then
    printf '%s\n' 'Usage: install-app.sh [--user]' >&2
    exit 2
fi

if [ "$MODE" = system ]; then
    [ "$(id -u)" -eq 0 ] || {
        printf '%s\n' 'System installation requires root; use --user for a per-user installation.' >&2
        exit 2
    }
    PREFIX=/usr/local
    DATA_PREFIX=/usr/share
else
    PREFIX=${HOME:?}/.local
    DATA_PREFIX=$PREFIX/share
fi

if [ -x "$ROOT/build/app/n14-control" ]; then
    APP="$ROOT/build/app/n14-control"
    RESOURCE_ROOT="$ROOT/resources"
elif [ -x "$ROOT/../bin/n14-control" ]; then
    APP="$ROOT/../bin/n14-control"
    RESOURCE_ROOT="$ROOT/../source/resources"
else
    printf '%s\n' 'Application is not built. Run scripts/build.sh or use a release bundle.' >&2
    exit 1
fi

DESKTOP_TEMPLATE="$RESOURCE_ROOT/n14-control.desktop.in"
ICON="$RESOURCE_ROOT/n14-control.svg"
[ -f "$DESKTOP_TEMPLATE" ] && [ -f "$ICON" ]

BINDIR="$PREFIX/bin"
ICONDIR="$DATA_PREFIX/icons/hicolor/scalable/apps"
APPDIR="$DATA_PREFIX/applications"
DESKTOP="$APPDIR/n14-control.desktop"
TMP=$(mktemp "${TMPDIR:-/tmp}/n14-control-XXXXXX.desktop")
trap 'rm -f "$TMP"' EXIT HUP INT TERM
ESCAPED_BINDIR=$(printf '%s\n' "$BINDIR" | sed 's/[\\&|]/\\&/g')
sed "s|@BINDIR@|$ESCAPED_BINDIR|g" "$DESKTOP_TEMPLATE" > "$TMP"
desktop-file-validate "$TMP"

install -D -m755 "$APP" "$BINDIR/n14-control"
install -D -m644 "$ICON" "$ICONDIR/n14-control.svg"
install -D -m644 "$TMP" "$DESKTOP"
rm -f "$TMP"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$APPDIR" >/dev/null 2>&1 || true
fi
trap - EXIT HUP INT TERM
printf 'Installed N14 Control GUI: %s\n' "$BINDIR/n14-control"
printf 'Desktop launcher: %s\n' "$DESKTOP"