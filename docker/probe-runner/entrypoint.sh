#!/bin/sh
set -eu

if ulimit -S -c unlimited 2>/dev/null; then
    printf '[probe-runner] core soft limit: %s\n' "$(ulimit -S -c)" >&2
else
    printf '[probe-runner] warning: cannot enable unlimited core dumps (soft=%s hard=%s); run Docker with --ulimit core=-1\n' \
        "$(ulimit -S -c)" \
        "$(ulimit -H -c)" >&2
fi

exec "$@"
