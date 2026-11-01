#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$ROOT/build/app"
cd "$ROOT/build/app"
qmake6 "$ROOT/n14-control.pro"
make -j2
mkdir -p "$ROOT/build/service"
cd "$ROOT/build/service"
qmake6 "$ROOT/service/service.pro"
make -j2
mkdir -p "$ROOT/build/sensors"
cd "$ROOT/build/sensors"
qmake6 "$ROOT/service/sensors.pro"
make -j2
mkdir -p "$ROOT/build/fan-controller"
cd "$ROOT/build/fan-controller"
qmake6 "$ROOT/service/fan_controller.pro"
make -j2
printf '\nBuilt: %s/build/app/n14-control\n' "$ROOT"