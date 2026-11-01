#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# A local rebuild must not silently launch an older bundled direct-EC client.
if [ -x "$ROOT/build/app/n14-control" ]; then
    exec "$ROOT/build/app/n14-control" "$@"
fi
if [ -x "$ROOT/dist/n14-control-0.2.1/AppRun" ]; then
    exec "$ROOT/dist/n14-control-0.2.1/AppRun" "$@"
fi
printf '%s\n' 'Application is not built. Run scripts/build.sh first.' >&2
exit 1